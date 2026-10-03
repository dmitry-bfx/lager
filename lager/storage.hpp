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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace lager {

static_assert(std::endian::native == std::endian::little,
              "Lager shared storage currently requires a little-endian target");

class storage_context_scope;

//! Reconstructible allocation and mark bits for one fixed-size arena.
class arena_slot_bitmap
{
public:
    explicit arena_slot_bitmap(std::size_t slot_count,
                               std::size_t used_prefix = 0)
        : slot_count_{slot_count},
          allocated_((slot_count + word_bits - 1) / word_bits),
          marked_((slot_count + word_bits - 1) / word_bits),
          allocation_generations_(slot_count)
    {
        if (used_prefix > slot_count_)
            throw std::invalid_argument{"arena used prefix exceeds its size"};
        for (std::size_t slot = 0; slot < used_prefix; ++slot)
            set(allocated_, slot);
    }

    std::optional<std::size_t> allocate()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t word = 0; word < allocated_.size(); ++word) {
            const auto valid = valid_bits(word);
            const auto available = ~allocated_[word] & valid;
            if (available == 0)
                continue;
            const auto bit = static_cast<std::size_t>(std::countr_zero(available));
            const auto slot = word * word_bits + bit;
            set(allocated_, slot);
            allocation_generations_[slot] = current_generation_;
            if (collecting_)
                set(marked_, slot);
            return slot;
        }
        return std::nullopt;
    }

    bool reserve(std::size_t slot)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        require_slot(slot);
        const auto fresh = !test(allocated_, slot);
        set(allocated_, slot);
        if (fresh)
            allocation_generations_[slot] = current_generation_;
        if (collecting_)
            set(marked_, slot);
        return fresh;
    }

    void reserve_range(std::size_t first, std::size_t last)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (first > last || last > slot_count_)
            throw std::out_of_range{"arena reservation range is invalid"};
        for (auto slot = first; slot < last; ++slot) {
            const auto fresh = !test(allocated_, slot);
            set(allocated_, slot);
            if (fresh)
                allocation_generations_[slot] = current_generation_;
            if (collecting_)
                set(marked_, slot);
        }
    }

    void begin_mark()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (collecting_)
            throw std::logic_error{"arena collection is already active"};
        if (current_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error{"arena allocation generation exhausted"};
        std::fill(marked_.begin(), marked_.end(), 0);
        frozen_generation_ = current_generation_;
        ++current_generation_;
        collecting_ = true;
    }

    //! Start an allocation generation for a registered writer transaction.
    std::uint64_t advance_allocation_generation()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (current_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error{"arena allocation generation exhausted"};
        return ++current_generation_;
    }

    //! Protect all allocations owned by a live transaction generation.
    void mark_generation(std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!collecting_)
            throw std::logic_error{"arena marking has not begun"};
        for (std::size_t slot = 0; slot < slot_count_; ++slot)
            if (test(allocated_, slot) &&
                allocation_generations_[slot] == generation)
                set(marked_, slot);
    }

    //! Returns true only the first time this allocated slot is marked.
    bool mark(std::size_t slot)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        require_slot(slot);
        if (!collecting_)
            throw std::logic_error{"arena marking has not begun"};
        if (!test(allocated_, slot))
            throw std::invalid_argument{"GC root names an unallocated slot"};
        const auto fresh = !test(marked_, slot);
        set(marked_, slot);
        return fresh;
    }

    bool is_allocated(std::size_t slot) const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        require_slot(slot);
        return test(allocated_, slot);
    }

    std::size_t sweep()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!collecting_)
            throw std::logic_error{"arena marking has not begun"};
        std::size_t reclaimed = 0;
        for (std::size_t word = 0; word < allocated_.size(); ++word) {
            auto slots = allocated_[word];
            while (slots != 0) {
                const auto bit = static_cast<std::size_t>(
                    std::countr_zero(slots));
                const auto slot = word * word_bits + bit;
                const auto mask = std::uint64_t{1} << bit;
                if (allocation_generations_[slot] <= frozen_generation_ &&
                    !test(marked_, slot)) {
                    allocated_[word] &= ~mask;
                    allocation_generations_[slot] = 0;
                    ++reclaimed;
                }
                slots &= slots - 1;
            }
        }
        collecting_ = false;
        std::fill(marked_.begin(), marked_.end(), 0);
        return reclaimed;
    }

    void cancel_mark() noexcept
    {
        std::lock_guard<std::mutex> lock{mutex_};
        collecting_ = false;
        std::fill(marked_.begin(), marked_.end(), 0);
    }

    std::size_t slot_count() const noexcept { return slot_count_; }

