// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <lager/storage.hpp>
#include <lager/root_journal.hpp>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lager {

inline void throw_mapped_storage_errno(const char* operation)
{
    throw std::system_error{errno, std::generic_category(), operation};
}

/*!
 * One mapped extent and its explicit durability operations.
 *
 * The mapping owner is also copied into `view.owner`, so publishing the view
 * in an extent_directory keeps the mapping alive after this wrapper is gone.
 */
class mapped_extent
{
    struct mapping_owner
    {
        mapping_owner(std::shared_ptr<void> file_,
                      int fd_,
                      void* address_,
                      std::size_t bytes_,
                      std::size_t data_offset_,
                      std::size_t data_bytes_,
                      std::uint64_t file_offset_,
                      std::uint64_t slot_count_,
                      std::size_t slot_size_,
                      std::size_t slot_alignment_,
                      std::size_t cursor_offset_,
                      std::shared_ptr<std::mutex> allocation_mutex_)
            : file{std::move(file_)},
              fd{fd_},
              address{address_},
              bytes{bytes_},
              data_offset{data_offset_},
              data_bytes{data_bytes_},
              file_offset{file_offset_},
              slot_count{slot_count_},
              slot_size{slot_size_},
              slot_alignment{slot_alignment_},
              cursor_offset{cursor_offset_},
              allocation_mutex{std::move(allocation_mutex_)}
        {
            const auto* region = static_cast<const unsigned char*>(address);
            std::uint64_t cursor = 0;
            std::uint64_t cursor_check = 0;
            std::memcpy(&cursor, region + cursor_offset, sizeof(cursor));
            std::memcpy(&cursor_check,
                        region + cursor_offset + sizeof(cursor),
                        sizeof(cursor_check));
            const auto used = cursor_check == ~cursor && cursor <= slot_count
                                  ? static_cast<std::size_t>(cursor)
                                  : static_cast<std::size_t>(slot_count);
            slot_bitmap =
                std::make_shared<arena_slot_bitmap>(slot_count, used);
            known_highwater = used;
            local_next = used;
            local_end = used;
        }
        ~mapping_owner();

        void* allocate_slot(std::size_t size, std::size_t alignment);
        void refresh_allocation_state();
        void flush_data_mapping();
        void flush_data();

        std::shared_ptr<void> file;
        int fd;
        void* address;
        std::size_t bytes;
        std::size_t data_offset;
        std::size_t data_bytes;
        std::uint64_t file_offset;
        std::uint64_t slot_count;
        std::size_t slot_size;
        std::size_t slot_alignment;
        std::size_t cursor_offset;
        std::shared_ptr<std::mutex> allocation_mutex;
        std::shared_ptr<arena_slot_bitmap> slot_bitmap;
        std::size_t known_highwater = 0;
        std::size_t local_next = 0;
        std::size_t local_end = 0;
    };
    friend class persistent_storage_file;

public:
    mapped_extent() = default;

    const storage_extent& view() const noexcept { return view_; }

    void flush() const
    {
        if (!mapping_)
            throw std::logic_error{"mapped extent is empty"};
        flush_mapping();
        sync_file();
    }

    void flush_data() const
    {
        if (!mapping_)
            throw std::logic_error{"mapped extent is empty"};
        flush_data_mapping();
        sync_file();
    }

private:
    std::shared_ptr<mapping_owner> mapping_;
    storage_extent view_;

    void flush_mapping() const;
    void flush_data_mapping() const;
    void sync_file() const;
};

/*!
 * Append-only persistent extent file.
 *
 * This POSIX backend reserves metadata pages at the start of each extent and
 * appends new mappings without moving existing mappings. Calls to `flush` or
 * `flush_data` make node bytes durable; Lager root publication must happen
 * after those calls. Extent discovery and layout validation run again on each
 * open. Windows mapping support is a separate backend and is not implemented
 * by this class yet.
 */
class persistent_storage_file
{
    struct file_state;
    struct disk_superblock
    {
        char magic[8];
        std::uint32_t version;
        std::uint32_t page_bytes;
        std::uint64_t checksum;
    };

