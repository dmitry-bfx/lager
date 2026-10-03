// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <lager/storage.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
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

struct shared_ephemeral_root_descriptor
{
    std::uint64_t store_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t read_watermark = 0;
    immer::storage_ptr<void> root;
    std::uint64_t layout_id = 0;
};

struct shared_ephemeral_read_dependency
{
    std::uint64_t store_id = 0;
    std::uint64_t generation = 0;
};

struct shared_ephemeral_root_pin
{
    shared_ephemeral_root_descriptor descriptor;
    std::shared_ptr<void> lifetime;
};

class shared_ephemeral_write_conflict : public std::runtime_error
{
public:
    shared_ephemeral_write_conflict()
        : std::runtime_error{"shared ephemeral root changed before commit"}
    {}
};

inline void throw_shared_memory_errno(const char* operation)
{
    throw std::system_error{errno, std::generic_category(), operation};
}

/*!
 * Named, process-shared, non-durable storage for one fixed-slot arena.
 *
 * The creator unlinks the shared-memory name when its final local mapping
 * owner is released. Other processes can open the name while it exists and
 * map it at arbitrary addresses. Slot reservations use a process lock and a
 * high-water cursor in the mapping. A fixed 32-entry root table publishes
 * immutable root descriptors with lock-free reads and serialized writers.
 * This backend does not reclaim slots.
 */
class shared_ephemeral_storage
{
    struct mapping_owner : std::enable_shared_from_this<mapping_owner>
    {
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

        struct reader_slot
        {
            std::atomic<std::uint64_t> process_id{0};
            std::atomic<std::uint64_t> sequence{0};
            std::atomic<std::uint64_t> store_id{0};
            std::atomic<std::uint64_t> generation{0};
            std::atomic<std::uint64_t> root_bits{0};
            std::atomic<std::uint64_t> layout_id{0};
            std::atomic<std::uint64_t> thread_id{0};
        };

        struct header
        {
            char magic[8];
            std::uint32_t version;
            std::uint32_t extent_id;
            std::uint64_t slot_size;
            std::uint64_t slot_alignment;
            std::uint64_t layout_id;
            std::uint64_t slot_count;
            std::uint64_t next_slot;
            std::uint64_t next_slot_check;
            std::atomic<std::uint64_t> global_generation{0};
            root_slot roots[32];
            reader_slot readers[24];
        };

        mapping_owner(std::string name_,
                      int fd_,
                      void* address_,
                      std::size_t bytes_,
                      std::size_t page_bytes_,
                      std::uint32_t extent_id_,
                      std::size_t slot_size_,
                      std::size_t slot_alignment_,
                      std::size_t slot_count_,
                      std::uint64_t layout_id_,
                      bool creator_)
            : name{std::move(name_)},
              fd{fd_},
              address{address_},
              bytes{bytes_},
              page_bytes{page_bytes_},
              extent_id{extent_id_},
              slot_size{slot_size_},
              slot_alignment{slot_alignment_},
              slot_count{slot_count_},
              layout_id{layout_id_},
              creator{creator_}
        {
#if !defined(_WIN32)
            lock(LOCK_SH);
            unlock_guard unlock{fd};
            if (!disk_header()->global_generation.is_lock_free() ||
                !disk_header()->roots[0].store_id.is_lock_free() ||
                !disk_header()->roots[0].sequence.is_lock_free() ||
                !disk_header()->roots[0].banks[0].generation.is_lock_free() ||
                !disk_header()->readers[0].process_id.is_lock_free() ||
                !disk_header()->readers[0].sequence.is_lock_free() ||
                !disk_header()->readers[0].root_bits.is_lock_free())
                throw std::runtime_error{
                    "shared root publication requires lock-free 64-bit atomics"};
#endif
            const auto cursor = disk_header()->next_slot;
            const auto check = disk_header()->next_slot_check;
            const auto used = check == ~cursor && cursor <= slot_count
                                  ? static_cast<std::size_t>(cursor)
                                  : slot_count;
            slot_bitmap = std::make_shared<arena_slot_bitmap>(slot_count, used);
            known_highwater = used;
            local_next = used;
            local_end = used;
        }