private:
    static constexpr std::size_t word_bits = 64;

    static void set(std::vector<std::uint64_t>& bitmap, std::size_t slot)
    {
        bitmap[slot / word_bits] |= std::uint64_t{1} << (slot % word_bits);
    }
    static bool test(const std::vector<std::uint64_t>& bitmap,
                     std::size_t slot)
    {
        return (bitmap[slot / word_bits] &
                (std::uint64_t{1} << (slot % word_bits))) != 0;
    }
    std::uint64_t valid_bits(std::size_t word) const noexcept
    {
        const auto remaining = slot_count_ - word * word_bits;
        return remaining >= word_bits ? ~std::uint64_t{0}
                                      : (std::uint64_t{1} << remaining) - 1;
    }
    void require_slot(std::size_t slot) const
    {
        if (slot >= slot_count_)
            throw std::out_of_range{"arena slot is outside its extent"};
    }

    std::size_t slot_count_;
    std::vector<std::uint64_t> allocated_;
    std::vector<std::uint64_t> marked_;
    std::vector<std::uint64_t> allocation_generations_;
    mutable std::mutex mutex_;
    std::uint64_t current_generation_ = 0;
    std::uint64_t frozen_generation_ = 0;
    bool collecting_ = false;
};

/*!
 * Process-local view of one fixed-size-slot extent.
 *
 * `owner` controls mapping lifetime. Extent metadata is runtime state and is
 * rebuilt when a process opens a store; only the IDs and slot layout belong
 * in the persistent extent header.
 */
struct storage_extent
{
    std::uint32_t id = 0;
    std::byte* base = nullptr;
    std::size_t size_bytes = 0;
    std::size_t slot_size = 0;
    std::size_t slot_alignment = 0;
    std::uint64_t layout_id = 0;
    std::shared_ptr<void> owner;
    std::function<void*(std::size_t, std::size_t)> allocate_slot;
    std::function<void()> refresh_allocation_state;
    std::function<void()> flush_data;
    std::shared_ptr<arena_slot_bitmap> slot_bitmap;
    // Enabled only when the runtime can provide every reader/process root.
    bool can_reclaim_slots = false;

    std::size_t slot_count() const noexcept
    {
        return slot_size == 0 ? 0 : size_bytes / slot_size;
    }
};

//! Immutable mapping table pinned while a snapshot traverses storage handles.
class extent_snapshot : public immer::storage_ptr_context
{
    using table_type = std::vector<storage_extent>;
    friend class extent_directory;

    explicit extent_snapshot(std::shared_ptr<const table_type> table)
        : table_{std::move(table)}
    {}

public:
    extent_snapshot() = default;

    template <typename T>
    T* resolve(immer::storage_ptr<T> handle,
               std::uint64_t expected_layout_id = 0) const
    {
        if (!handle)
            return nullptr;
        if (!table_)
            throw std::logic_error{"extent snapshot is not pinned"};

        const auto id = handle.extent_id();
        const auto found = std::lower_bound(
            table_->begin(), table_->end(), id, [](const auto& extent,
                                                   std::uint32_t value) {
                return extent.id < value;
            });
        if (found == table_->end() || found->id != id)
            throw std::out_of_range{"storage handle names an unmapped extent"};
        if (expected_layout_id != 0 &&
            found->layout_id != expected_layout_id)
            throw std::invalid_argument{"storage handle layout mismatch"};

        const auto slot = static_cast<std::size_t>(handle.slot_id());
        if (slot >= found->slot_count())
            throw std::out_of_range{"storage handle slot is outside its extent"};
        if constexpr (!std::is_void<std::remove_cv_t<T>>::value) {
            if (sizeof(T) > found->slot_size ||
                alignof(T) > found->slot_alignment)
                throw std::invalid_argument{
                    "storage handle type does not fit its arena slot"};
        }
        auto* address = found->base + slot * found->slot_size;
        if constexpr (!std::is_void<std::remove_cv_t<T>>::value) {
            if (reinterpret_cast<std::uintptr_t>(address) % alignof(T) != 0)
                throw std::invalid_argument{
                    "storage handle resolves to a misaligned object"};
        }
        return reinterpret_cast<T*>(address);
    }

