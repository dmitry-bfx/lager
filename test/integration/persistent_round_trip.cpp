// lager - library for functional interactive C++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#include <lager/extra/persistent_state.hpp>
#include <lager/extra/struct.hpp>
#include <lager/garbage_collector.hpp>
#include <lager/mapped_storage.hpp>

#include <immer/vector.hpp>
#include <immer/vector_transient.hpp>
#include <immer/map.hpp>
#include <immer/set.hpp>
#include <immer/table.hpp>
#include <immer/variant.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace persistent_round_trip {

using values_type = immer::vector<int, immer::storage_memory_policy>;
using map_type = immer::map<std::uint64_t,
                            std::uint64_t,
                            std::hash<std::uint64_t>,
                            std::equal_to<std::uint64_t>,
                            immer::storage_memory_policy>;
using set_type = immer::set<std::uint64_t,
                            std::hash<std::uint64_t>,
                            std::equal_to<std::uint64_t>,
                            immer::storage_memory_policy>;

struct table_value
{
    std::uint64_t id;
    std::uint64_t value;
};

using table_type = immer::table<table_value,
                                immer::table_key_fn,
                                std::hash<std::uint64_t>,
                                std::equal_to<std::uint64_t>,
                                immer::storage_memory_policy>;
using variant_type = immer::variant<std::uint64_t, values_type>;

struct root_state
{
    values_type values;
    map_type map_values;
    set_type set_values;
    table_type table_values;
    variant_type variant_value;
};

} // namespace persistent_round_trip

LAGER_STRUCT(persistent_round_trip,
             table_value,
             (std::uint64_t, id),
             (std::uint64_t, value));
LAGER_STRUCT(persistent_round_trip,
             root_state,
             (values_type, values),
             (map_type, map_values),
             (set_type, set_values),
             (table_type, table_values),
             (variant_type, variant_value));

namespace {

using namespace persistent_round_trip;
constexpr std::uint64_t store_id = 0x4c41474552505431ULL;
constexpr std::uint64_t dependency_store_id = store_id + 1;
constexpr std::size_t value_count = 100000;
constexpr std::size_t indexed_container_count = 1024;
constexpr std::size_t changed_index = 5137;
constexpr int changed_value = -5137;
constexpr std::size_t slot_bytes = 1024;
constexpr std::size_t slot_alignment = 16;
constexpr std::uint64_t extent_layout_id = 0x455854454e543031ULL;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error{message};
}

std::string metadata_path(const std::string& path)
{
    return path + ".mapping";
}

#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
std::pair<void*, std::size_t>& reserved_mapping();
#endif

void write_mapping_metadata(const std::string& path,
                            const lager::storage_extent& extent)
{
#if !defined(_WIN32)
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const auto mapping_begin =
        reinterpret_cast<std::uintptr_t>(extent.base) - page;
    const auto mapping_bytes = page + extent.size_bytes;
    std::ofstream metadata{metadata_path(path), std::ios::trunc};
    require(static_cast<bool>(metadata), "cannot write mapping metadata");
    metadata << mapping_begin << ' ' << mapping_bytes << '\n';
#else
    (void) path;
    (void) extent;
    throw std::runtime_error{"persistent round trip requires POSIX mmap"};
#endif
}

void reserve_writer_address(const std::string& path)
{
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    std::uintptr_t address = 0;
    std::size_t bytes = 0;
    std::ifstream metadata{metadata_path(path)};
    require(static_cast<bool>(metadata), "mapping metadata is missing");
    metadata >> address >> bytes;
    require(address != 0 && bytes != 0, "mapping metadata is invalid");
    auto* reservation = ::mmap(reinterpret_cast<void*>(address),
                               bytes,
                               PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS |
                                   MAP_FIXED_NOREPLACE,
                               -1,
                               0);
    require(reservation != MAP_FAILED,
            "could not reserve the writer's mapping address");
    // Keep the reservation until all extents have been mapped by this reader.
    reserved_mapping() = {reservation, bytes};
#else
    (void) path;
    throw std::runtime_error{
        "this platform does not provide MAP_FIXED_NOREPLACE"};
#endif
}

#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
std::pair<void*, std::size_t>& reserved_mapping()
{
    static std::pair<void*, std::size_t> value{MAP_FAILED, 0};
    return value;
}

void release_reserved_address()
{
    auto& reservation = reserved_mapping();
    if (reservation.first != MAP_FAILED) {
        ::munmap(reservation.first, reservation.second);
        reservation = {MAP_FAILED, 0};
    }
}
#endif

std::vector<lager::mapped_extent> map_existing(
    const std::string& path,
    lager::persistent_storage_file& file,
    lager::shared_store_runtime& runtime,
    bool force_relocation)
{
    if (force_relocation)
        reserve_writer_address(path);
    auto mappings = file.map_existing_extents();
    require(!mappings.empty(), "persistent store contains no extents");
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    if (force_relocation) {
        const auto mapped_data = reinterpret_cast<std::uintptr_t>(
            mappings.front().view().base);
        require(mapped_data != reinterpret_cast<std::uintptr_t>(
                                   reserved_mapping().first) +
                                   static_cast<std::size_t>(
                                       ::sysconf(_SC_PAGESIZE)),
                "reader reused the writer's virtual mapping address");
    }
#endif
    return mappings;
}

struct traced_nodes
{
    std::set<std::uint64_t> bits;

    template <typename T>
    void visit_node(immer::storage_ptr<T> handle)
    {
        bits.insert(handle.bits());
    }

    template <typename T>
    void visit_auxiliary(immer::storage_ptr<T> handle)
    {
        bits.insert(handle.bits());
    }

    template <typename T>
    void visit_value(const T&)
    {}
};

class deterministic_gate
{
public:
    void pause()
    {
        std::unique_lock<std::mutex> lock{mutex_};
        arrived_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
    }