        ~mapping_owner()
        {
#if !defined(_WIN32)
            if (address != MAP_FAILED)
                ::munmap(address, bytes);
            if (fd >= 0)
                ::close(fd);
            if (creator)
                ::shm_unlink(name.c_str());
#endif
        }

        struct reader_pin_lease
        {
            reader_pin_lease(std::shared_ptr<mapping_owner> owner_,
                             std::size_t slot_,
                             std::uint64_t process_id_)
                : owner{std::move(owner_)}, slot{slot_}, process_id{process_id_}
            {}

            ~reader_pin_lease()
            {
#if !defined(_WIN32)
                if (!owner || static_cast<std::uint64_t>(::getpid()) !=
                                  process_id)
                    return;
                auto& pin = owner->disk_header()->readers[slot];
                pin.sequence.store(0, std::memory_order_seq_cst);
                pin.root_bits.store(0, std::memory_order_relaxed);
                pin.store_id.store(0, std::memory_order_relaxed);
                pin.generation.store(0, std::memory_order_relaxed);
                pin.layout_id.store(0, std::memory_order_relaxed);
                pin.thread_id.store(0, std::memory_order_relaxed);
                pin.process_id.store(0, std::memory_order_seq_cst);
#endif
            }

            std::shared_ptr<mapping_owner> owner;
            std::size_t slot;
            std::uint64_t process_id;
        };

        header* disk_header() const
        {
            return static_cast<header*>(address);
        }

#if !defined(_WIN32)
        void lock(int operation) const
        {
            while (::flock(fd, operation) != 0) {
                if (errno != EINTR)
                    throw_shared_memory_errno("lock shared ephemeral arena");
            }
        }

        struct unlock_guard
        {
            explicit unlock_guard(int fd_) : fd{fd_} {}
            ~unlock_guard() { ::flock(fd, LOCK_UN); }
            int fd;
        };

        void refresh_allocation_state()
        {
            std::lock_guard<std::mutex> thread_lock{allocation_mutex};
            lock(LOCK_EX);
            unlock_guard unlock{fd};
            const auto cursor = disk_header()->next_slot;
            const auto check = disk_header()->next_slot_check;
            if (check != ~cursor || cursor > slot_count)
                throw std::runtime_error{
                    "shared ephemeral allocation cursor is invalid"};
            if (cursor > known_highwater) {
                slot_bitmap->reserve_range(known_highwater,
                                           static_cast<std::size_t>(cursor));
                known_highwater = static_cast<std::size_t>(cursor);
            }
        }

        shared_ephemeral_root_descriptor read_root(std::uint64_t store_id) const
        {
            if (store_id == 0)
                throw std::invalid_argument{
                    "shared ephemeral store ID must be nonzero"};
            for (const auto& slot : disk_header()->roots) {
                if (slot.store_id.load(std::memory_order_acquire) != store_id)
                    continue;
                for (;;) {
                    const auto sequence =
                        slot.sequence.load(std::memory_order_seq_cst);
                    if (sequence == 0)
                        throw std::out_of_range{
                            "shared ephemeral root is not published"};
                    const auto& bank = slot.banks[sequence & 1u];
                    const auto generation =
                        bank.generation.load(std::memory_order_relaxed);
                    const auto watermark =
                        bank.read_watermark.load(std::memory_order_relaxed);
                    const auto root_bits =
                        bank.root_bits.load(std::memory_order_relaxed);
                    const auto layout_id =
                        bank.layout_id.load(std::memory_order_relaxed);
                    if (slot.sequence.load(std::memory_order_seq_cst) !=
                        sequence)
                        continue;
                    if (generation == 0 || root_bits == 0 || layout_id == 0 ||
                        watermark >= generation)
                        throw std::runtime_error{
                            "shared ephemeral root descriptor is invalid"};
                    return {store_id,
                            generation,
                            watermark,
                            immer::storage_ptr<void>::from_bits(root_bits),
                            layout_id};
                }
            }
            throw std::out_of_range{"shared ephemeral store is not registered"};
        }