    void* resolve_handle(std::uint64_t bits,
                         std::size_t size,
                         std::size_t alignment) const override
    {
        const auto handle = immer::storage_ptr<void>::from_bits(bits);
        if (!handle)
            return nullptr;
        if (!table_)
            throw std::logic_error{"extent snapshot is not pinned"};
        const auto found = std::lower_bound(
            table_->begin(), table_->end(), handle.extent_id(),
            [](const auto& extent, std::uint32_t value) {
                return extent.id < value;
            });
        if (found == table_->end() || found->id != handle.extent_id())
            throw std::out_of_range{"storage handle names an unmapped extent"};
        if (size > found->slot_size || alignment > found->slot_alignment)
            throw std::invalid_argument{
                "storage handle type does not fit its arena slot"};
        const auto slot = static_cast<std::size_t>(handle.slot_id());
        if (slot >= found->slot_count())
            throw std::out_of_range{"storage handle slot is outside its extent"};
        auto* address = found->base + slot * found->slot_size;
        if (reinterpret_cast<std::uintptr_t>(address) % alignment != 0)
            throw std::invalid_argument{
                "storage handle resolves to a misaligned object"};
        return address;
    }

    std::uint64_t encode_address(const void* address,
                                std::size_t size,
                                std::size_t alignment) const override
    {
        if (address == nullptr)
            return 0;
        if (!table_)
            throw std::logic_error{"extent snapshot is not pinned"};
        const auto location = reinterpret_cast<std::uintptr_t>(address);
        for (const auto& extent : *table_) {
            const auto base = reinterpret_cast<std::uintptr_t>(extent.base);
            if (location < base || location - base >= extent.size_bytes)
                continue;
            if (size > extent.slot_size || alignment > extent.slot_alignment)
                throw std::invalid_argument{
                    "storage pointer type does not fit its arena slot"};
            const auto offset = location - base;
            if (offset % extent.slot_size != 0)
                throw std::invalid_argument{
                    "storage pointer does not name the start of an arena slot"};
            const auto slot = offset / extent.slot_size;
            if (location % alignment != 0)
                throw std::invalid_argument{
                    "storage pointer address is misaligned"};
            if (slot >= extent.slot_count() ||
                slot > std::numeric_limits<std::uint32_t>::max())
                throw std::out_of_range{"storage pointer slot is invalid"};
            if (extent.slot_bitmap &&
                !extent.slot_bitmap->is_allocated(
                    static_cast<std::size_t>(slot)))
                throw std::invalid_argument{
                    "storage pointer names an unallocated arena slot"};
            return immer::storage_ptr<void>::from_parts(
                       extent.id, static_cast<std::uint32_t>(slot))
                .bits();
        }
        throw std::out_of_range{
            "storage pointer address is outside the pinned extents"};
    }

    void* allocate_slot(std::size_t size,
                        std::size_t alignment) const override
    {
        if (!table_)
            throw std::logic_error{"extent snapshot is not pinned"};
        for (const auto& extent : *table_) {
            if (!extent.allocate_slot || size > extent.slot_size ||
                alignment > extent.slot_alignment)
                continue;
            if (auto* slot = extent.allocate_slot(size, alignment)) {
                const auto address = reinterpret_cast<std::uintptr_t>(slot);
                const auto base = reinterpret_cast<std::uintptr_t>(extent.base);
                if (address < base || address - base >= extent.size_bytes ||
                    (address - base) % extent.slot_size != 0 ||
                    address % alignment != 0)
                    throw std::logic_error{
                        "arena allocator returned an invalid storage slot"};
                if (extent.slot_bitmap)
                    extent.slot_bitmap->reserve(
                        (address - base) / extent.slot_size);
                return slot;
            }
        }
        throw std::bad_alloc{};
    }