    void signal()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        arrived_ = true;
        condition_.notify_all();
    }

    void wait_until_arrived()
    {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [&] { return arrived_; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool arrived_ = false;
    bool released_ = false;
};

//! Deterministic, process-local coordinator for the first reclaiming arena.
//! It serializes root/pin/transaction changes with root capture and sweep.
class controlled_gc_protocol
    : public std::enable_shared_from_this<controlled_gc_protocol>
{
public:
    using root_handle = immer::storage_ptr<root_state>;

    struct pin_token
    {
        std::size_t slot;
        std::uint64_t sequence;
    };

    struct transaction_token
    {
        std::size_t slot;
        std::uint64_t sequence;
        std::uint64_t base_generation;
        std::uint64_t allocation_generation;
    };

    class snapshot
    {
    public:
        snapshot() = default;
        snapshot(std::shared_ptr<controlled_gc_protocol> owner,
                 pin_token token,
                 root_handle root,
                 std::uint64_t generation)
            : owner_{std::move(owner)},
              token_{token},
              root_{root},
              generation_{generation}
        {}
        snapshot(const snapshot&) = delete;
        snapshot& operator=(const snapshot&) = delete;
        snapshot(snapshot&& other) noexcept
            : owner_{std::move(other.owner_)},
              token_{other.token_},
              root_{other.root_},
              generation_{other.generation_}
        {}
        snapshot& operator=(snapshot&& other) noexcept
        {
            if (this == &other)
                return *this;
            reset();
            owner_ = std::move(other.owner_);
            token_ = other.token_;
            root_ = other.root_;
            generation_ = other.generation_;
            return *this;
        }
        ~snapshot() { reset(); }

        root_handle root() const noexcept { return root_; }
        std::uint64_t generation() const noexcept { return generation_; }
        pin_token token() const noexcept { return token_; }
        void reset() noexcept
        {
            if (owner_)
                owner_->release_pin(token_);
            owner_.reset();
            root_ = {};
        }

    private:
        std::shared_ptr<controlled_gc_protocol> owner_;
        pin_token token_{};
        root_handle root_{};
        std::uint64_t generation_ = 0;
    };

    controlled_gc_protocol(lager::extent_snapshot extents, root_handle initial)
        : extents_{std::move(extents)}, current_{initial}
    {}

    snapshot read(std::function<void()> before_lock = {},
                  std::function<void()> after_pin = {})
    {
        if (before_lock)
            before_lock();
        std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t i = 0; i < pins_.size(); ++i) {
            auto& slot = pins_[i];
            if (slot.active)
                continue;
            if (slot.sequence == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error{"controlled pin sequence exhausted"};
            ++slot.sequence;
            slot.root = current_;
            slot.active = true;
            const pin_token token{i, slot.sequence};
            if (after_pin)
                after_pin();
            return snapshot{shared_from_this(),
                            token,
                            slot.root,
                            current_generation_};
        }
        throw std::length_error{"controlled arena pin table is full"};
    }

    transaction_token begin_transaction()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t i = 0; i < transactions_.size(); ++i) {
            auto& slot = transactions_[i];
            if (slot.active)
                continue;
            if (slot.sequence == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error{
                    "controlled transaction sequence exhausted"};
            ++slot.sequence;
            slot.base = current_;
            slot.candidate = {};
            slot.base_generation = current_generation_;
            slot.allocation_generation =
                extents_.advance_allocation_generation();
            slot.active = true;
            return {i,
                    slot.sequence,
                    current_generation_,
                    slot.allocation_generation};
        }
        throw std::length_error{"controlled arena transaction table is full"};
    }

    void set_candidate(transaction_token token, root_handle candidate)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto& slot = transaction(token);
        slot.candidate = candidate;
    }

    void publish(transaction_token token)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto& slot = transaction(token);
        if (current_generation_ != token.base_generation ||
            current_ != slot.base)
            throw std::runtime_error{"controlled writer candidate is stale"};
        if (!slot.candidate)
            throw std::logic_error{"controlled writer has no candidate root"};
        current_ = slot.candidate;
        ++current_generation_;
        slot.active = false;
        slot.base = {};
        slot.candidate = {};
    }

    std::size_t collect(std::function<void()> after_root_capture = {})
    {
        std::lock_guard<std::mutex> lock{mutex_};
        lager::tracing_collector collector{extents_};
        collector.add_root(current_);
        for (const auto& slot : pins_)
            if (slot.active)
                collector.add_root(slot.root);
        for (const auto& slot : transactions_)
            if (slot.active) {
                collector.add_root(slot.base);
                if (slot.candidate)
                    collector.add_root(slot.candidate);
                collector.add_protected_generation(
                    slot.allocation_generation);
            }
        if (after_root_capture)
            after_root_capture();
        return collector.collect();
    }

    void release_pin(pin_token token) noexcept
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (token.slot >= pins_.size())
            return;
        auto& slot = pins_[token.slot];
        if (!slot.active || slot.sequence != token.sequence)
            return;
        slot.root = {};
        slot.active = false;
    }

    root_handle current() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return current_;
    }

    std::size_t pin_count() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return static_cast<std::size_t>(std::count_if(
            pins_.begin(), pins_.end(), [](const auto& slot) {
                return slot.active;
            }));
    }

private:
    struct pin_slot
    {
        std::uint64_t sequence = 0;
        bool active = false;
        root_handle root{};
    };

    struct transaction_slot
    {
        std::uint64_t sequence = 0;
        bool active = false;
        std::uint64_t base_generation = 0;
        std::uint64_t allocation_generation = 0;
        root_handle base{};
        root_handle candidate{};
    };

    transaction_slot& transaction(transaction_token token)
    {
        if (token.slot >= transactions_.size())
            throw std::out_of_range{"controlled transaction slot is invalid"};
        auto& slot = transactions_[token.slot];
        if (!slot.active || slot.sequence != token.sequence ||
            slot.base_generation != token.base_generation)
            throw std::logic_error{"controlled transaction token is stale"};
        return slot;
    }

    lager::extent_snapshot extents_;
    mutable std::mutex mutex_;
    root_handle current_;
    std::uint64_t current_generation_ = 1;
    std::array<pin_slot, 8> pins_{};
    std::array<transaction_slot, 8> transactions_{};
};

void create_store(const std::string& path)
{
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".roots");
    std::filesystem::remove(metadata_path(path));

    auto file = lager::persistent_storage_file::open(path);
    auto mapping = file.append_extent(slot_bytes,
                                      slot_alignment,
                                      extent_layout_id,
                                      static_cast<std::uint32_t>(
                                          value_count + 40000));
    lager::shared_store_runtime runtime;
    runtime.publish_extent(mapping.view());
    write_mapping_metadata(path, mapping.view());

    {
        auto storage = runtime.activate_storage();
        auto builder = values_type{}.transient();
        for (std::size_t i = 0; i < value_count; ++i)
            builder.push_back(static_cast<int>(i));
        auto values = std::move(builder).persistent();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> map_entries;
        std::vector<std::uint64_t> set_entries;
        std::vector<table_value> table_entries;
        map_entries.reserve(indexed_container_count);
        set_entries.reserve(indexed_container_count);
        table_entries.reserve(indexed_container_count);
        for (std::size_t i = 0; i < indexed_container_count; ++i) {
            const auto key = static_cast<std::uint64_t>(i);
            map_entries.emplace_back(key, key * 3 + 1);
            set_entries.push_back(key * 5 + 2);
            table_entries.push_back({key, key * 7 + 3});
        }
        root_state initial_root{values};
        initial_root.map_values = map_type{map_entries.begin(), map_entries.end()};
        initial_root.set_values = set_type{set_entries.begin(), set_entries.end()};
        initial_root.table_values =
            table_type{table_entries.begin(), table_entries.end()};
        auto variant_builder = values_type{}.transient();
        for (std::size_t i = 0; i < 64; ++i)
            variant_builder.push_back(static_cast<int>(-static_cast<int>(i) - 1));
        initial_root.variant_value = variant_type{
            immer::in_place_index<1>,
            std::move(variant_builder).persistent()};
        const auto persisted_root = lager::persistent_layout(initial_root, 1);
        auto root = immer::make_storage<root_state>(std::move(initial_root));
        auto journal = file.roots();
        auto store = lager::create_persistent_substore(
            runtime, {store_id}, root, journal);
        auto dependency_builder = values_type{}.transient();
        for (std::size_t i = 0; i < 1024; ++i)
            dependency_builder.push_back(8675309 + static_cast<int>(i));
        auto dependency_values = std::move(dependency_builder).persistent();
        auto dependency_root = immer::make_storage<root_state>(
            root_state{std::move(dependency_values)});
        (void) lager::create_persistent_substore(
            runtime, {dependency_store_id}, dependency_root, journal);
        const auto snapshot = runtime.read(store);
        snapshot.with_root<root_state>(
            persisted_root.fingerprint,
            [](const root_state& saved) {
                require(saved.values.size() == value_count,
                        "writer root has the wrong element count");
                require(saved.values[0] == 0 &&
                            saved.values[value_count - 1] ==
                                static_cast<int>(value_count - 1),
                        "writer indexed reads returned incorrect values");
                require(saved.map_values.size() == indexed_container_count &&
                            saved.set_values.size() == indexed_container_count &&
                            saved.table_values.size() == indexed_container_count,
                        "writer HAMT containers have the wrong sizes");
            });
    }
    std::cout << "writer committed " << value_count
              << " values and exited with a populated persistent root\n";
}