        shared_ephemeral_root_pin pin_current_root(
            std::uint64_t store_id)
        {
            for (;;) {
                const auto before = read_root(store_id);
                auto lifetime = pin_root_handle(before.store_id,
                                                before.generation,
                                                before.root,
                                                before.layout_id);
                const auto after = read_root(store_id);
                if (before.generation == after.generation &&
                    before.root == after.root &&
                    before.layout_id == after.layout_id)
                    return {after, std::move(lifetime)};
            }
        }

        std::shared_ptr<void> pin_root_handle(
            std::uint64_t store_id,
            std::uint64_t generation,
            immer::storage_ptr<void> root,
            std::uint64_t layout_id)
        {
            if (store_id == 0 || !root || layout_id == 0)
                throw std::invalid_argument{
                    "shared ephemeral root pin is invalid"};
            const auto process_id = static_cast<std::uint64_t>(::getpid());
            const auto thread_id =
                static_cast<std::uint64_t>(std::hash<std::thread::id>{}(
                    std::this_thread::get_id()));
            auto* header = disk_header();
            for (int attempt = 0; attempt != 2; ++attempt) {
                for (std::size_t i = 0; i < std::size(header->readers); ++i) {
                    auto& slot = header->readers[i];
                    std::uint64_t empty = 0;
                    if (!slot.process_id.compare_exchange_strong(
                            empty,
                            process_id,
                            std::memory_order_seq_cst,
                            std::memory_order_relaxed))
                        continue;
                    slot.store_id.store(store_id, std::memory_order_relaxed);
                    slot.generation.store(generation,
                                          std::memory_order_relaxed);
                    slot.root_bits.store(root.bits(),
                                         std::memory_order_relaxed);
                    slot.layout_id.store(layout_id,
                                         std::memory_order_relaxed);
                    slot.thread_id.store(thread_id,
                                         std::memory_order_relaxed);
                    slot.sequence.store(1, std::memory_order_seq_cst);
                    try {
                        return std::shared_ptr<void>{
                            new reader_pin_lease{shared_from_this(),
                                                 i,
                                                 process_id}};
                    } catch (...) {
                        clear_reader_pin(slot, process_id);
                        throw;
                    }
                }
                if (attempt == 0)
                    reap_dead_reader_pins();
            }
            throw std::length_error{
                "shared ephemeral root pin table has reached its 24-slot capacity"};
        }

        std::vector<shared_ephemeral_root_descriptor> pinned_roots() const
        {
            std::vector<shared_ephemeral_root_descriptor> result;
            for (auto& slot : disk_header()->readers) {
                const auto process_id =
                    slot.process_id.load(std::memory_order_seq_cst);
                if (process_id == 0)
                    continue;
                if (!process_alive(process_id)) {
                    clear_reader_pin(slot, process_id);
                    continue;
                }
                if (slot.sequence.load(std::memory_order_seq_cst) != 1)
                    continue;
                const auto store_id =
                    slot.store_id.load(std::memory_order_relaxed);
                const auto generation =
                    slot.generation.load(std::memory_order_relaxed);
                const auto root_bits =
                    slot.root_bits.load(std::memory_order_relaxed);
                const auto layout_id =
                    slot.layout_id.load(std::memory_order_relaxed);
                if (slot.sequence.load(std::memory_order_seq_cst) != 1 ||
                    slot.process_id.load(std::memory_order_seq_cst) !=
                        process_id)
                    continue;
                if (store_id == 0 || root_bits == 0 || layout_id == 0)
                    throw std::runtime_error{
                        "shared ephemeral root pin is invalid"};
                result.push_back({store_id,
                                  generation,
                                  0,
                                  immer::storage_ptr<void>::from_bits(root_bits),
                                  layout_id});
            }
            return result;
        }