    struct disk_extent_header
    {
        char magic[8];
        std::uint32_t version;
        std::uint32_t id;
        std::uint64_t region_bytes;
        std::uint64_t data_offset;
        std::uint64_t data_bytes;
        std::uint64_t slot_size;
        std::uint64_t slot_alignment;
        std::uint64_t layout_id;
        std::uint64_t slot_count;
        std::uint64_t checksum;
        std::uint64_t next_slot;
        std::uint64_t next_slot_check;
    };
    static_assert(sizeof(disk_superblock) == 24,
                  "persistent superblock layout changed");
    static_assert(sizeof(disk_extent_header) == 96,
                  "persistent extent header layout changed");

    struct extent_record
    {
        disk_extent_header header;
        std::uint64_t file_offset;
    };

    static constexpr std::uint32_t format_version = 2;
    static constexpr char superblock_magic[8] = {'L', 'A', 'G', 'E', 'R',
                                                  'S', 'T', '\0'};
    static constexpr char extent_magic[8] = {'L', 'A', 'G', 'E', 'R',
                                              'E', 'X', '\0'};

public:
    persistent_storage_file() = default;

    static persistent_storage_file open(const std::string& path)
    {
#if defined(_WIN32)
        (void) path;
        throw std::runtime_error{
            "persistent_storage_file currently requires a POSIX platform"};
#else
        auto result = persistent_storage_file{};
        result.state_ = std::make_shared<file_state>(path);
        result.initialize();
        return result;
#endif
    }

    explicit operator bool() const noexcept { return bool{state_}; }

    std::vector<mapped_extent> map_existing_extents() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "persistent_storage_file currently requires a POSIX platform"};
#else
        require_open();
        file_lock lock{state_->fd, LOCK_SH};
        std::vector<mapped_extent> result;
        for (const auto& record : scan_records())
            result.push_back(map_record(record));
        return result;
#endif
    }

    persistent_root_journal roots() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "persistent_storage_file currently requires a POSIX platform"};
#else
        require_open();
        return persistent_root_journal::open(state_->path + ".roots");
#endif
    }

    std::vector<durable_root_descriptor> recover_roots() const
    {
        return roots().recover();
    }

    mapped_extent append_extent(std::size_t slot_size,
                                std::size_t slot_alignment,
                                std::uint64_t layout_id,
                                std::uint32_t minimum_slots)
    {
#if defined(_WIN32)
        (void) slot_size;
        (void) slot_alignment;
        (void) layout_id;
        (void) minimum_slots;
        throw std::runtime_error{
            "persistent_storage_file currently requires a POSIX platform"};
#else
        require_open();
        if (slot_size == 0 || slot_alignment == 0 || layout_id == 0 ||
            minimum_slots == 0 || slot_size % slot_alignment != 0)
            throw std::invalid_argument{"invalid persistent extent layout"};
        if (slot_alignment > state_->page_bytes ||
            state_->page_bytes % slot_alignment != 0)
            throw std::invalid_argument{
                "extent slot alignment must divide the system page size"};

        std::lock_guard<std::mutex> thread_lock{state_->append_mutex};
        file_lock process_lock{state_->fd, LOCK_EX};
        const auto records = scan_records();
        const auto valid_end = records.empty()
                                   ? static_cast<std::uint64_t>(state_->page_bytes)
                                   : records.back().file_offset +
                                         records.back().header.region_bytes;
        if (file_size() != valid_end &&
            ::ftruncate(state_->fd, static_cast<off_t>(valid_end)) != 0)
            throw_mapped_storage_errno("truncate incomplete extent tail");

        const auto previous_id =
            records.empty() ? 0 : records.back().header.id;
        if (previous_id == std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error{"persistent extent IDs are exhausted"};

        const auto requested_bytes = checked_multiply(slot_size, minimum_slots);
        const auto rounded_data_bytes = round_up(requested_bytes,
                                                 state_->page_bytes);
        const auto actual_slots = rounded_data_bytes / slot_size;
        if (actual_slots == 0 ||
            actual_slots >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()) +
                    1)
            throw std::overflow_error{"persistent extent slot count is invalid"};
        const auto data_bytes = checked_multiply(slot_size, actual_slots);
        const auto mapped_data_bytes = round_up(data_bytes,
                                                state_->page_bytes);
        const auto region_bytes = checked_add(state_->page_bytes,
                                              mapped_data_bytes);
        const auto end_offset = checked_add(valid_end, region_bytes);
        if (end_offset > static_cast<std::uint64_t>(
                             std::numeric_limits<off_t>::max()))
            throw std::overflow_error{"persistent storage file is too large"};

        if (::ftruncate(state_->fd, static_cast<off_t>(end_offset)) != 0)
            throw_mapped_storage_errno("grow persistent storage file");
        extent_record record{};
        auto& header = record.header;
        std::memcpy(header.magic, extent_magic, sizeof(extent_magic));
        header.version = format_version;
        header.id = previous_id + 1;
        header.region_bytes = region_bytes;
        header.data_offset = state_->page_bytes;
        header.data_bytes = data_bytes;
        header.slot_size = slot_size;
        header.slot_alignment = slot_alignment;
        header.layout_id = layout_id;
        header.slot_count = actual_slots;
        header.next_slot = 0;
        header.next_slot_check = ~std::uint64_t{0};
        header.checksum = checksum(&header,
                                   offsetof(disk_extent_header, checksum));
        record.file_offset = valid_end;
        std::memset(header.magic, 0, sizeof(header.magic));
        write_exact(state_->fd,
                    &header,
                    sizeof(header),
                    static_cast<off_t>(valid_end));
        sync_file_descriptor();
        write_exact(state_->fd,
                    extent_magic,
                    sizeof(extent_magic),
                    static_cast<off_t>(valid_end));
        sync_file_descriptor();
        std::memcpy(header.magic, extent_magic, sizeof(extent_magic));
        auto mapped = map_record(record);
        return mapped;
#endif
    }