void read_store(const std::string& path, bool updated)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = map_existing(path, file, runtime, true);
    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "persistent root was not recovered");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    const auto snapshot = runtime.read(store);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    snapshot.with_root<root_state>(layout.fingerprint, [&](const auto& root) {
        require(root.values.size() == value_count,
                "reader found the wrong number of values");
        for (std::size_t i = 0; i < value_count; ++i) {
            const auto expected = updated && i == changed_index
                                      ? changed_value
                                      : static_cast<int>(i);
            require(root.values[i] == expected,
                    "reader found an incorrect value at index " +
                        std::to_string(i));
        }
        require(root.map_values.size() == indexed_container_count &&
                    root.set_values.size() == indexed_container_count &&
                    root.table_values.size() == indexed_container_count,
                "reader found a HAMT container with the wrong size");
        for (std::size_t i = 0; i < indexed_container_count; ++i) {
            const auto key = static_cast<std::uint64_t>(i);
            require(root.map_values.at(key) == key * 3 + 1,
                    "reader found an incorrect persistent map value");
            require(root.set_values.count(key * 5 + 2) == 1,
                    "reader missed a persistent set value");
            const auto* table_entry = root.table_values.find(key);
            require(table_entry && table_entry->id == key &&
                        table_entry->value == key * 7 + 3,
                    "reader found an incorrect persistent table value");
        }
        require(root.variant_value.index() == 1,
                "reader found the wrong persistent variant tag");
        const auto& variant_values = root.variant_value.template get<1>();
        require(variant_values.size() == 64,
                "reader found the wrong persistent variant payload size");
        for (std::size_t i = 0; i < variant_values.size(); ++i)
            require(variant_values[i] == -static_cast<int>(i) - 1,
                    "reader found an incorrect persistent variant payload");
    });
    std::cout << "separate reader recovered generation "
              << snapshot.generation() << " at relocated address "
              << static_cast<const void*>(mappings.front().view().base)
              << " and verified all values"
              << (updated ? " after update" : " before update") << '\n';
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    release_reserved_address();
#endif
}

void update_store(const std::string& path)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = map_existing(path, file, runtime, true);
    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "persistent root was not recovered");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    auto transaction = runtime.begin_transaction(store);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    const auto new_generation = lager::update_persistent_root<root_state>(
        transaction,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(changed_index, changed_value);
            return updated;
        },
        journal);
    require(new_generation > found->generation,
            "persistent update did not advance generation");
    const auto updated_snapshot = runtime.read(store);
    std::set<std::uint64_t> old_nodes;
    std::set<std::uint64_t> new_nodes;
    transaction.base().with_root<root_state>(layout.fingerprint,
        [&](const auto& old_root) {
            require(old_root.values[changed_index] ==
                        static_cast<int>(changed_index),
                    "pinned old snapshot changed after commit");
            traced_nodes visitor;
            old_root.values.trace_children(visitor);
            old_nodes = std::move(visitor.bits);
        });
    updated_snapshot.with_root<root_state>(layout.fingerprint,
        [&](const auto& new_root) {
            require(new_root.values[changed_index] == changed_value,
                    "updated root does not contain the new value");
            traced_nodes visitor;
            new_root.values.trace_children(visitor);
            new_nodes = std::move(visitor.bits);
        });
    std::vector<std::uint64_t> shared_nodes;
    std::set_intersection(old_nodes.begin(),
                          old_nodes.end(),
                          new_nodes.begin(),
                          new_nodes.end(),
                          std::back_inserter(shared_nodes));
    require(!shared_nodes.empty(),
            "persistent vector update did not preserve shared nodes");
    lager::tracing_collector collector{updated_snapshot.extents()};
    collector.add_persistent_roots<root_state>(journal, layout.fingerprint);
    require(collector.collect() == 0,
            "mapped extents reclaimed slots without a reader-safe sweep");
    std::cout << "updated generation " << new_generation << "; "
              << shared_nodes.size()
              << " Immer nodes remain structurally shared\n";
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    release_reserved_address();
#endif
}

#if !defined(_WIN32)
int shared_ephemeral_writer_child(const std::string& name,
                                  std::uint64_t store_id,
                                  std::uint64_t layout_id,
                                  int value)
{
    auto shared = lager::shared_ephemeral_storage::open(
        name, 73, slot_bytes, slot_alignment, extent_layout_id);
    lager::shared_store_runtime runtime;
    runtime.publish_extent(shared.view());
    auto store = runtime.attach_shared_ephemeral_root(
        {store_id}, layout_id, shared);
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    require(lager::persistent_layout(layout_sample, 1).fingerprint ==
                layout_id,
            "shared ephemeral layout did not match the attached root");
    auto transaction = runtime.begin_transaction(store);
    auto candidate = transaction.base().with_root<root_state>(
        layout_id,
        [value](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, value);
            return updated;
        });
    auto handle = immer::make_storage<root_state>(std::move(candidate));
    (void) transaction.commit(handle.template rebind<void>());
    return 0;
}

int shared_ephemeral_reader_child(const std::string& name,
                                  std::uint64_t store_id,
                                  std::uint64_t layout_id,
                                  int ready_fd,
                                  int release_fd)
{
    auto shared = lager::shared_ephemeral_storage::open(
        name, 73, slot_bytes, slot_alignment, extent_layout_id);
    lager::shared_store_runtime runtime;
    runtime.publish_extent(shared.view());
    auto store = runtime.attach_shared_ephemeral_root(
        {store_id}, layout_id, shared);
    const auto snapshot = runtime.read(store);
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1)
        return 4;
    char release = 0;
    if (::read(release_fd, &release, 1) != 1)
        return 5;
    snapshot.with_root<root_state>(layout_id, [](const auto& root) {
        require(root.values[0] == 1,
                "old shared ephemeral reader snapshot changed after commit");
    });
    ::_exit(0);
    return 0;
}