        std::vector<shared_ephemeral_root_descriptor> roots_for_gc() const
        {
            lock(LOCK_SH);
            unlock_guard unlock{fd};
            std::vector<shared_ephemeral_root_descriptor> result;
            for (const auto& slot : disk_header()->roots) {
                const auto store_id =
                    slot.store_id.load(std::memory_order_acquire);
                if (store_id != 0 &&
                    slot.sequence.load(std::memory_order_seq_cst) != 0)
                    result.push_back(read_root(store_id));
            }
            auto pins = pinned_roots();
            result.insert(result.end(), pins.begin(), pins.end());
            return result;
        }

        static bool process_alive(std::uint64_t process_id) noexcept
        {
            if (process_id == 0)
                return false;
            if (process_id > static_cast<std::uint64_t>(
                                 std::numeric_limits<pid_t>::max()))
                return true;
            if (::kill(static_cast<pid_t>(process_id), 0) == 0)
                return true;
            return errno != ESRCH;
        }

        static void clear_reader_pin(reader_slot& slot,
                                     std::uint64_t process_id) noexcept
        {
            auto expected = process_id;
            if (!slot.process_id.compare_exchange_strong(
                    expected,
                    std::numeric_limits<std::uint64_t>::max(),
                    std::memory_order_seq_cst,
                    std::memory_order_relaxed))
                return;
            slot.sequence.store(0, std::memory_order_seq_cst);
            slot.root_bits.store(0, std::memory_order_relaxed);
            slot.store_id.store(0, std::memory_order_relaxed);
            slot.generation.store(0, std::memory_order_relaxed);
            slot.layout_id.store(0, std::memory_order_relaxed);
            slot.thread_id.store(0, std::memory_order_relaxed);
            slot.process_id.store(0, std::memory_order_seq_cst);
        }

        void reap_dead_reader_pins() const
        {
            for (auto& slot : disk_header()->readers) {
                const auto process_id =
                    slot.process_id.load(std::memory_order_seq_cst);
                if (process_id != 0 && !process_alive(process_id))
                    clear_reader_pin(slot, process_id);
            }
        }