private:
#if !defined(_WIN32)
    struct file_state
    {
        explicit file_state(const std::string& path_)
            : path{path_},
              fd{::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)}
        {
            if (fd < 0)
                throw_mapped_storage_errno("open persistent storage file");
        }
        ~file_state()
        {
            if (fd >= 0)
                ::close(fd);
        }

        std::string path;
        int fd;
        std::size_t page_bytes = 0;
        std::mutex append_mutex;
        std::shared_ptr<std::mutex> allocation_mutex =
            std::make_shared<std::mutex>();
    };

    class file_lock
    {
    public:
        file_lock(int fd, int operation) : fd_{fd}
        {
            while (::flock(fd_, operation) != 0) {
                if (errno != EINTR)
                    throw_mapped_storage_errno("lock persistent storage file");
            }
        }
        ~file_lock() { ::flock(fd_, LOCK_UN); }
        file_lock(const file_lock&) = delete;
        file_lock& operator=(const file_lock&) = delete;

    private:
        int fd_;
    };

    void initialize()
    {
        const auto page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0)
            throw_mapped_storage_errno("query system page size");
        if (static_cast<std::uint64_t>(page) >
            std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error{"system page size is unsupported"};

        file_lock lock{state_->fd, LOCK_EX};
        state_->page_bytes = static_cast<std::size_t>(page);
        if (std::endian::native != std::endian::little)
            throw std::runtime_error{
                "persistent storage requires a little-endian target"};
        const auto size = file_size();
        if (size == 0) {
            if (::ftruncate(state_->fd, static_cast<off_t>(page)) != 0)
                throw_mapped_storage_errno("initialize persistent storage file");
            write_superblock();
            return;
        }
        if (size < sizeof(disk_superblock))
            throw std::runtime_error{"persistent storage superblock is truncated"};
        disk_superblock block{};
        read_exact(state_->fd, &block, sizeof(block), 0);
        if (size == state_->page_bytes &&
            std::all_of(std::begin(block.magic),
                        std::end(block.magic),
                        [](char c) { return c == 0; })) {
            write_superblock();
            return;
        }
        if (std::memcmp(block.magic,
                        superblock_magic,
                        sizeof(superblock_magic)) != 0 ||
            block.version != format_version ||
            block.checksum != checksum(
                &block, offsetof(disk_superblock, checksum)))
            throw std::runtime_error{"persistent storage superblock is invalid"};
        if (block.page_bytes != state_->page_bytes ||
            size < block.page_bytes || size % block.page_bytes != 0)
            throw std::runtime_error{
                "persistent storage page size is incompatible"};
    }

    std::vector<extent_record> scan_records() const
    {
        std::vector<extent_record> records;
        const auto end = file_size();
        auto offset = static_cast<std::uint64_t>(state_->page_bytes);
        while (offset < end) {
            if (end - offset < sizeof(disk_extent_header))
                break;
            extent_record record{};
            read_exact(state_->fd,
                       &record.header,
                       sizeof(record.header),
                       static_cast<off_t>(offset));
            const auto& header = record.header;
            const auto empty_magic =
                std::all_of(std::begin(header.magic),
                            std::end(header.magic),
                            [](char c) { return c == 0; });
            if (empty_magic)
                break;
            if (std::memcmp(header.magic,
                            extent_magic,
                            sizeof(extent_magic)) != 0) {
                // Extent bytes are reserved before the header's commit
                // marker is written. An incomplete marker is recoverable
                // only when this is still the zero-filled append tail.
                const auto tail_offset = checked_add(
                    offset, sizeof(disk_extent_header));
                if (zero_range(tail_offset, end - tail_offset))
                    break;
                throw std::runtime_error{
                    "persistent storage extent commit marker is invalid"};
            }
            const auto expected_data_bytes =
                checked_multiply(header.slot_size, header.slot_count);
            const auto expected_region_bytes = checked_add(
                header.data_offset,
                round_up(header.data_bytes, state_->page_bytes));
            if (header.version != format_version || header.id == 0 ||
                header.checksum != checksum(
                    &header, offsetof(disk_extent_header, checksum)) ||
                header.data_offset != state_->page_bytes ||
                header.region_bytes < header.data_offset ||
                header.region_bytes % state_->page_bytes != 0 ||
                header.slot_size == 0 || header.slot_alignment == 0 ||
                header.slot_size % header.slot_alignment != 0 ||
                state_->page_bytes % header.slot_alignment != 0 ||
                header.layout_id == 0 || header.slot_count == 0 ||
                header.slot_count >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::uint32_t>::max()) +
                        1 ||
                header.data_bytes != expected_data_bytes ||
                header.region_bytes != expected_region_bytes ||
                header.region_bytes >
                    std::numeric_limits<std::size_t>::max() ||
                (records.size() != 0 &&
                 header.id <= records.back().header.id))
                throw std::runtime_error{
                    "persistent storage extent header is invalid"};
            if (header.region_bytes > end - offset)
                break;
            record.file_offset = offset;
            records.push_back(record);
            offset += header.region_bytes;
        }
        return records;
    }

    mapped_extent map_record(const extent_record& record) const
    {
        const auto bytes = static_cast<std::size_t>(record.header.region_bytes);
        auto* address = ::mmap(nullptr,
                               bytes,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               state_->fd,
                               static_cast<off_t>(record.file_offset));
        if (address == MAP_FAILED)
            throw_mapped_storage_errno("map persistent storage extent");
        auto owner = std::make_shared<mapped_extent::mapping_owner>(
            state_,
            state_->fd,
            address,
            bytes,
            static_cast<std::size_t>(record.header.data_offset),
            static_cast<std::size_t>(record.header.data_bytes),
            record.file_offset,
            record.header.slot_count,
            static_cast<std::size_t>(record.header.slot_size),
            static_cast<std::size_t>(record.header.slot_alignment),
            offsetof(disk_extent_header, next_slot),
            state_->allocation_mutex);
        mapped_extent result;
        result.mapping_ = owner;
        result.view_ = storage_extent{
            record.header.id,
            static_cast<std::byte*>(address) + record.header.data_offset,
            static_cast<std::size_t>(record.header.data_bytes),
            static_cast<std::size_t>(record.header.slot_size),
            static_cast<std::size_t>(record.header.slot_alignment),
            record.header.layout_id,
            std::move(owner)};
        auto allocation_owner = result.mapping_;
        result.view_.slot_bitmap = allocation_owner->slot_bitmap;
        result.view_.can_reclaim_slots = false;
        result.view_.allocate_slot =
            [allocation_owner](std::size_t size, std::size_t alignment) {
                return allocation_owner->allocate_slot(size, alignment);
            };
        result.view_.refresh_allocation_state = [allocation_owner] {
            allocation_owner->refresh_allocation_state();
        };
        result.view_.flush_data = [allocation_owner] {
            allocation_owner->flush_data();
        };
        return result;
    }

    std::uint64_t file_size() const
    {
        struct stat status
        {};
        if (::fstat(state_->fd, &status) != 0)
            throw_mapped_storage_errno("stat persistent storage file");
        if (status.st_size < 0)
            throw std::runtime_error{"persistent storage file has invalid size"};
        return static_cast<std::uint64_t>(status.st_size);
    }

    static std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs)
    {
        if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
            throw std::overflow_error{"persistent extent size overflow"};
        return lhs + rhs;
    }

    static std::uint64_t checked_multiply(std::uint64_t lhs,
                                          std::uint64_t rhs)
    {
        if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
            throw std::overflow_error{"persistent extent size overflow"};
        return lhs * rhs;
    }

    static std::uint64_t round_up(std::uint64_t value, std::uint64_t multiple)
    {
        if (multiple == 0)
            throw std::invalid_argument{"rounding multiple must be nonzero"};
        const auto remainder = value % multiple;
        return remainder == 0 ? value : checked_add(value, multiple - remainder);
    }

    static std::uint64_t checksum(const void* data, std::size_t size) noexcept
    {
        auto result = std::uint64_t{14695981039346656037ull};
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            result ^= bytes[i];
            result *= 1099511628211ull;
        }
        return result;
    }

    bool zero_range(std::uint64_t offset, std::uint64_t size) const
    {
        unsigned char bytes[4096];
        while (size != 0) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(size, sizeof(bytes)));
            read_exact(state_->fd, bytes, count, static_cast<off_t>(offset));
            if (!std::all_of(bytes, bytes + count, [](unsigned char value) {
                    return value == 0;
                }))
                return false;
            offset += count;
            size -= count;
        }
        return true;
    }

    void write_superblock()
    {
        disk_superblock block{};
        std::memcpy(block.magic, superblock_magic, sizeof(superblock_magic));
        block.version = format_version;
        block.page_bytes = static_cast<std::uint32_t>(state_->page_bytes);
        block.checksum = checksum(&block,
                                  offsetof(disk_superblock, checksum));
        std::memset(block.magic, 0, sizeof(block.magic));
        write_exact(state_->fd, &block, sizeof(block), 0);
        sync_file_descriptor();
        write_exact(state_->fd, superblock_magic, sizeof(superblock_magic), 0);
        sync_file_descriptor();
    }

    static void read_exact(int fd, void* data, std::size_t size, off_t offset)
    {
        auto* bytes = static_cast<unsigned char*>(data);
        std::size_t completed = 0;
        while (completed < size) {
            const auto count = ::pread(fd,
                                      bytes + completed,
                                      size - completed,
                                      offset + static_cast<off_t>(completed));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                throw_mapped_storage_errno("read persistent storage file");
            if (count == 0)
                throw std::runtime_error{
                    "persistent storage file ended during a metadata read"};
            completed += static_cast<std::size_t>(count);
        }
    }

    static void write_exact(int fd,
                            const void* data,
                            std::size_t size,
                            off_t offset)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t completed = 0;
        while (completed < size) {
            const auto count = ::pwrite(fd,
                                       bytes + completed,
                                       size - completed,
                                       offset + static_cast<off_t>(completed));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                throw_mapped_storage_errno("write persistent storage file");
            if (count == 0)
                throw std::runtime_error{
                    "persistent storage file accepted a zero-byte write"};
            completed += static_cast<std::size_t>(count);
        }
    }

    void sync_file_descriptor() const
    {
        if (::fdatasync(state_->fd) != 0)
            throw_mapped_storage_errno("flush persistent storage metadata");
    }

    void require_open() const
    {
        if (!state_)
            throw std::logic_error{"persistent storage file is not open"};
    }

    std::shared_ptr<file_state> state_;
