// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <immer/storage_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lager {

inline void throw_root_journal_errno(const char* operation)
{
    throw std::system_error{errno, std::generic_category(), operation};
}

struct durable_root_descriptor
{
    std::uint64_t store_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t read_watermark = 0;
    immer::storage_ptr<void> root;
    std::uint64_t layout_id = 0;
};

struct durable_read_dependency
{
    std::uint64_t store_id = 0;
    std::uint64_t generation = 0;
};

class durable_root_conflict : public std::runtime_error
{
public:
    durable_root_conflict()
        : std::runtime_error{"persistent substore root changed before commit"}
    {}
};

/*!
 * Append-only durable publication log for persistent substore roots.
 *
 * Callers must flush every extent reachable from a candidate root before
 * publishing its descriptor. `recover()` returns the highest committed root
 * for each substore. The append file is a separate metadata stream so a
 * torn final record cannot damage previously committed roots.
 */
class persistent_root_journal
{
    struct journal_state;
    struct disk_superblock
    {
        char magic[8];
        std::uint32_t version;
        std::uint32_t page_bytes;
        std::uint64_t checksum;
    };
    struct disk_root_record
    {
        char magic[8];
        std::uint32_t version;
        std::uint32_t reserved;
        std::uint64_t store_id;
        std::uint64_t generation;
        std::uint64_t read_watermark;
        std::uint64_t root_bits;
        std::uint64_t layout_id;
        std::uint64_t checksum;
    };
    struct root_bank
    {
        std::atomic<std::uint64_t> generation{0};
        std::atomic<std::uint64_t> read_watermark{0};
        std::atomic<std::uint64_t> root_bits{0};
        std::atomic<std::uint64_t> layout_id{0};
    };
    struct root_slot
    {
        std::atomic<std::uint64_t> store_id{0};
        std::atomic<std::uint64_t> sequence{0};
        root_bank banks[2];
    };
    struct shared_root_table
    {
        std::uint64_t magic = 0;
        std::atomic<std::uint64_t> global_generation{0};
        root_slot roots[32];
    };
    static_assert(sizeof(disk_superblock) == 24,
                  "root journal superblock layout changed");
    static_assert(sizeof(disk_root_record) == 64,
                  "root journal record layout changed");

    static_assert(64 + sizeof(shared_root_table) <= 4096,
                  "persistent root cache must fit in the journal header page");
    static constexpr std::uint32_t format_version = 2;
    static constexpr std::size_t root_table_offset = 64;
    static constexpr std::uint64_t root_table_magic = 0x4c41474552544332ULL;
    static constexpr char superblock_magic[8] = {'L', 'A', 'G', 'E', 'R',
                                                  'R', 'O', '\0'};
    static constexpr char record_magic[8] = {'L', 'A', 'G', 'E', 'R',
                                              'R', 'T', '\0'};

public:
    persistent_root_journal() = default;

    static persistent_root_journal open(const std::string& path)
    {
#if defined(_WIN32)
        (void) path;
        throw std::runtime_error{
            "persistent_root_journal currently requires a POSIX platform"};
#else
        persistent_root_journal result;
        result.state_ = std::make_shared<journal_state>(path);
        result.initialize();
        return result;
#endif
    }

    std::vector<durable_root_descriptor> recover() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "persistent_root_journal currently requires a POSIX platform"};
#else
        require_open();
        file_lock lock{state_->fd, LOCK_SH};
        const auto records = scan();
        std::map<std::uint64_t, durable_root_descriptor> latest;
        for (const auto& record : records) {
            const auto& d = record.descriptor;
            const auto found = latest.find(d.store_id);
            if (found == latest.end() ||
                d.generation > found->second.generation)
                latest[d.store_id] = d;
        }
        std::vector<durable_root_descriptor> result;
        result.reserve(latest.size());
        for (auto& item : latest)
            result.push_back(item.second);
        return result;