void shared_ephemeral_process_round_trip()
{
    const auto name = "/lager_ephemeral_" + std::to_string(::getpid());
    constexpr std::uint64_t id = 0x4c41474552455031ULL;
    auto shared = lager::shared_ephemeral_storage::create(
        name, 73, slot_bytes, slot_alignment, extent_layout_id, 4096);
    lager::shared_store_runtime runtime;
    runtime.publish_extent(shared.view());
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout_id = lager::persistent_layout(layout_sample, 1).fingerprint;
    auto initial_values = values_type{}.push_back(1);
    auto initial = immer::make_storage<root_state>(
        root_state{std::move(initial_values)});
    auto store = runtime.create_shared_ephemeral_root(
        {id}, initial.template rebind<void>(), layout_id, shared);
    auto dependency_values = values_type{}.push_back(2);
    auto dependency_root = immer::make_storage<root_state>(
        root_state{std::move(dependency_values)});
    auto dependency = runtime.create_shared_ephemeral_root(
        {id + 1}, dependency_root.template rebind<void>(), layout_id, shared);

    auto stale = runtime.begin_transaction(store);
    auto stale_candidate = stale.base().with_root<root_state>(
        layout_id,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 99);
            return updated;
        });
    auto stale_handle = immer::make_storage<root_state>(
        std::move(stale_candidate));

    int reader_ready[2];
    int reader_release[2];
    require(::pipe(reader_ready) == 0 && ::pipe(reader_release) == 0,
            "could not create shared ephemeral reader pipes");
    const auto reader_child = ::fork();
    require(reader_child >= 0,
            "fork failed for shared ephemeral pinned reader");
    if (reader_child == 0) {
        ::close(reader_ready[0]);
        ::close(reader_release[1]);
        ::_exit(shared_ephemeral_reader_child(name,
                                              id,
                                              layout_id,
                                              reader_ready[1],
                                              reader_release[0]));
    }
    ::close(reader_ready[1]);
    ::close(reader_release[0]);
    char reader_signal = 0;
    require(::read(reader_ready[0], &reader_signal, 1) == 1,
            "shared ephemeral reader did not pin its snapshot");
    auto roots_with_reader = shared.roots_for_gc();
    require(std::any_of(roots_with_reader.begin(),
                        roots_with_reader.end(),
                        [&](const auto& root) {
                            return root.store_id == id &&
                                   root.generation == stale.base().generation() &&
                                   root.root == stale.base().get();
                        }),
            "shared ephemeral pin was not visible to another process");

    const auto child = ::fork();
    require(child >= 0, "fork failed for shared ephemeral store test");
    if (child == 0) {
        try {
            ::_exit(shared_ephemeral_writer_child(name, id, layout_id, 42));
        } catch (...) {
            ::_exit(3);
        }
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "separate shared ephemeral writer failed");
    auto shared_roots = shared.roots_for_gc();
    require(std::any_of(shared_roots.begin(),
                        shared_roots.end(),
                        [&](const auto& root) {
                            return root.store_id == id &&
                                   root.generation == stale.base().generation() &&
                                   root.root == stale.base().get();
                        }),
            "shared ephemeral GC roots omitted an old pinned snapshot");
    auto current = runtime.read(store);
    lager::tracing_collector collector{current.extents()};
    collector.add_shared_ephemeral_roots<root_state>(shared, layout_id);
    require(collector.collect() == 0,
            "shared ephemeral collector reclaimed a mapped slot");
    const char release = 'g';
    require(::write(reader_release[1], &release, 1) == 1,
            "could not release shared ephemeral reader");
    int reader_status = 0;
    require(::waitpid(reader_child, &reader_status, 0) == reader_child &&
                WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0,
            "shared ephemeral pinned reader did not finish cleanly");
    ::close(reader_ready[0]);
    ::close(reader_release[1]);
    const auto roots_after_reader = shared.roots_for_gc();
    const auto old_root_pins = std::count_if(
        roots_after_reader.begin(),
        roots_after_reader.end(),
        [&](const auto& root) {
            return root.store_id == id &&
                   root.generation == stale.base().generation() &&
                   root.root == stale.base().get();
        });
    require(old_root_pins == 1,
            "shared ephemeral reader process left its root pin registered");
    bool conflicted = false;
    try {
        (void) stale.commit(stale_handle.template rebind<void>());
    } catch (const lager::shared_write_conflict&) {
        conflicted = true;
    }
    require(conflicted,
            "shared ephemeral writer accepted a stale process-local root");
    auto latest = runtime.read(store);
    latest.with_root<root_state>(layout_id, [](const auto& root) {
        require(root.values[0] == 42,
                "shared ephemeral read did not observe the other process commit");
    });

    auto stale_dependency = runtime.begin_transaction(store);
    (void) stale_dependency.read(dependency);
    auto stale_dependency_candidate = stale_dependency.base().with_root<root_state>(
        layout_id,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 99);
            return updated;
        });
    auto stale_dependency_handle = immer::make_storage<root_state>(
        std::move(stale_dependency_candidate));
    const auto dependency_child = ::fork();
    require(dependency_child >= 0,
            "fork failed for shared ephemeral dependency test");
    if (dependency_child == 0) {
        try {
            ::_exit(shared_ephemeral_writer_child(
                name, id + 1, layout_id, 24));
        } catch (...) {
            ::_exit(3);
        }
    }
    status = 0;
    require(::waitpid(dependency_child, &status, 0) == dependency_child &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "shared ephemeral dependency writer failed");
    bool dependency_conflicted = false;
    try {
        (void) stale_dependency.commit(
            stale_dependency_handle.template rebind<void>());
    } catch (const lager::shared_write_conflict&) {
        dependency_conflicted = true;
    }
    require(dependency_conflicted,
            "shared ephemeral exact dependency accepted a stale generation");
    latest = runtime.read(store);
    latest.with_root<root_state>(layout_id, [](const auto& root) {
        require(root.values[0] == 42,
                "failed dependency commit changed the target root");
    });
    std::cout << "shared ephemeral reader observed a process-shared commit and "
                 "rejected stale root and dependency candidates\n";
}

void crash_update_child(const std::string& path, const std::string& stage)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings) {
        auto extent = mapping.view();
        if (stage == "data-flush") {
            const auto flush = extent.flush_data;
            extent.flush_data = [flush] {
                flush();
                ::kill(::getpid(), SIGKILL);
                ::_exit(137);
            };
        }
        runtime.publish_extent(std::move(extent));
    }

    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "crash writer could not recover root");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    auto transaction = runtime.begin_transaction(store);
    if (stage == "allocation") {
        const auto layout = lager::persistent_layout(layout_sample, 1);
        auto candidate = transaction.base().with_root<root_state>(
            layout.fingerprint,
            [](const root_state& old_root) {
                auto updated = old_root;
                updated.values =
                    updated.values.set(changed_index, changed_value);
                return updated;
            });
        (void) immer::make_storage<root_state>(std::move(candidate));
        ::kill(::getpid(), SIGKILL);
        ::_exit(137);
    }
    if (stage == "root-record-write" || stage == "root-record-commit" ||
        stage == "publication")
        ::setenv("LAGER_PERSISTENT_FAILPOINT", stage.c_str(), 1);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    (void) lager::update_persistent_root<root_state>(
        transaction,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(changed_index, changed_value);
            return updated;
        },
        journal);
    ::_exit(0);
}