#else
    struct file_state
    {};
#endif
};

#if !defined(_WIN32)
inline void mapped_extent::mapping_owner::refresh_allocation_state()
{
    std::lock_guard<std::mutex> thread_lock{*allocation_mutex};
    while (::flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR)
            throw_mapped_storage_errno("lock persistent extent allocator");
    }
    struct unlock_file
    {
        explicit unlock_file(int fd_) : fd{fd_} {}
        ~unlock_file() { ::flock(fd, LOCK_UN); }
        int fd;
    } unlock{fd};

    auto* region = static_cast<unsigned char*>(address);
    std::uint64_t cursor = 0;
    std::uint64_t cursor_check = 0;
    std::memcpy(&cursor, region + cursor_offset, sizeof(cursor));
    std::memcpy(&cursor_check,
                region + cursor_offset + sizeof(cursor),
                sizeof(cursor_check));
    if (cursor_check != ~cursor || cursor > slot_count)
        throw std::runtime_error{
            "persistent extent allocation cursor is invalid during GC"};

    if (cursor > known_highwater) {
        slot_bitmap->reserve_range(known_highwater,
                                   static_cast<std::size_t>(cursor));
        known_highwater = static_cast<std::size_t>(cursor);
    }
}

inline void* mapped_extent::mapping_owner::allocate_slot(
    std::size_t requested_size, std::size_t requested_alignment)
{
    if (requested_size > slot_size || requested_alignment > slot_alignment)
        return nullptr;
    std::lock_guard<std::mutex> thread_lock{*allocation_mutex};

    constexpr std::size_t reservation_size = 64;
    auto* region = static_cast<unsigned char*>(address);
    if (local_next < local_end) {
        const auto slot = local_next++;
        slot_bitmap->reserve(slot);
        return region + data_offset + slot * slot_size;
    }

    while (::flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR)
            throw_mapped_storage_errno("lock persistent extent allocator");
    }
    struct unlock_file
    {
        explicit unlock_file(int fd_) : fd{fd_} {}
        ~unlock_file() { ::flock(fd, LOCK_UN); }
        int fd;
    } unlock{fd};

    auto* cursor_address = region + cursor_offset;
    std::uint64_t cursor = 0;
    std::uint64_t cursor_check = 0;
    std::memcpy(&cursor, cursor_address, sizeof(cursor));
    std::memcpy(&cursor_check,
                cursor_address + sizeof(cursor),
                sizeof(cursor_check));
    if (cursor_check != ~cursor || cursor > slot_count)
        return nullptr;
    if (cursor > known_highwater) {
        slot_bitmap->reserve_range(known_highwater,
                                   static_cast<std::size_t>(cursor));
        known_highwater = static_cast<std::size_t>(cursor);
    }
    if (cursor == slot_count)
        return nullptr;

    const auto remaining = slot_count - cursor;
    const auto reservation = std::min<std::uint64_t>(reservation_size,
                                                      remaining);
    const auto next = cursor + reservation;
    const auto next_check = ~next;
    // Reserve a small range in one process-safe metadata update. A torn
    // update is treated as a full arena on recovery rather than risking
    // reuse of a slot which may contain committed data.
    std::memcpy(cursor_address + sizeof(cursor),
                &next_check,
                sizeof(next_check));
    std::memcpy(cursor_address, &next, sizeof(next));
    const auto first = static_cast<std::size_t>(cursor);
    local_next = first + 1;
    local_end = static_cast<std::size_t>(next);
    slot_bitmap->reserve_range(first, local_end);
    known_highwater = local_end;
    return region + data_offset + first * slot_size;
}