        shared_ephemeral_root_descriptor publish_root(
            std::uint64_t store_id,
            std::uint64_t expected_generation,
            immer::storage_ptr<void> root,
            std::uint64_t layout_id,
            std::uint64_t read_watermark,
            const std::vector<shared_ephemeral_read_dependency>& reads,
            std::uint64_t minimum_generation)
        {
            if (store_id == 0 || !root || root.extent_id() == 0 ||
                layout_id == 0)
                throw std::invalid_argument{
                    "shared ephemeral root publication is invalid"};
            std::lock_guard<std::mutex> thread_lock{allocation_mutex};
            lock(LOCK_EX);
            unlock_guard unlock{fd};

            auto* target = static_cast<root_slot*>(nullptr);
            root_slot* empty_slot = nullptr;
            for (auto& slot : disk_header()->roots) {
                const auto registered =
                    slot.store_id.load(std::memory_order_acquire);
                if (registered == store_id) {
                    target = &slot;
                    break;
                }
                if ((registered == 0 ||
                     slot.sequence.load(std::memory_order_seq_cst) == 0) &&
                    empty_slot == nullptr)
                    empty_slot = &slot;
            }
            const bool is_new = target == nullptr;
            if (is_new) {
                if (expected_generation != 0 || empty_slot == nullptr)
                    throw std::out_of_range{
                        "shared ephemeral root slot is unavailable"};
                target = empty_slot;
            }

            std::uint64_t sequence =
                target->sequence.load(std::memory_order_seq_cst);
            if (sequence == 0) {
                if (expected_generation != 0)
                    throw std::runtime_error{
                        "shared ephemeral root changed before commit"};
            } else {
                const auto current = read_root(store_id);
                if (current.generation != expected_generation)
                    throw shared_ephemeral_write_conflict{};
                if (current.layout_id != layout_id)
                    throw std::invalid_argument{
                        "shared ephemeral root layout changed"};
            }

            for (const auto& dependency : reads) {
                const auto current = read_root(dependency.store_id);
                if (current.generation != dependency.generation)
                    throw shared_ephemeral_write_conflict{};
            }

            auto* header = disk_header();
            const auto current_global =
                header->global_generation.load(std::memory_order_relaxed);
            const auto floor =
                std::max({current_global, minimum_generation, read_watermark});
            if (floor == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error{
                    "shared ephemeral generations are exhausted"};
            const auto generation = floor + 1;

            const auto next_sequence = sequence + 1;
            if (next_sequence == 0)
                throw std::overflow_error{
                    "shared ephemeral root sequence is exhausted"};
            auto& bank = target->banks[next_sequence & 1u];
            bank.generation.store(generation, std::memory_order_relaxed);
            bank.read_watermark.store(read_watermark,
                                      std::memory_order_relaxed);
            bank.root_bits.store(root.bits(), std::memory_order_relaxed);
            bank.layout_id.store(layout_id, std::memory_order_relaxed);
            header->global_generation.store(generation,
                                            std::memory_order_relaxed);
            if (is_new || sequence == 0)
                target->store_id.store(store_id, std::memory_order_release);
            target->sequence.store(next_sequence,
                                   std::memory_order_seq_cst);
            return {store_id,
                    generation,
                    read_watermark,
                    root,
                    layout_id};
        }

        void* allocate_slot(std::size_t requested_size,
                            std::size_t requested_alignment)
        {
            if (requested_size > slot_size ||
                requested_alignment > slot_alignment)
                return nullptr;
            std::lock_guard<std::mutex> thread_lock{allocation_mutex};
            auto* data = static_cast<std::byte*>(address) + page_bytes;
            if (local_next < local_end) {
                const auto slot = local_next++;
                slot_bitmap->reserve(slot);
                return data + slot * slot_size;
            }

            lock(LOCK_EX);
            unlock_guard unlock{fd};
            auto* header = disk_header();
            auto cursor = header->next_slot;
            const auto check = header->next_slot_check;
            if (check != ~cursor || cursor > slot_count)
                return nullptr;
            if (cursor > known_highwater) {
                slot_bitmap->reserve_range(known_highwater,
                                           static_cast<std::size_t>(cursor));
                known_highwater = static_cast<std::size_t>(cursor);
            }
            if (cursor == slot_count)
                return nullptr;

            // This backend does not reclaim abandoned reservations yet, so
            // keep crash loss to one slot instead of stranding a large batch.
            constexpr std::size_t reservation_size = 1;
            const auto remaining = slot_count - cursor;
            const auto reservation = std::min<std::uint64_t>(
                reservation_size, remaining);
            const auto next = cursor + reservation;
            header->next_slot_check = ~next;
            std::atomic_thread_fence(std::memory_order_release);
            header->next_slot = next;
            const auto first = static_cast<std::size_t>(cursor);
            local_next = first + 1;
            local_end = static_cast<std::size_t>(next);
            slot_bitmap->reserve_range(first, local_end);
            known_highwater = local_end;
            return data + first * slot_size;
        }
#endif

        std::string name;
        int fd;
        void* address;
        std::size_t bytes;
        std::size_t page_bytes;
        std::uint32_t extent_id;
        std::size_t slot_size;
        std::size_t slot_alignment;
        std::size_t slot_count;
        std::uint64_t layout_id;
        bool creator;
        std::shared_ptr<arena_slot_bitmap> slot_bitmap;
        std::mutex allocation_mutex;
        std::size_t known_highwater = 0;
        std::size_t local_next = 0;
        std::size_t local_end = 0;
    };

    static_assert(sizeof(mapping_owner::header) <= 4096,
                  "shared arena control header must fit a 4 KiB page");

public:
    shared_ephemeral_storage() = default;