const lager::durable_root_descriptor& root_for(
    const std::vector<lager::durable_root_descriptor>& roots,
    std::uint64_t id);

void crash_case(const std::string& path, const std::string& stage)
{
    create_store(path);
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto storage = runtime.activate_storage();
    auto journal = file.roots();
    const auto roots = journal.recover();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    const auto old_snapshot = runtime.read(store);
    const auto layout = lager::persistent_layout(layout_sample, 1);

    const auto child = ::fork();
    require(child >= 0, "fork failed for crash-boundary test");
    if (child == 0)
        crash_update_child(path, stage);
    int status = 0;
    require(::waitpid(child, &status, 0) == child,
            "waitpid failed for crash-boundary test");
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "crash writer did not terminate at requested boundary");

    // Reuse the already-attached runtime: this exercises repair of the mapped
    // publication cache after a writer dies between the log marker and cache.
    const auto snapshot = runtime.read(store);
    const bool committed = stage == "root-record-commit" ||
                           stage == "publication";
    snapshot.with_root<root_state>(layout.fingerprint, [&](const auto& root) {
        require(root.values.size() == value_count,
                "crash recovery exposed an incomplete vector");
        require(root.values[changed_index] ==
                    (committed ? changed_value
                               : static_cast<int>(changed_index)),
                "crash recovery did not select an atomic root version");
    });
    const auto repaired_roots = journal.recover();
    const auto& repaired_root = root_for(repaired_roots, store_id);
    const auto repaired_snapshot = runtime.read(store);
    require(repaired_snapshot.generation() == repaired_root.generation,
            "attached runtime did not adopt the repaired root cache");
    repaired_snapshot.with_root<root_state>(layout.fingerprint,
        [&](const auto& root) {
            require(root.values[changed_index] ==
                        (committed ? changed_value
                                   : static_cast<int>(changed_index)),
                    "journal repair selected an incorrect root");
        });
    old_snapshot.with_root<root_state>(
        layout.fingerprint,
        [&](const auto& root) {
            require(root.values[changed_index] ==
                        static_cast<int>(changed_index),
                    "crash recovery changed an old pinned snapshot");
        });
    std::cout << "termination at " << stage << " recovered the "
              << (committed ? "new" : "previous") << " complete root\n";
}

const lager::durable_root_descriptor& root_for(
    const std::vector<lager::durable_root_descriptor>& roots,
    std::uint64_t id)
{
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [id](const auto& root) {
                                        return root.store_id == id;
                                    });
    if (found == roots.end())
        throw std::runtime_error{"requested persistent substore was not recovered"};
    return *found;
}

void transaction_semantics(const std::string& path)
{
    create_store(path);
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    auto recovered = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto first = lager::restore_persistent_substore(
        runtime, root_for(recovered, store_id), layout_sample, journal);
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(recovered, dependency_store_id),
        layout_sample,
        journal);

    auto reads_dependency = runtime.begin_transaction(first);
    const auto dependency_snapshot = reads_dependency.read(dependency);
    const auto dependency_generation = dependency_snapshot.generation();
    (void) lager::update_persistent_root<root_state>(
        reads_dependency,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, -1);
            return updated;
        },
        journal);
    recovered = journal.recover();
    const auto& committed = root_for(recovered, store_id);
    require(committed.read_watermark == dependency_generation,
            "commit did not persist the maximum read generation");
    require(committed.generation > committed.read_watermark,
            "commit generation did not advance beyond its read watermark");

    auto stale_reader = runtime.begin_transaction(first);
    (void) stale_reader.read(dependency);
    auto dependency_writer = runtime.begin_transaction(dependency);
    (void) lager::update_persistent_root<root_state>(
        dependency_writer,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 8675310);
            return updated;
        },
        journal);
    bool stale_rejected = false;
    try {
        (void) lager::update_persistent_root<root_state>(
            stale_reader,
            layout_sample,
            [](const root_state& old_root) {
                auto updated = old_root;
                updated.values = updated.values.set(1, -2);
                return updated;
            },
            journal);
    } catch (const lager::shared_write_conflict&) {
        stale_rejected = true;
    }
    require(stale_rejected,
            "exact cross-domain dependency accepted a stale generation");

    lager::shared_transaction_scope single_writer;
    single_writer.record_write({store_id});
    bool second_write_rejected = false;
    try {
        single_writer.record_write({dependency_store_id});
    } catch (const std::logic_error&) {
        second_write_rejected = true;
    }
    require(second_write_rejected,
            "transaction scope accepted two write domains");
    std::cout << "transaction tests confirmed single-writer domains, exact "
                 "cross-domain validation, and durable read watermarks\n";
}

void race_writer_child(const std::string& path,
                       std::size_t index,
                       int ready_fd,
                       int release_fd)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto store = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    auto transaction = runtime.begin_transaction(store);
    const auto candidate = transaction.base().with_root<root_state>(
        layout.fingerprint,
        [index](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(index, -static_cast<int>(index));
            return updated;
        });
    auto candidate_handle = immer::make_storage<root_state>(candidate);
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1)
        ::_exit(4);
    char release = 0;
    if (::read(release_fd, &release, 1) != 1)
        ::_exit(5);
    try {
        (void) transaction.commit_persistent_root(
            candidate_handle.template rebind<void>(),
            layout.fingerprint,
            journal);
        ::_exit(0);
    } catch (const lager::shared_write_conflict&) {
        ::_exit(2);
    } catch (...) {
        ::_exit(3);
    }
}

void competing_writers(const std::string& path)
{
    create_store(path);
    int ready_pipe[2];
    int release_pipe[2];
    require(::pipe(ready_pipe) == 0 && ::pipe(release_pipe) == 0,
            "could not create writer synchronization pipes");
    const std::size_t indexes[] = {101, 202};
    pid_t children[2] = {-1, -1};
    for (std::size_t i = 0; i < 2; ++i) {
        children[i] = ::fork();
        require(children[i] >= 0, "fork failed for competing writers");
        if (children[i] == 0) {
            ::close(ready_pipe[0]);
            ::close(release_pipe[1]);
            race_writer_child(path,
                              indexes[i],
                              ready_pipe[1],
                              release_pipe[0]);
        }
    }
    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);
    char ready = 0;
    require(::read(ready_pipe[0], &ready, 1) == 1 &&
                ::read(ready_pipe[0], &ready, 1) == 1,
            "competing writers did not prepare both candidates");
    const char release[2] = {'g', 'g'};
    require(::write(release_pipe[1], release, sizeof(release)) == 2,
            "could not release competing writers");
    int statuses[2]{};
    for (std::size_t i = 0; i < 2; ++i)
        require(::waitpid(children[i], &statuses[i], 0) == children[i],
                "waitpid failed for competing writers");
    const auto success_count =
        static_cast<int>(WIFEXITED(statuses[0]) && WEXITSTATUS(statuses[0]) == 0) +
        static_cast<int>(WIFEXITED(statuses[1]) && WEXITSTATUS(statuses[1]) == 0);
    const auto conflict_count =
        static_cast<int>(WIFEXITED(statuses[0]) && WEXITSTATUS(statuses[0]) == 2) +
        static_cast<int>(WIFEXITED(statuses[1]) && WEXITSTATUS(statuses[1]) == 2);
    require(success_count == 1 && conflict_count == 1,
            "competing writers did not produce one commit and one conflict");
    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);

    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto store = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    runtime.read(store).with_root<root_state>(layout.fingerprint,
        [](const auto& root) {
            const bool first_won = root.values[101] == -101;
            const bool second_won = root.values[202] == -202;
            require(first_won != second_won,
                    "stale candidate lost a committed update");
        });
    std::cout << "competing processes produced one durable commit and one "
                 "stale-candidate conflict\n";
}