#endif
    }

    //! Lock-free read of the process-shared root cache populated only after
    //! its corresponding durable journal record has been committed.
    durable_root_descriptor read_root(std::uint64_t store_id) const
    {
#if defined(_WIN32)
        (void) store_id;
        throw std::runtime_error{
            "persistent_root_journal currently requires a POSIX platform"};
#else
        require_open();
        if (store_id == 0)
            throw std::invalid_argument{
                "persistent root store ID must be nonzero"};
        const auto* table = root_table();
        for (const auto& slot : table->roots) {
            if (slot.store_id.load(std::memory_order_acquire) != store_id)
                continue;
            for (;;) {
                const auto sequence =
                    slot.sequence.load(std::memory_order_acquire);
                if (sequence == 0)
                    throw std::out_of_range{
                        "persistent root is not published"};
                const auto& bank = slot.banks[sequence & 1u];
                const auto generation =
                    bank.generation.load(std::memory_order_relaxed);
                const auto watermark =
                    bank.read_watermark.load(std::memory_order_relaxed);
                const auto root_bits =
                    bank.root_bits.load(std::memory_order_relaxed);
                const auto layout_id =
                    bank.layout_id.load(std::memory_order_relaxed);
                if (slot.sequence.load(std::memory_order_acquire) != sequence)
                    continue;
                if (generation == 0 || root_bits == 0 || layout_id == 0 ||
                    watermark >= generation)
                    throw std::runtime_error{
                        "persistent root cache descriptor is invalid"};
                return {store_id,
                        generation,
                        watermark,
                        immer::storage_ptr<void>::from_bits(root_bits),
                        layout_id};
            }
        }
        throw std::out_of_range{"persistent root is not registered"};
#endif
    }

    std::uint64_t global_generation() const noexcept;

    std::uint64_t last_generation() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "persistent_root_journal currently requires a POSIX platform"};
#else
        require_open();
        file_lock lock{state_->fd, LOCK_SH};
        const auto records = scan();
        return records.empty() ? 0 : records.back().descriptor.generation;