inline void mapped_extent::mapping_owner::flush_data()
{
    flush_data_mapping();
    if (::fdatasync(fd) != 0)
        throw_mapped_storage_errno("flush persistent storage file");
}

inline void mapped_extent::mapping_owner::flush_data_mapping()
{
    if (::msync(address, data_offset, MS_SYNC) != 0)
        throw_mapped_storage_errno("flush extent allocator metadata");
    auto* data = static_cast<std::byte*>(address) + data_offset;
    if (::msync(data, data_bytes, MS_SYNC) != 0)
        throw_mapped_storage_errno("flush mapped extent data");
}

inline mapped_extent::mapping_owner::~mapping_owner()
{
    if (address != MAP_FAILED)
        ::munmap(address, bytes);
}

inline void mapped_extent::flush_mapping() const
{
    if (::msync(mapping_->address, mapping_->bytes, MS_SYNC) != 0)
        throw_mapped_storage_errno("flush mapped extent");
}

inline void mapped_extent::flush_data_mapping() const
{
    mapping_->flush_data_mapping();
}

inline void mapped_extent::sync_file() const
{
    if (::fdatasync(mapping_->fd) != 0)
        throw_mapped_storage_errno("flush persistent storage file");
}
#else
inline mapped_extent::mapping_owner::~mapping_owner() = default;
inline void mapped_extent::flush_mapping() const
{
    throw std::runtime_error{"POSIX mapped storage is unavailable"};
}
inline void mapped_extent::flush_data_mapping() const
{
    throw std::runtime_error{"POSIX mapped storage is unavailable"};
}
inline void mapped_extent::sync_file() const
{
    throw std::runtime_error{"POSIX mapped storage is unavailable"};
}
#endif

} // namespace lager