void cross_dependency_writer_child(const std::string& path,
                                   int ready_fd,
                                   int release_fd)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto first = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(roots, dependency_store_id),
        layout_sample,
        journal);
    auto transaction = runtime.begin_transaction(first);
    (void) transaction.read(dependency);
    const auto candidate = transaction.base().with_root<root_state>(
        layout.fingerprint,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, -10);
            return updated;
        });
    auto candidate_handle = immer::make_storage<root_state>(candidate);
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1)
        ::_exit(4);
    char release = 0;
    if (::read(release_fd, &release, 1) != 1)
        ::_exit(5);
    try {
        (void) transaction.commit_persistent_root(
            candidate_handle.template rebind<void>(),
            layout.fingerprint,
            journal);
        ::_exit(0);
    } catch (const lager::shared_write_conflict&) {
        ::_exit(2);
    } catch (...) {
        ::_exit(3);
    }
}

void cross_process_dependency(const std::string& path)
{
    create_store(path);
    int ready_pipe[2];
    int release_pipe[2];
    require(::pipe(ready_pipe) == 0 && ::pipe(release_pipe) == 0,
            "could not create dependency synchronization pipes");
    const auto child = ::fork();
    require(child >= 0, "fork failed for cross-domain dependency test");
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);
        cross_dependency_writer_child(
            path, ready_pipe[1], release_pipe[0]);
    }
    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);
    char ready = 0;
    require(::read(ready_pipe[0], &ready, 1) == 1,
            "cross-domain transaction did not prepare its candidate");
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto& initial_dependency = root_for(roots,
                                              dependency_store_id);
    auto child_pins = journal.pinned_roots();
    require(std::any_of(child_pins.begin(),
                        child_pins.end(),
                        [&](const auto& pin) {
                            return pin.store_id == dependency_store_id &&
                                   pin.generation ==
                                       initial_dependency.generation &&
                                   pin.root == initial_dependency.root;
                        }),
            "another process's transaction read was absent from GC roots");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(roots, dependency_store_id),
        layout_sample,
        journal);
    const auto before_remote_commit = runtime.read(dependency).generation();
    {
        auto transaction = runtime.begin_transaction(dependency);
        (void) lager::update_persistent_root<root_state>(
            transaction,
            layout_sample,
            [](const root_state& old_root) {
                auto updated = old_root;
                updated.values = updated.values.set(0, 8675311);
                return updated;
            },
            journal);
    }
    const auto remotely_updated = runtime.read(dependency);
    require(remotely_updated.generation() > before_remote_commit,
            "persistent read did not acquire the other process root");
    remotely_updated.with_root<root_state>(
        lager::persistent_layout(layout_sample, 1).fingerprint,
        [](const auto& root) {
    require(root.values[0] == 8675311,
                    "persistent read saw a stale process-local root");
        });
    child_pins = journal.pinned_roots();
    require(std::any_of(child_pins.begin(),
                        child_pins.end(),
                        [&](const auto& pin) {
                            return pin.store_id == dependency_store_id &&
                                   pin.generation ==
                                       initial_dependency.generation &&
                                   pin.root == initial_dependency.root;
                        }),
            "committing a new root hid an old process-pinned dependency");
    lager::tracing_collector collector{remotely_updated.extents()};
    collector.add_persistent_roots<root_state>(
        journal,
        lager::persistent_layout(layout_sample, 1).fingerprint);
    require(collector.collect() == 0,
            "collector reclaimed slots while another process pinned an old root");
    const char release = 'g';
    require(::write(release_pipe[1], &release, 1) == 1,
            "could not release cross-domain transaction");
    int status = 0;
    require(::waitpid(child, &status, 0) == child,
            "waitpid failed for cross-domain transaction");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 2,
            "cross-process exact dependency did not reject stale commit");
    child_pins = journal.pinned_roots();
    require(std::none_of(child_pins.begin(),
                         child_pins.end(),
                         [&](const auto& pin) {
                             return pin.store_id == dependency_store_id &&
                                    pin.generation ==
                                        initial_dependency.generation &&
                                    pin.root == initial_dependency.root;
                         }),
            "dead reader process left its root pin registered");
    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);

    const auto latest = journal.recover();
    auto first = lager::restore_persistent_substore(
        runtime, root_for(latest, store_id), layout_sample, journal);
    runtime.read(first).with_root<root_state>(
        lager::persistent_layout(layout_sample, 1).fingerprint,
        [](const auto& root) {
            require(root.values[0] == 0,
                    "stale cross-domain writer changed its target root");
        });
    std::cout << "exact cross-domain reads rejected a stale process-local "
                 "candidate through the durable journal\n";
}

int persistent_graft_reader_child(const std::string& path,
                                  std::uint64_t layout_id,
                                  int ready_fd,
                                  int release_fd)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(roots, dependency_store_id),
        layout_sample,
        journal);
    const auto snapshot = runtime.read(dependency);
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1)
        return 4;
    char release = 0;
    if (::read(release_fd, &release, 1) != 1)
        return 5;
    snapshot.with_root<root_state>(layout_id, [](const auto& old_root) {
        require(old_root.values.size() == 1024 &&
                    old_root.values[0] == 8675309,
                "pinned persistent reader lost its old subtree");
    });
    return 0;
}