#endif
    }

    durable_root_descriptor publish_next(durable_root_descriptor descriptor,
                                          std::uint64_t minimum_generation,
                                          std::uint64_t expected_store_generation,
                                          const std::vector<
                                              durable_read_dependency>& reads = {})
    {
#if defined(_WIN32)
        (void) descriptor;
        (void) minimum_generation;
        (void) expected_store_generation;
        (void) reads;
        throw std::runtime_error{
            "persistent_root_journal currently requires a POSIX platform"};
#else
        require_open();
        if (descriptor.store_id == 0 || !descriptor.root ||
            descriptor.root.extent_id() == 0 ||
            descriptor.layout_id == 0)
            throw std::invalid_argument{"durable root descriptor is invalid"};
        std::lock_guard<std::mutex> thread_lock{state_->append_mutex};
        file_lock process_lock{state_->fd, LOCK_EX};
        const auto existing = scan();
        const auto last = existing.empty()
                              ? 0
                              : existing.back().descriptor.generation;
        std::uint64_t current_store_generation = 0;
        for (auto iter = existing.rbegin(); iter != existing.rend(); ++iter) {
            if (iter->descriptor.store_id == descriptor.store_id) {
                current_store_generation = iter->descriptor.generation;
                break;
            }
        }
        if (current_store_generation != expected_store_generation)
            throw durable_root_conflict{};
        for (const auto& dependency : reads) {
            std::uint64_t current_generation = 0;
            for (auto iter = existing.rbegin(); iter != existing.rend(); ++iter) {
                if (iter->descriptor.store_id == dependency.store_id) {
                    current_generation = iter->descriptor.generation;
                    break;
                }
            }
            if (current_generation != dependency.generation)
                throw durable_root_conflict{};
        }
        const auto floor = std::max(last, minimum_generation);
        if (floor == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error{"durable root generations are exhausted"};
        descriptor.generation = floor + 1;
        if (descriptor.read_watermark >= descriptor.generation)
            throw std::invalid_argument{
                "durable root watermark must precede its generation"};
        auto* slot = find_root_slot(descriptor.store_id);
        if (slot == nullptr)
            throw std::length_error{
                "persistent root cache has reached its 32-store capacity"};
        append_locked(descriptor, existing.size());
        publish_root_cache(*slot, descriptor);
        test_failpoint("publication");
        return descriptor;
#endif
    }

private:
#if !defined(_WIN32)
    struct scanned_record
    {
        durable_root_descriptor descriptor;
    };

    struct journal_state
    {
        explicit journal_state(const std::string& path_)
            : path{path_},
              fd{::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)}
        {
            if (fd < 0)
                throw_root_journal_errno("open persistent root journal");
        }
        ~journal_state()
        {
#if !defined(_WIN32)
            if (control_mapping != MAP_FAILED)
                ::munmap(control_mapping, control_mapping_bytes);
#endif
            if (fd >= 0)
                ::close(fd);
        }

        std::string path;
        int fd;
        std::size_t page_bytes = 0;
        void* control_mapping = MAP_FAILED;
        std::size_t control_mapping_bytes = 0;
        std::mutex append_mutex;
    };

    class file_lock
    {
    public:
        file_lock(int fd, int operation) : fd_{fd}
        {
            while (::flock(fd_, operation) != 0) {
                if (errno != EINTR)
                    throw_root_journal_errno("lock persistent root journal");
            }
        }
        ~file_lock() { ::flock(fd_, LOCK_UN); }
        file_lock(const file_lock&) = delete;
        file_lock& operator=(const file_lock&) = delete;

    private:
        int fd_;
    };

    shared_root_table* root_table() const
    {
        if (state_->control_mapping == MAP_FAILED)
            throw std::logic_error{"persistent root cache is not mapped"};
        return reinterpret_cast<shared_root_table*>(
            static_cast<std::byte*>(state_->control_mapping) +
            root_table_offset);
    }

    void initialize_root_table()
    {
        if (state_->page_bytes < root_table_offset +
                                     sizeof(shared_root_table))
            throw std::runtime_error{
                "root journal page is too small for the shared root cache"};
        state_->control_mapping_bytes = state_->page_bytes;
        state_->control_mapping = ::mmap(nullptr,
                                         state_->control_mapping_bytes,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED,
                                         state_->fd,
                                         0);
        if (state_->control_mapping == MAP_FAILED)
            throw_root_journal_errno("map persistent root cache");

        auto* table = root_table();
        std::uint64_t found_magic = 0;
        std::memcpy(&found_magic, &table->magic, sizeof(found_magic));
        if (found_magic != root_table_magic) {
            table = ::new (table) shared_root_table{};
            table->magic = root_table_magic;
        }
        if (!table->global_generation.is_lock_free() ||
            !table->roots[0].store_id.is_lock_free() ||
            !table->roots[0].sequence.is_lock_free() ||
            !table->roots[0].banks[0].generation.is_lock_free())
            throw std::runtime_error{
                "persistent root reads require lock-free 64-bit atomics"};
    }

    root_slot* find_root_slot(std::uint64_t store_id) const
    {
        root_slot* empty = nullptr;
        for (auto& slot : root_table()->roots) {
            const auto registered =
                slot.store_id.load(std::memory_order_acquire);
            if (registered == store_id)
                return &slot;
            if ((registered == 0 ||
                 slot.sequence.load(std::memory_order_acquire) == 0) &&
                empty == nullptr)
                empty = &slot;
        }
        return empty;
    }

    static void publish_root_cache(
        root_slot& slot, const durable_root_descriptor& descriptor)
    {
        auto sequence = slot.sequence.load(std::memory_order_acquire);
        if (sequence != 0) {
            const auto current = descriptor_from_slot(
                descriptor.store_id, slot, sequence);
            if (current.generation > descriptor.generation)
                return;
            if (current.generation == descriptor.generation) {
                if (current.root != descriptor.root ||
                    current.read_watermark != descriptor.read_watermark ||
                    current.layout_id != descriptor.layout_id)
                    throw std::runtime_error{
                        "durable root cache disagrees with its journal record"};
                return;
            }
        }
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error{
                "persistent root cache sequence is exhausted"};
        const auto next = sequence + 1;
        auto& bank = slot.banks[next & 1u];
        bank.generation.store(descriptor.generation,
                              std::memory_order_relaxed);
        bank.read_watermark.store(descriptor.read_watermark,
                                  std::memory_order_relaxed);
        bank.root_bits.store(descriptor.root.bits(),
                             std::memory_order_relaxed);
        bank.layout_id.store(descriptor.layout_id,
                             std::memory_order_relaxed);
        auto* table = reinterpret_cast<shared_root_table*>(
            reinterpret_cast<std::byte*>(&slot) -
            offsetof(shared_root_table, roots));
        auto generation = table->global_generation.load(
            std::memory_order_relaxed);
        while (generation < descriptor.generation &&
               !table->global_generation.compare_exchange_weak(
                   generation,
                   descriptor.generation,
                   std::memory_order_release,
                   std::memory_order_relaxed))
            ;
        if (sequence == 0 ||
            slot.store_id.load(std::memory_order_relaxed) !=
                descriptor.store_id)
            slot.store_id.store(descriptor.store_id,
                                std::memory_order_release);
        slot.sequence.store(next, std::memory_order_release);
    }

    static durable_root_descriptor descriptor_from_slot(
        std::uint64_t store_id, const root_slot& slot, std::uint64_t sequence)
    {
        for (;;) {
            const auto& bank = slot.banks[sequence & 1u];
            const auto generation =
                bank.generation.load(std::memory_order_relaxed);
            const auto watermark =
                bank.read_watermark.load(std::memory_order_relaxed);
            const auto root_bits =
                bank.root_bits.load(std::memory_order_relaxed);
            const auto layout_id =
                bank.layout_id.load(std::memory_order_relaxed);
            if (slot.sequence.load(std::memory_order_acquire) != sequence) {
                sequence = slot.sequence.load(std::memory_order_acquire);
                continue;
            }
            return {store_id,
                    generation,
                    watermark,
                    immer::storage_ptr<void>::from_bits(root_bits),
                    layout_id};
        }
    }

    void reconcile_root_table(const std::vector<scanned_record>& records)
    {
        std::map<std::uint64_t, durable_root_descriptor> latest;
        for (const auto& record : records)
            latest[record.descriptor.store_id] = record.descriptor;
        for (const auto& item : latest) {
            auto* slot = find_root_slot(item.first);
            if (slot == nullptr)
                throw std::length_error{
                    "persistent root cache has reached its 32-store capacity"};
            publish_root_cache(*slot, item.second);
        }
    }

    void initialize()
    {
        const auto page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0)
            throw_root_journal_errno("query root journal page size");
        if (std::endian::native != std::endian::little)
            throw std::runtime_error{
                "persistent root journal requires a little-endian target"};
        state_->page_bytes = static_cast<std::size_t>(page);

        file_lock lock{state_->fd, LOCK_EX};
        const auto size = file_size();
        if (size == 0) {
            if (::ftruncate(state_->fd, static_cast<off_t>(page)) != 0)
                throw_root_journal_errno("initialize persistent root journal");
            write_superblock();
        } else {
            if (size < sizeof(disk_superblock))
                throw std::runtime_error{
                    "root journal superblock is truncated"};
            disk_superblock block{};
            read_exact(state_->fd, &block, sizeof(block), 0);
            if (size == state_->page_bytes &&
                std::all_of(std::begin(block.magic),
                            std::end(block.magic),
                            [](char c) { return c == 0; })) {
                write_superblock();
            } else if (std::memcmp(block.magic,
                                   superblock_magic,
                                   sizeof(superblock_magic)) != 0 ||
                       block.version != format_version ||
                       block.page_bytes != state_->page_bytes ||
                       block.checksum != checksum(
                           &block, offsetof(disk_superblock, checksum)) ||
                       size < block.page_bytes) {
                throw std::runtime_error{
                    "root journal superblock is invalid"};
            }
        }
        initialize_root_table();
        reconcile_root_table(scan());
    }

    std::vector<scanned_record> scan() const
    {
        const auto end = file_size();
        auto offset = static_cast<std::uint64_t>(state_->page_bytes);
        std::vector<scanned_record> result;
        while (end - offset >= sizeof(disk_root_record)) {
            disk_root_record record{};
            read_exact(state_->fd,
                       &record,
                       sizeof(record),
                       static_cast<off_t>(offset));
            const auto empty_magic =
                std::all_of(std::begin(record.magic),
                            std::end(record.magic),
                            [](char c) { return c == 0; });
            if (empty_magic)
                break;
            if (std::memcmp(record.magic,
                            record_magic,
                            sizeof(record_magic)) != 0) {
                // The commit marker is written only after the record body is
                // durable. A torn marker on the final append is an
                // uncommitted record, so retain earlier roots and let the
                // next writer truncate this tail. An invalid marker before
                // another record indicates corruption in committed history.
                if (offset + sizeof(disk_root_record) == end)
                    break;
                throw std::runtime_error{
                    "persistent root record commit marker is invalid"};
            }
            if (record.version != format_version || record.store_id == 0 ||
                record.generation == 0 || record.root_bits == 0 ||
                (record.root_bits >> 32) == 0 ||
                record.layout_id == 0 ||
                record.read_watermark >= record.generation ||
                record.checksum != checksum(
                    &record, offsetof(disk_root_record, checksum)) ||
                (!result.empty() &&
                 record.generation <=
                     result.back().descriptor.generation))
                throw std::runtime_error{"persistent root record is invalid"};
            result.push_back(scanned_record{
                durable_root_descriptor{record.store_id,
                                        record.generation,
                                        record.read_watermark,
                                        immer::storage_ptr<void>::from_bits(
                                            record.root_bits),
                                        record.layout_id}});
            offset += sizeof(disk_root_record);
        }
        return result;
    }

    std::uint64_t file_size() const
    {
        struct stat status
        {};
        if (::fstat(state_->fd, &status) != 0)
            throw_root_journal_errno("stat persistent root journal");
        if (status.st_size < 0)
            throw std::runtime_error{"root journal has invalid size"};
        return static_cast<std::uint64_t>(status.st_size);
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

    void write_superblock()
    {
        disk_superblock block{};
        std::memcpy(block.magic,
                    superblock_magic,
                    sizeof(superblock_magic));
        block.version = format_version;
        block.page_bytes = static_cast<std::uint32_t>(state_->page_bytes);
        block.checksum = checksum(&block,
                                  offsetof(disk_superblock, checksum));
        std::memset(block.magic, 0, sizeof(block.magic));
        write_exact(state_->fd, &block, sizeof(block), 0);
        sync_file();
        write_exact(state_->fd, superblock_magic, sizeof(superblock_magic), 0);
        sync_file();
    }

    void append_locked(const durable_root_descriptor& descriptor,
                       std::size_t record_count)
    {
        const auto valid_end = state_->page_bytes +
                               record_count * sizeof(disk_root_record);
        if (file_size() != valid_end &&
            ::ftruncate(state_->fd, static_cast<off_t>(valid_end)) != 0)
            throw_root_journal_errno("truncate incomplete root record");

        disk_root_record record{};
        std::memcpy(record.magic, record_magic, sizeof(record_magic));
        record.version = format_version;
        record.store_id = descriptor.store_id;
        record.generation = descriptor.generation;
        record.read_watermark = descriptor.read_watermark;
        record.root_bits = descriptor.root.bits();
        record.layout_id = descriptor.layout_id;
        record.checksum = checksum(
            &record, offsetof(disk_root_record, checksum));
        std::memset(record.magic, 0, sizeof(record.magic));
        write_exact(state_->fd,
                    &record,
                    sizeof(record),
                    static_cast<off_t>(valid_end));
        sync_file();
        test_failpoint("root-record-write");
        write_exact(state_->fd,
                    record_magic,
                    sizeof(record_magic),
                    static_cast<off_t>(valid_end));
        sync_file();
    }

    static void test_failpoint(const char* name)
    {
#if defined(LAGER_PERSISTENT_ROUND_TRIP_FAILPOINTS)
        const auto* requested = std::getenv("LAGER_PERSISTENT_FAILPOINT");
        if (requested != nullptr && std::strcmp(requested, name) == 0) {
            ::kill(::getpid(), SIGKILL);
            ::_exit(137);
        }
#else
        (void) name;
#endif
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
                throw_root_journal_errno("read persistent root journal");
            if (count == 0)
                throw std::runtime_error{"root journal ended during a read"};
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
                throw_root_journal_errno("write persistent root journal");
            if (count == 0)
                throw std::runtime_error{
                    "root journal accepted a zero-byte write"};
            completed += static_cast<std::size_t>(count);
        }
    }

    void sync_file() const
    {
        if (::fdatasync(state_->fd) != 0)
            throw_root_journal_errno("flush persistent root journal");
    }

    void require_open() const
    {
        if (!state_)
            throw std::logic_error{"persistent root journal is not open"};
    }

    std::shared_ptr<journal_state> state_;
#else
    struct journal_state
    {};
#endif
};

inline std::uint64_t persistent_root_journal::global_generation() const noexcept
{
#if defined(_WIN32)
    return 0;
#else
    if (!state_ || state_->control_mapping == MAP_FAILED)
        return 0;
    return root_table()->global_generation.load(std::memory_order_acquire);
#endif
}

} // namespace lager