    static shared_ephemeral_storage create(const std::string& name,
                                           std::uint32_t extent_id,
                                           std::size_t slot_size,
                                           std::size_t slot_alignment,
                                           std::uint64_t layout_id,
                                           std::size_t slot_count)
    {
#if defined(_WIN32)
        (void) name;
        (void) extent_id;
        (void) slot_size;
        (void) slot_alignment;
        (void) layout_id;
        (void) slot_count;
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        validate_parameters(name,
                            extent_id,
                            slot_size,
                            slot_alignment,
                            layout_id,
                            slot_count);
        const auto fd =
            ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            throw_shared_memory_errno("create shared ephemeral arena");
        try {
            const auto page = system_page_size();
            if (page < sizeof(mapping_owner::header))
                throw std::runtime_error{
                    "shared arena page is too small for its header"};
            if (slot_alignment > page || page % slot_alignment != 0)
                throw std::invalid_argument{
                    "shared arena alignment must divide the system page size"};
            lock_fd(fd, LOCK_EX);
            const auto data_bytes = checked_multiply(slot_size, slot_count);
            const auto mapped_data_bytes = round_up(data_bytes, page);
            const auto bytes = checked_add(page, mapped_data_bytes);
            if (bytes > static_cast<std::uint64_t>(
                            std::numeric_limits<off_t>::max()) ||
                bytes > std::numeric_limits<std::size_t>::max())
                throw std::overflow_error{"shared arena is too large"};
            if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0)
                throw_shared_memory_errno("size shared ephemeral arena");
            auto* address = ::mmap(nullptr,
                                   static_cast<std::size_t>(bytes),
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fd,
                                   0);
            if (address == MAP_FAILED) {
                const auto error = errno;
                unlock_fd(fd);
                errno = error;
                throw_shared_memory_errno("map shared ephemeral arena");
            }

            std::memset(address, 0, page);
            auto* header = ::new (address) mapping_owner::header{};
            header->version = 3;
            header->extent_id = extent_id;
            header->slot_size = slot_size;
            header->slot_alignment = slot_alignment;
            header->layout_id = layout_id;
            header->slot_count = slot_count;
            header->next_slot = 0;
            header->next_slot_check = ~std::uint64_t{0};
            std::memcpy(header->magic, magic, sizeof(magic));
            unlock_fd(fd);
            shared_ephemeral_storage result;
            try {
                result.mapping_ = std::make_shared<mapping_owner>(
                    name,
                    fd,
                    address,
                    static_cast<std::size_t>(bytes),
                    page,
                    extent_id,
                    slot_size,
                    slot_alignment,
                    slot_count,
                    layout_id,
                    true);
            } catch (...) {
                ::munmap(address, static_cast<std::size_t>(bytes));
                throw;
            }
            return result;
        } catch (...) {
            ::close(fd);
            ::shm_unlink(name.c_str());
            throw;
        }
#endif
    }