void persistent_snapshot_graft(const std::string& path)
{
    create_store(path);
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto target = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(roots, dependency_store_id),
        layout_sample,
        journal);
    const auto old_dependency = root_for(roots, dependency_store_id);

    int ready_pipe[2];
    int release_pipe[2];
    require(::pipe(ready_pipe) == 0 && ::pipe(release_pipe) == 0,
            "could not create persistent graft reader pipes");
    const auto reader = ::fork();
    require(reader >= 0, "fork failed for persistent graft reader");
    if (reader == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);
        try {
            ::_exit(persistent_graft_reader_child(path,
                                                  layout.fingerprint,
                                                  ready_pipe[1],
                                                  release_pipe[0]));
        } catch (...) {
            ::_exit(3);
        }
    }
    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);
    char ready = 0;
    require(::read(ready_pipe[0], &ready, 1) == 1,
            "persistent graft reader did not pin its old snapshot");
    auto pins = journal.pinned_roots();
    require(std::any_of(pins.begin(), pins.end(), [&](const auto& pin) {
                return pin.store_id == dependency_store_id &&
                       pin.generation == old_dependency.generation &&
                       pin.root == old_dependency.root;
            }),
            "persistent graft reader pin was not process-shared");

    {
        const auto old_snapshot = runtime.read(dependency);
        auto transaction = runtime.begin_transaction(target);
        std::set<std::uint64_t> old_nodes;
        auto grafted_values = old_snapshot.with_root<root_state>(
            layout.fingerprint,
            [&](const auto& old_root) {
                traced_nodes visitor;
                old_root.values.trace_children(visitor);
                old_nodes = std::move(visitor.bits);
                return old_root.values;
            });
        auto candidate = immer::make_storage<root_state>(
            root_state{std::move(grafted_values)});

        auto dependency_update = runtime.begin_transaction(dependency);
        (void) lager::update_persistent_root<root_state>(
            dependency_update,
            layout_sample,
            [](const root_state& old_root) {
                auto updated = old_root;
                updated.values = updated.values.set(0, 8675312);
                return updated;
            },
            journal);
        (void) transaction.commit_persistent_root(
            candidate.template rebind<void>(),
            layout.fingerprint,
            journal);
        const auto committed = runtime.read(target);
        committed.with_root<root_state>(layout.fingerprint,
            [&](const auto& new_root) {
                traced_nodes visitor;
                new_root.values.trace_children(visitor);
                std::vector<std::uint64_t> shared_nodes;
                std::set_intersection(old_nodes.begin(),
                                      old_nodes.end(),
                                      visitor.bits.begin(),
                                      visitor.bits.end(),
                                      std::back_inserter(shared_nodes));
                require(!shared_nodes.empty(),
                        "grafted persistent vector did not share tree nodes");
            });
    }

    auto current_target = runtime.read(target);
    current_target.with_root<root_state>(layout.fingerprint,
        [](const auto& root) {
            require(root.values.size() == 1024 &&
                        root.values[0] == 8675309 &&
                        root.values[1023] == 8676332,
                    "new root did not retain the grafted old subtree");
        });
    lager::tracing_collector collector{current_target.extents()};
    collector.add_persistent_roots<root_state>(journal,
                                               layout.fingerprint);
    pins = journal.pinned_roots();
    require(std::any_of(pins.begin(), pins.end(), [&](const auto& pin) {
                return pin.store_id == dependency_store_id &&
                       pin.generation == old_dependency.generation &&
                       pin.root == old_dependency.root;
            }),
            "GC root capture omitted the pinned old graft source");
    require(collector.collect() == 0,
            "persistent collector reclaimed a pinned graft source");

    const char release = 'g';
    require(::write(release_pipe[1], &release, 1) == 1,
            "could not release persistent graft reader");
    int status = 0;
    require(::waitpid(reader, &status, 0) == reader && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "persistent graft reader did not retain its old snapshot");
    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);
    pins = journal.pinned_roots();
    require(std::none_of(pins.begin(), pins.end(), [&](const auto& pin) {
                return pin.store_id == dependency_store_id &&
                       pin.generation == old_dependency.generation &&
                       pin.root == old_dependency.root;
            }),
            "finished persistent graft reader left its pin registered");
    std::cout << "persistent commit grafted a subtree from an old pinned "
                 "snapshot while GC captured its roots\n";
}