    //! Flush every durable mapping held by this snapshot before root publish.
    void flush_data() const
    {
        if (!table_)
            throw std::logic_error{"extent snapshot is not pinned"};
        for (const auto& extent : *table_)
            if (extent.flush_data)
                extent.flush_data();
    }

    bool is_durable(immer::storage_ptr<void> handle) const
    {
        if (!handle || !table_)
            return false;
        const auto found = std::lower_bound(
            table_->begin(), table_->end(), handle.extent_id(),
            [](const auto& extent, std::uint32_t id) {
                return extent.id < id;
            });
        if (found == table_->end() || found->id != handle.extent_id() ||
            !found->flush_data || !found->slot_bitmap ||
            !found->refresh_allocation_state ||
            handle.slot_id() >= found->slot_count())
            return false;
        found->refresh_allocation_state();
        return found->slot_bitmap->is_allocated(handle.slot_id());
    }

    bool is_allocated(immer::storage_ptr<void> handle) const
    {
        if (!handle || !table_)
            return false;
        const auto found = std::lower_bound(
            table_->begin(), table_->end(), handle.extent_id(),
            [](const auto& extent, std::uint32_t id) {
                return extent.id < id;
            });
        if (found == table_->end() || found->id != handle.extent_id() ||
            !found->slot_bitmap || handle.slot_id() >= found->slot_count())
            return false;
        if (found->refresh_allocation_state)
            found->refresh_allocation_state();
        return found->slot_bitmap->is_allocated(handle.slot_id());
    }

    void begin_mark() const
    {
        require_table();
        std::vector<std::shared_ptr<arena_slot_bitmap>> started;
        try {
            for (const auto& extent : *table_) {
                if (!extent.slot_bitmap)
                    continue;
                if (extent.refresh_allocation_state)
                    extent.refresh_allocation_state();
                extent.slot_bitmap->begin_mark();
                started.push_back(extent.slot_bitmap);
            }
        } catch (...) {
            for (const auto& bitmap : started)
                bitmap->cancel_mark();
            throw;
        }
    }

    //! Advance every distinct arena to one writer allocation generation.
    std::uint64_t advance_allocation_generation() const
    {
        require_table();
        std::vector<std::shared_ptr<arena_slot_bitmap>> advanced;
        std::optional<std::uint64_t> generation;
        for (const auto& extent : *table_) {
            if (!extent.slot_bitmap ||
                std::find(advanced.begin(), advanced.end(), extent.slot_bitmap) !=
                    advanced.end())
                continue;
            const auto next = extent.slot_bitmap->advance_allocation_generation();
            if (generation && *generation != next)
                throw std::logic_error{
                    "arena allocation generations are out of sync"};
            generation = next;
            advanced.push_back(extent.slot_bitmap);
        }
        if (!generation)
            throw std::logic_error{"storage snapshot has no allocation arenas"};
        return *generation;
    }

    //! Retain all slots allocated by one active transaction generation.
    void mark_generation(std::uint64_t generation) const
    {
        require_table();
        for (const auto& extent : *table_)
            if (extent.slot_bitmap)
                extent.slot_bitmap->mark_generation(generation);
    }

    bool mark(immer::storage_ptr<void> handle) const
    {
        const auto& extent = find_extent(handle);
        if (!extent.slot_bitmap)
            throw std::logic_error{"storage extent has no GC slot bitmap"};
        return extent.slot_bitmap->mark(handle.slot_id());
    }

    std::size_t sweep() const
    {
        require_table();
        std::size_t reclaimed = 0;
        for (const auto& extent : *table_) {
            if (extent.slot_bitmap && extent.can_reclaim_slots)
                reclaimed += extent.slot_bitmap->sweep();
            else if (extent.slot_bitmap)
                extent.slot_bitmap->cancel_mark();
        }
        return reclaimed;
    }

    void cancel_mark() const noexcept
    {
        if (!table_)
            return;
        for (const auto& extent : *table_)
            if (extent.slot_bitmap)
                extent.slot_bitmap->cancel_mark();
    }

    void deallocate_slot(std::size_t, void*) const override
    {
        // Extent slots are reclaimed by the runtime's tracing collector.
    }