    static shared_ephemeral_storage open(const std::string& name,
                                         std::uint32_t expected_extent_id,
                                         std::size_t expected_slot_size,
                                         std::size_t expected_alignment,
                                         std::uint64_t expected_layout_id)
    {
#if defined(_WIN32)
        (void) name;
        (void) expected_extent_id;
        (void) expected_slot_size;
        (void) expected_alignment;
        (void) expected_layout_id;
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (name.empty() || name.front() != '/' || expected_extent_id == 0 ||
            expected_slot_size == 0 || expected_alignment == 0 ||
            expected_layout_id == 0)
            throw std::invalid_argument{
                "shared ephemeral arena identity is invalid"};
        const auto fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        if (fd < 0)
            throw_shared_memory_errno("open shared ephemeral arena");
        try {
            lock_fd(fd, LOCK_SH);
            struct stat status
            {};
            if (::fstat(fd, &status) != 0)
                throw_shared_memory_errno("stat shared ephemeral arena");
            if (status.st_size <= 0 ||
                static_cast<std::uint64_t>(status.st_size) >
                    std::numeric_limits<std::size_t>::max())
                throw std::runtime_error{
                    "shared ephemeral arena has an invalid size"};
            auto* address = ::mmap(nullptr,
                                   static_cast<std::size_t>(status.st_size),
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fd,
                                   0);
            if (address == MAP_FAILED)
                throw_shared_memory_errno("map shared ephemeral arena");
            struct mapping_guard
            {
                mapping_guard(void* address_, std::size_t bytes_)
                    : address{address_}, bytes{bytes_}
                {}
                ~mapping_guard()
                {
                    if (address != MAP_FAILED)
                        ::munmap(address, bytes);
                }
                void release() noexcept { address = MAP_FAILED; }
                void* address;
                std::size_t bytes;
            } mapped{address, static_cast<std::size_t>(status.st_size)};
            const auto page = system_page_size();
            if (static_cast<std::size_t>(status.st_size) < page) {
                throw std::runtime_error{
                    "shared ephemeral arena header is truncated"};
            }
            if (page < sizeof(mapping_owner::header))
                throw std::runtime_error{
                    "shared arena page is too small for its header"};
            if (expected_alignment > page || page % expected_alignment != 0 ||
                expected_slot_size % expected_alignment != 0) {
                throw std::invalid_argument{
                    "shared ephemeral arena alignment is incompatible"};
            }
            const auto* header =
                static_cast<const mapping_owner::header*>(address);
            const auto header_ok =
                std::memcmp(header->magic, magic, sizeof(magic)) == 0 &&
                header->version == 3 &&
                header->extent_id == expected_extent_id &&
                header->slot_size == expected_slot_size &&
                header->slot_alignment == expected_alignment &&
                header->layout_id == expected_layout_id &&
                header->slot_count != 0 &&
                header->slot_count <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::uint32_t>::max()) +
                        std::uint64_t{1} &&
                header->slot_count <= std::numeric_limits<std::size_t>::max() &&
                header->slot_size <= std::numeric_limits<std::size_t>::max();
            if (!header_ok) {
                throw std::invalid_argument{
                    "shared ephemeral arena layout does not match"};
            }
            const auto count = static_cast<std::size_t>(header->slot_count);
            const auto data_bytes = checked_multiply(expected_slot_size, count);
            if (checked_add(page, round_up(data_bytes, page)) !=
                static_cast<std::uint64_t>(status.st_size)) {
                throw std::runtime_error{
                    "shared ephemeral arena mapping size is invalid"};
            }
            unlock_fd(fd);
            shared_ephemeral_storage result;
            result.mapping_ = std::make_shared<mapping_owner>(
                name,
                fd,
                address,
                static_cast<std::size_t>(status.st_size),
                page,
                expected_extent_id,
                expected_slot_size,
                expected_alignment,
                count,
                expected_layout_id,
                false);
            mapped.release();
            return result;
        } catch (...) {
            unlock_fd(fd);
            ::close(fd);
            throw;
        }
#endif
    }

    storage_extent view() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        auto owner = mapping_;
        storage_extent extent;
        extent.id = owner->extent_id;
        extent.base = static_cast<std::byte*>(owner->address) + owner->page_bytes;
        extent.size_bytes = owner->slot_size * owner->slot_count;
        extent.slot_size = owner->slot_size;
        extent.slot_alignment = owner->slot_alignment;
        extent.layout_id = owner->layout_id;
        extent.owner = owner;
        extent.slot_bitmap = owner->slot_bitmap;
        extent.allocate_slot = [owner](std::size_t size,
                                       std::size_t alignment) {
            return owner->allocate_slot(size, alignment);
        };
        extent.refresh_allocation_state = [owner] {
            owner->refresh_allocation_state();
        };
        extent.can_reclaim_slots = false;
        return extent;