void controlled_arena_reclamation()
{
    lager::arena_slot_bitmap generation_bitmap{4};
    const auto abandoned_before_collection = generation_bitmap.allocate();
    require(abandoned_before_collection.has_value(),
            "generation arena failed its initial allocation");
    generation_bitmap.begin_mark();
    const auto allocated_during_collection = generation_bitmap.allocate();
    require(allocated_during_collection.has_value(),
            "generation arena failed its concurrent allocation");
    (void) generation_bitmap.sweep();
    require(!generation_bitmap.is_allocated(*abandoned_before_collection) &&
                generation_bitmap.is_allocated(*allocated_during_collection),
            "collection reclaimed a post-capture generation allocation");
    generation_bitmap.begin_mark();
    (void) generation_bitmap.sweep();
    require(!generation_bitmap.is_allocated(*allocated_during_collection),
            "unrooted post-capture allocation survived the next generation");

    constexpr std::uint32_t local_extent_id = 91;
    constexpr std::size_t local_slot_size = 1024;
    constexpr std::size_t local_slot_alignment = 16;
    constexpr std::size_t local_slot_count = 65536;
    constexpr std::uint64_t local_layout_id = 0x434f4e54524f4c31ULL;
    const auto byte_count = local_slot_size * local_slot_count;
    auto* local_base = static_cast<std::byte*>(
        ::operator new(byte_count, std::align_val_t{local_slot_alignment}));
    std::shared_ptr<void> local_owner{
        local_base,
        [](void* memory) {
            ::operator delete(memory,
                              std::align_val_t{local_slot_alignment});
        }};
    auto bitmap = std::make_shared<lager::arena_slot_bitmap>(
        local_slot_count);
    lager::storage_extent local_extent;
    local_extent.id = local_extent_id;
    local_extent.base = local_base;
    local_extent.size_bytes = byte_count;
    local_extent.slot_size = local_slot_size;
    local_extent.slot_alignment = local_slot_alignment;
    local_extent.layout_id = local_layout_id;
    local_extent.owner = local_owner;
    local_extent.slot_bitmap = bitmap;
    local_extent.allocate_slot = [local_base, bitmap](std::size_t size,
                                                       std::size_t alignment) {
        if (size > local_slot_size || alignment > local_slot_alignment) {
            return static_cast<void*>(nullptr);
        }
        const auto slot = bitmap->allocate();
        return slot ? static_cast<void*>(local_base +
                                         *slot * local_slot_size)
                    : nullptr;
    };
    local_extent.refresh_allocation_state = [] {};
    local_extent.can_reclaim_slots = true;

    lager::shared_store_runtime runtime;
    runtime.publish_extent(std::move(local_extent));
    auto metadata_store = runtime.create<int, lager::shared_ephemeral_policy>(
        {0x434f4e54524f4c31ULL}, 0);
    const auto metadata_snapshot = runtime.read(metadata_store);
    auto extents = metadata_snapshot.extents();
    auto active_storage = runtime.activate_storage();

    auto make_root = [&](std::size_t count, int start) {
        auto storage_scope = runtime.activate_storage();
        auto builder = values_type{}.transient();
        for (std::size_t i = 0; i < count; ++i)
            builder.push_back(start + static_cast<int>(i));
        return immer::make_storage<root_state>(root_state{
            std::move(builder).persistent()});
    };

    auto initial = make_root(4096, 0);
    auto protocol = std::make_shared<controlled_gc_protocol>(extents, initial);
    const auto orphan = make_root(4096, 20000);
    const auto transaction = protocol->begin_transaction();
    immer::storage_ptr<root_state> candidate;
    {
        auto storage_scope = runtime.activate_storage();
        auto changed = initial->values.set(0, -1);
        candidate = immer::make_storage<root_state>(
            root_state{std::move(changed)});
    }
    traced_nodes old_nodes;
    initial->values.trace_children(old_nodes);
    traced_nodes candidate_nodes;
    candidate->values.trace_children(candidate_nodes);
    std::vector<std::uint64_t> grafted_nodes;
    std::set_intersection(old_nodes.bits.begin(),
                          old_nodes.bits.end(),
                          candidate_nodes.bits.begin(),
                          candidate_nodes.bits.end(),
                          std::back_inserter(grafted_nodes));
    require(!grafted_nodes.empty(),
            "controlled candidate did not graft old vector nodes");

    std::vector<bool> allocated_before(local_slot_count);
    for (std::size_t slot = 0; slot < local_slot_count; ++slot)
        allocated_before[slot] = bitmap->is_allocated(slot);

    deterministic_gate root_capture_gate;
    std::size_t first_reclaimed = 0;
    std::exception_ptr collector_error;
    std::thread first_collector([&] {
        try {
            first_reclaimed = protocol->collect(
                [&] { root_capture_gate.pause(); });
        } catch (...) {
            collector_error = std::current_exception();
        }
    });
    root_capture_gate.wait_until_arrived();

    deterministic_gate reader_attempt_gate;
    std::optional<controlled_gc_protocol::snapshot> old_reader;
    std::exception_ptr reader_error;
    std::thread acquiring_reader([&] {
        try {
            old_reader.emplace(protocol->read(
                [&] { reader_attempt_gate.signal(); }));
        } catch (...) {
            reader_error = std::current_exception();
        }
    });
    reader_attempt_gate.wait_until_arrived();
    root_capture_gate.release();
    first_collector.join();
    acquiring_reader.join();
    if (collector_error)
        std::rethrow_exception(collector_error);
    if (reader_error)
        std::rethrow_exception(reader_error);
    require(old_reader.has_value(),
            "reader did not acquire a snapshot after root capture");
    require(old_reader->root() == initial,
            "reader did not acquire the root protected by the collection");
    require(first_reclaimed > 0,
            "controlled collection did not reclaim unreachable slots");
    require(!extents.is_allocated(orphan.template rebind<void>()),
            "controlled collection retained an unreachable root slot");
    require(extents.is_allocated(candidate.template rebind<void>()),
            "GC reclaimed an unregistered candidate allocation generation");
    protocol->set_candidate(transaction, candidate);

    deterministic_gate publication_capture_gate;
    std::size_t second_reclaimed = 0;
    collector_error = nullptr;
    std::thread second_collector([&] {
        try {
            second_reclaimed = protocol->collect(
                [&] { publication_capture_gate.pause(); });
        } catch (...) {
            collector_error = std::current_exception();
        }
    });
    publication_capture_gate.wait_until_arrived();
    deterministic_gate writer_attempt_gate;
    std::exception_ptr writer_error;
    std::thread late_writer([&] {
        try {
            writer_attempt_gate.signal();
            protocol->publish(transaction);
        } catch (...) {
            writer_error = std::current_exception();
        }
    });
    writer_attempt_gate.wait_until_arrived();
    publication_capture_gate.release();
    second_collector.join();
    late_writer.join();
    if (collector_error)
        std::rethrow_exception(collector_error);
    if (writer_error)
        std::rethrow_exception(writer_error);
    (void) second_reclaimed;
    require(protocol->current() == candidate,
            "writer failed to publish its pre-capture candidate");
    require(extents.is_allocated(candidate.template rebind<void>()),
            "GC reclaimed a candidate published after root capture");
    require(old_reader->root()->values[0] == 0,
            "old reader lost its root while collection and commit raced");
    require(protocol->pin_count() == 1,
            "reader pin disappeared before snapshot release");

    const auto old_token = old_reader->token();
    old_reader.reset();
    auto current_reader = protocol->read();
    require(current_reader.token().slot == old_token.slot &&
                current_reader.token().sequence != old_token.sequence,
            "released reader pin slot was not immediately reusable");
    protocol->release_pin(old_token);
    require(protocol->pin_count() == 1 &&
                current_reader.root() == candidate,
            "stale release cleared a newly reused reader pin");

    auto replacement = make_root(4096, 40000);
    const auto replacement_transaction = protocol->begin_transaction();
    protocol->set_candidate(replacement_transaction, replacement);
    protocol->publish(replacement_transaction);
    (void) protocol->collect();
    require(extents.is_allocated(candidate.template rebind<void>()),
            "GC reclaimed a subtree still pinned by a reader");
    require(current_reader.root()->values[0] == -1,
            "GC reclaimed a grafted node reachable from the pinned root");

    current_reader.reset();
    (void) protocol->collect();
    std::vector<std::size_t> freed_slots;
    for (std::size_t slot = 0; slot < local_slot_count; ++slot)
        if (allocated_before[slot] && !bitmap->is_allocated(slot))
            freed_slots.push_back(slot);
    require(!freed_slots.empty(),
            "controlled sweep did not expose unreachable slots for reuse");
    auto recycled = make_root(1, 50000);
    require(std::find(freed_slots.begin(),
                      freed_slots.end(),
                      recycled.slot_id()) != freed_slots.end(),
            "controlled arena did not reuse a swept slot");
    require(replacement->values[0] == 40000,
            "live replacement root was damaged by slot reuse");
    std::cout << "controlled arena preserved reader, candidate, and grafted "
                 "nodes; reclaimed " << freed_slots.size()
              << " unreachable slots and reused one\n";
}
#endif

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2)
            throw std::invalid_argument{"persistent round-trip mode is required"};
        const std::string mode{argv[1]};
        if (mode == "ephemeral" && argc == 2) {
#if !defined(_WIN32)
            shared_ephemeral_process_round_trip();
            return 0;
#else
            throw std::runtime_error{"shared ephemeral test requires POSIX"};
#endif
        }
        if (mode == "controlled-gc" && argc == 2) {
            controlled_arena_reclamation();
            return 0;
        }
        if (argc != 3 && argc != 4)
            throw std::invalid_argument{
                "usage: persistent-round-trip write|update|read STORE [initial|updated]"};
        const std::string path{argv[2]};
        if (mode == "write" && argc == 3)
            create_store(path);
        else if (mode == "update" && argc == 3)
            update_store(path);
#if !defined(_WIN32)
        else if (mode == "ephemeral" && argc == 3)
            shared_ephemeral_process_round_trip();
        else if (mode == "crash" && argc == 4)
            crash_case(path, argv[3]);
        else if (mode == "transactions" && argc == 3)
            transaction_semantics(path);
        else if (mode == "race" && argc == 3)
            competing_writers(path);
        else if (mode == "cross-dependency" && argc == 3)
            cross_process_dependency(path);
        else if (mode == "graft" && argc == 3)
            persistent_snapshot_graft(path);
#endif
        else if (mode == "read" && argc == 4) {
            const std::string stage{argv[3]};
            if (stage != "initial" && stage != "updated")
                throw std::invalid_argument{"reader stage must be initial or updated"};
            read_store(path, stage == "updated");
        } else
            throw std::invalid_argument{"invalid persistent round-trip command"};
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent round trip failed: " << error.what() << '\n';
        return 1;
    }
}