    storage_context_scope activate() const;

    explicit operator bool() const noexcept { return bool{table_}; }

private:
    void require_table() const
    {
        if (!table_)
            throw std::logic_error{"extent snapshot is not pinned"};
    }

    const storage_extent& find_extent(immer::storage_ptr<void> handle) const
    {
        require_table();
        if (!handle)
            throw std::invalid_argument{"cannot mark a null storage handle"};
        const auto found = std::lower_bound(
            table_->begin(), table_->end(), handle.extent_id(),
            [](const auto& extent, std::uint32_t id) {
                return extent.id < id;
            });
        if (found == table_->end() || found->id != handle.extent_id() ||
            handle.slot_id() >= found->slot_count())
            throw std::out_of_range{"GC handle is outside a mapped extent"};
        return *found;
    }

    std::shared_ptr<const table_type> table_;
};

//! Resolver scope that owns its extent pin until after the context is removed.
class storage_context_scope
{
public:
    explicit storage_context_scope(const extent_snapshot& extents)
        : extents_{extents}, context_{extents_}
    {}

    storage_context_scope(const storage_context_scope&) = delete;
    storage_context_scope& operator=(const storage_context_scope&) = delete;

private:
    extent_snapshot extents_;
    immer::storage_ptr_context_scope context_;
};

inline storage_context_scope extent_snapshot::activate() const
{
    if (!*this)
        throw std::logic_error{"extent snapshot is not pinned"};
    return storage_context_scope{*this};
}

/*!
 * Publishes append-only mapping metadata without invalidating snapshots.
 *
 * Mapping an extent and managing its backing file/shared-memory object are
 * responsibilities of the caller. Once published, an extent ID and its
 * arena layout are immutable. Replacing the directory table does not move
 * existing mappings; pinned extent snapshots retain their mapping owners.
 */
class extent_directory
{
public:
    extent_directory()
        : current_{std::make_shared<const table_type>()}
    {}

    extent_directory(const extent_directory&) = delete;
    extent_directory& operator=(const extent_directory&) = delete;

    extent_snapshot pin() const noexcept
    {
        return extent_snapshot{
            std::atomic_load_explicit(&current_, std::memory_order_acquire)};
    }

    void publish(storage_extent extent)
    {
        validate(extent);
        std::lock_guard<std::mutex> lock{publish_mutex_};
        auto current =
            std::atomic_load_explicit(&current_, std::memory_order_acquire);
        auto next = std::make_shared<table_type>(*current);
        const auto position = std::lower_bound(
            next->begin(), next->end(), extent.id, [](const auto& item,
                                                       std::uint32_t id) {
                return item.id < id;
            });
        if (position != next->end() && position->id == extent.id)
            throw std::logic_error{"storage extent ID is already published"};
        next->insert(position, std::move(extent));
        std::atomic_store_explicit(
            &current_,
            std::shared_ptr<const table_type>{std::move(next)},
            std::memory_order_release);
    }

private:
    using table_type = std::vector<storage_extent>;

    static void validate(const storage_extent& extent)
    {
        if (extent.id == 0 || extent.base == nullptr || extent.size_bytes == 0 ||
            extent.slot_size == 0 || extent.slot_alignment == 0 ||
            extent.layout_id == 0)
            throw std::invalid_argument{"storage extent metadata is incomplete"};
        if (extent.size_bytes % extent.slot_size != 0 ||
            extent.slot_size % extent.slot_alignment != 0)
            throw std::invalid_argument{
                "storage extent size or slot alignment is invalid"};
        if (extent.slot_bitmap &&
            extent.slot_bitmap->slot_count() != extent.slot_count())
            throw std::invalid_argument{
                "storage extent bitmap does not match its slot count"};
        if (reinterpret_cast<std::uintptr_t>(extent.base) %
            extent.slot_alignment != 0)
            throw std::invalid_argument{"storage extent base is misaligned"};
        if (extent.slot_count() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) +
                std::size_t{1})
            throw std::invalid_argument{"storage extent has too many slots"};
    }

    mutable std::mutex publish_mutex_;
    std::shared_ptr<const table_type> current_;
};

} // namespace lager