#endif
    }

    shared_ephemeral_root_descriptor read_root(std::uint64_t store_id) const
    {
#if defined(_WIN32)
        (void) store_id;
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->read_root(store_id);
#endif
    }

    shared_ephemeral_root_pin pin_current_root(std::uint64_t store_id) const
    {
#if defined(_WIN32)
        (void) store_id;
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->pin_current_root(store_id);
#endif
    }

    std::shared_ptr<void> pin_root_handle(std::uint64_t store_id,
                                          std::uint64_t generation,
                                          immer::storage_ptr<void> root,
                                          std::uint64_t layout_id) const
    {
#if defined(_WIN32)
        (void) store_id;
        (void) generation;
        (void) root;
        (void) layout_id;
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->pin_root_handle(store_id,
                                         generation,
                                         root,
                                         layout_id);
#endif
    }

    std::vector<shared_ephemeral_root_descriptor> roots_for_gc() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->roots_for_gc();
#endif
    }

    shared_ephemeral_root_descriptor publish_root(
        std::uint64_t store_id,
        std::uint64_t expected_generation,
        immer::storage_ptr<void> root,
        std::uint64_t layout_id,
        std::uint64_t read_watermark = 0,
        const std::vector<shared_ephemeral_read_dependency>& reads = {},
        std::uint64_t minimum_generation = 0)
    {
#if defined(_WIN32)
        (void) store_id;
        (void) expected_generation;
        (void) root;
        (void) layout_id;
        (void) read_watermark;
        (void) reads;
        (void) minimum_generation;
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->publish_root(store_id,
                                      expected_generation,
                                      root,
                                      layout_id,
                                      read_watermark,
                                      reads,
                                      minimum_generation);
#endif
    }

    const std::string& name() const
    {
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->name;
    }

    std::uint64_t global_generation() const
    {
#if defined(_WIN32)
        throw std::runtime_error{
            "shared ephemeral storage currently requires POSIX"};
#else
        if (!mapping_)
            throw std::logic_error{"shared ephemeral arena is empty"};
        return mapping_->disk_header()->global_generation.load(
            std::memory_order_acquire);
#endif
    }

    explicit operator bool() const noexcept { return bool{mapping_}; }

private:
    static constexpr char magic[8] = {'L', 'A', 'G', 'E', 'R', 'S', 'H', '\0'};

#if !defined(_WIN32)
    static void lock_fd(int fd, int operation)
    {
        while (::flock(fd, operation) != 0) {
            if (errno != EINTR)
                throw_shared_memory_errno("lock shared ephemeral arena");
        }
    }

    static void unlock_fd(int fd) noexcept { ::flock(fd, LOCK_UN); }

    static std::size_t system_page_size()
    {
        const auto page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0)
            throw_shared_memory_errno("query shared arena page size");
        return static_cast<std::size_t>(page);
    }

    static void validate_parameters(const std::string& name,
                                    std::uint32_t extent_id,
                                    std::size_t slot_size,
                                    std::size_t slot_alignment,
                                    std::uint64_t layout_id,
                                    std::size_t slot_count)
    {
        if (name.empty() || name.front() != '/' || extent_id == 0 ||
            slot_size == 0 || slot_alignment == 0 || layout_id == 0 ||
            slot_count == 0 || slot_size % slot_alignment != 0 ||
            slot_count >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()) +
                    std::uint64_t{1})
            throw std::invalid_argument{
                "shared ephemeral arena parameters are invalid"};
    }

    static std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs)
    {
        if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
            throw std::overflow_error{"shared ephemeral arena size overflow"};
        return lhs + rhs;
    }

    static std::uint64_t checked_multiply(std::uint64_t lhs,
                                          std::uint64_t rhs)
    {
        if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
            throw std::overflow_error{"shared ephemeral arena size overflow"};
        return lhs * rhs;
    }

    static std::uint64_t round_up(std::uint64_t value, std::uint64_t multiple)
    {
        if (multiple == 0)
            throw std::invalid_argument{
                "shared ephemeral arena page size is invalid"};
        const auto remainder = value % multiple;
        return remainder == 0 ? value
                              : checked_add(value, multiple - remainder);
    }
#endif

    std::shared_ptr<mapping_owner> mapping_;
};

} // namespace lager
