// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <lager/storage.hpp>
#include <lager/root_journal.hpp>
#include <lager/shared_memory.hpp>
#include <lager/extra/persistent.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <typeindex>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace lager {

// Forward declarations keep this storage/runtime header independent from the
// Zug optics implementation while allowing transactions to use Lager paths.
template <typename LensT, typename T, typename U>
decltype(auto) set(LensT&& lens, T&& whole, U&& value);

template <typename LensT, typename T>
decltype(auto) view(LensT&& lens, T&& whole);

template <typename LensT, typename T, typename Fn>
decltype(auto) over(LensT&& lens, T&& whole, Fn&& fn);

//! Identity of a shared transaction/storage domain.
struct stable_store_id
{
    std::uint64_t value = 0;

    constexpr explicit operator bool() const noexcept { return value != 0; }

    friend constexpr bool operator==(stable_store_id lhs,
                                     stable_store_id rhs) noexcept
    {
        return lhs.value == rhs.value;
    }
    friend constexpr bool operator!=(stable_store_id lhs,
                                     stable_store_id rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

//! A process-shared substore whose state is not required to survive restart.
struct shared_ephemeral_policy
{};

//! A file-backed substore whose committed roots survive restart.
struct shared_persistent_policy
{};

/*!
 * A typed handle embedded in the process-local Lager state tree.
 *
 * The handle names a transaction domain. Root resolution and lifetime
 * protection are supplied by the shared-store runtime.
 */
template <typename T, typename Policy>
class substore
{
public:
    using value_type = T;
    using policy_type = Policy;

    constexpr substore() noexcept = default;
    constexpr explicit substore(stable_store_id id) noexcept : id_{id} {}

    constexpr stable_store_id id() const noexcept { return id_; }
    constexpr explicit operator bool() const noexcept
    {
        return static_cast<bool>(id_);
    }

private:
    stable_store_id id_{};
};

template <typename T>
struct is_substore : std::false_type
{};

template <typename T, typename Policy>
struct is_substore<substore<T, Policy>> : std::true_type
{};

//! How reads from other substores should be validated at commit time.
enum class dependency_validation
{
    none,
    ordering,
    exact_generation
};

struct shared_read_dependency
{
    stable_store_id store;
    std::uint64_t generation;
    dependency_validation validation;
    bool persistent;
};

/*!
 * Per-transaction bookkeeping for the single-write-domain rule.
 *
 * Reads can name any number of domains. The first write selects the only
 * domain the transaction may modify. This class records dependency
 * generations and the maximum read watermark; the runtime validates them
 * while committing.
 */
class shared_transaction_scope
{
public:
    void record_read(stable_store_id store,
                     std::uint64_t generation,
                     dependency_validation validation =
                         dependency_validation::exact_generation,
                     bool persistent = false)
    {
        require_valid(store);
        reads_.push_back({store, generation, validation, persistent});
        if (generation > read_watermark_)
            read_watermark_ = generation;
    }

    void record_write(stable_store_id store)
    {
        require_valid(store);
        if (write_domain_ && *write_domain_ != store)
            throw std::logic_error{
                "a Lager transaction may write only one shared substore"};
        write_domain_ = store;
    }

    stable_store_id write_domain() const
    {
        return write_domain_.value_or(stable_store_id{});
    }
    std::uint64_t read_watermark() const noexcept { return read_watermark_; }
    const std::vector<shared_read_dependency>& reads() const noexcept
    {
        return reads_;
    }

private:
    static void require_valid(stable_store_id store)
    {
        if (!store)
            throw std::invalid_argument{"shared substore id must be nonzero"};
    }

    std::vector<shared_read_dependency> reads_;
    std::uint64_t read_watermark_ = 0;
    std::optional<stable_store_id> write_domain_;
};

//! Immutable root published by one successful substore commit.
template <typename T>
struct shared_root_descriptor
{
    std::uint64_t generation;
    std::uint64_t read_watermark;
    std::shared_ptr<const T> root;
    std::uint64_t layout_id;
};

class shared_store_runtime;

template <typename T, typename Policy>
class shared_transaction;

//! A pinned immutable view of a shared substore root.
template <typename T>
class shared_snapshot
{
public:
    using value_type = T;
    using descriptor_type = shared_root_descriptor<T>;

    shared_snapshot() = default;

    const T& get() const
    {
        if (!descriptor_)
            throw std::logic_error{"shared snapshot is empty"};
        return *descriptor_->root;
    }
    const T* operator->() const
    {
        if (!descriptor_)
            throw std::logic_error{"shared snapshot is empty"};
        return descriptor_->root.get();
    }
    const T& operator*() const { return get(); }
    std::uint64_t generation() const noexcept
    {
        return descriptor_ ? descriptor_->generation : 0;
    }
    std::uint64_t read_watermark() const noexcept
    {
        return descriptor_ ? descriptor_->read_watermark : 0;
    }
    std::uint64_t layout_id() const noexcept
    {
        return descriptor_ ? descriptor_->layout_id : 0;
    }
    const extent_snapshot& extents() const noexcept { return extents_; }
    storage_context_scope activate_storage() const
    {
        if (!descriptor_ || !extents_)
            throw std::logic_error{
                "shared snapshot has no active storage mapping"};
        return storage_context_scope{extents_};
    }

    //! Run a batch of reads with this snapshot's relocatable handles active.
    //! The scope is installed once for the callback, so cursor traversal does
    //! not add synchronization to each handle dereference.
    template <typename Fn>
    decltype(auto) with_storage(Fn&& fn) const
    {
        if (!descriptor_)
            throw std::logic_error{"shared snapshot is empty"};
        auto storage_scope = activate_storage();
        return std::forward<Fn>(fn)(get());
    }

    //! Invoke `fn` on a typed persistent root while its mapping is pinned.
    //! References returned by `fn` must not outlive this call or the snapshot.
    template <typename RootT,
              typename Fn,
              typename U = T,
              std::enable_if_t<std::is_same<
                                   U,
                                   immer::storage_ptr<void>>::value,
                               int> = 0>
    decltype(auto) with_root(std::uint64_t expected_layout_id,
                             Fn&& fn) const
    {
        if (!descriptor_)
            throw std::logic_error{"shared snapshot is empty"};
        if (expected_layout_id == 0 ||
            descriptor_->layout_id != expected_layout_id)
            throw std::invalid_argument{
                "shared snapshot root layout does not match"};
        auto storage_scope = activate_storage();
        auto root = descriptor_->root->template rebind<const RootT>();
        if (!root)
            throw std::logic_error{"shared snapshot has a null root handle"};
        return std::forward<Fn>(fn)(*root);
    }
    explicit operator bool() const noexcept
    {
        return static_cast<bool>(descriptor_);
    }

private:
    friend class shared_store_runtime;
    explicit shared_snapshot(std::shared_ptr<const descriptor_type> descriptor,
                             extent_snapshot extents,
                             std::shared_ptr<void> root_pin = {})
        : descriptor_{std::move(descriptor)},
          extents_{std::move(extents)},
          root_pin_{std::move(root_pin)}
    {}

    std::shared_ptr<const descriptor_type> descriptor_;
    extent_snapshot extents_;
    std::shared_ptr<void> root_pin_;
};

//! Thrown when a transaction's write snapshot is no longer the current root.
class shared_write_conflict : public std::runtime_error
{
public:
    shared_write_conflict()
        : std::runtime_error{"shared substore root changed before commit"}
    {}
};

/*!
 * Registry and commit coordinator for typed shared substores.
 *
 * Root descriptors are immutable and readers pin one descriptor before
 * traversal. Writers compute candidate values outside commit locks, then
 * validate dependencies and publish under ordered per-domain locks. This
 * Value roots are process-local. Persistent storage_ptr roots can be restored
 * from and durably committed to a persistent_root_journal.
 */
class shared_store_runtime
{
    struct domain_base
    {
        domain_base(stable_store_id id_, std::type_index type_)
            : id{id_}, type{type_}
        {}
        virtual ~domain_base() = default;

        stable_store_id id;
        std::type_index type;
        std::mutex commit_mutex;
        virtual std::uint64_t current_generation() const = 0;
    };

    template <typename T, typename Policy>
    struct domain final : domain_base
    {
        using descriptor_type = shared_root_descriptor<T>;

        domain(stable_store_id id,
               T initial,
               std::uint64_t generation,
               std::uint64_t read_watermark = 0,
               std::uint64_t layout_id = 0,
               std::shared_ptr<shared_ephemeral_storage> shared_root_channel = {},
               std::shared_ptr<persistent_root_journal>
                   persistent_root_channel = {})
            : domain_base{id, std::type_index{typeid(domain<T, Policy>)}},
              current{std::make_shared<const descriptor_type>(
                  descriptor_type{generation,
                                 read_watermark,
                                 std::make_shared<const T>(
                                     std::move(initial)),
                                 layout_id})},
              shared_root_channel{std::move(shared_root_channel)},
              persistent_root_channel{std::move(persistent_root_channel)}
        {}

        std::uint64_t current_generation() const override
        {
            return std::atomic_load_explicit(&current,
                                             std::memory_order_acquire)
                ->generation;
        }

        std::shared_ptr<const descriptor_type> current;
        std::shared_ptr<shared_ephemeral_storage> shared_root_channel;
        std::shared_ptr<persistent_root_journal> persistent_root_channel;
    };

    using registry_map =
        std::unordered_map<std::uint64_t, std::shared_ptr<domain_base>>;

public:
    shared_store_runtime() = default;
    shared_store_runtime(const shared_store_runtime&) = delete;
    shared_store_runtime& operator=(const shared_store_runtime&) = delete;

    template <typename T, typename Policy>
    substore<T, Policy> create(stable_store_id id, T initial)
    {
        static_assert(!std::is_same<Policy,
                                    shared_ephemeral_policy>::value ||
                          is_persistent_safe<T>::value,
                      "shared ephemeral state must use relocatable persistent-safe types");
        static_assert(!std::is_same<Policy,
                                    shared_persistent_policy>::value,
                      "persistent substores must be created through a durable root journal");
        if (!id)
            throw std::invalid_argument{"shared substore id must be nonzero"};
        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        std::lock_guard<std::mutex> lock{registry_mutex_};
        auto current =
            std::atomic_load_explicit(&domains_, std::memory_order_acquire);
        auto next = std::make_shared<registry_map>(*current);
        if (next->find(id.value) != next->end())
            throw std::logic_error{"shared substore id is already registered"};
        const auto generation = next_generation_unlocked();
        auto value = std::make_shared<domain<T, Policy>>(
            id, std::move(initial), generation);
        next->emplace(id.value, std::move(value));
        std::atomic_store_explicit(
            &domains_, std::shared_ptr<const registry_map>{std::move(next)},
            std::memory_order_release);
        return substore<T, Policy>{id};
    }

    substore<immer::storage_ptr<void>, shared_ephemeral_policy>
    create_shared_ephemeral_root(stable_store_id id,
                                 immer::storage_ptr<void> initial_root,
                                 std::uint64_t layout_id,
                                 shared_ephemeral_storage& root_channel)
    {
        if (!id || !initial_root || layout_id == 0 ||
            !extents_.pin().is_allocated(initial_root))
            throw std::invalid_argument{
                "shared ephemeral root is not in an allocated shared extent"};
        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        std::lock_guard<std::mutex> registry_lock{registry_mutex_};
        auto current =
            std::atomic_load_explicit(&domains_, std::memory_order_acquire);
        if (current->find(id.value) != current->end())
            throw std::logic_error{"shared substore id is already registered"};
        auto next = std::make_shared<registry_map>(*current);
        auto local_channel =
            std::make_shared<shared_ephemeral_storage>(root_channel);
        auto value =
            std::make_shared<domain<immer::storage_ptr<void>,
                                    shared_ephemeral_policy>>(
                id,
                initial_root,
                0,
                0,
                layout_id,
                local_channel);
        next->emplace(id.value, value);
        const auto minimum =
            global_generation_.load(std::memory_order_acquire);
        const auto root = root_channel.publish_root(
            id.value, 0, initial_root, layout_id, 0, {}, minimum);
        auto published = std::make_shared<const shared_root_descriptor<
            immer::storage_ptr<void>>>(shared_root_descriptor<
            immer::storage_ptr<void>>{root.generation,
                                     root.read_watermark,
                                     std::make_shared<const immer::storage_ptr<
                                         void>>(root.root),
                                     root.layout_id});
        std::atomic_store_explicit(&value->current,
                                   std::move(published),
                                   std::memory_order_release);
        const auto generation =
            global_generation_.load(std::memory_order_relaxed);
        if (root.generation > generation)
            global_generation_.store(root.generation,
                                     std::memory_order_release);
        std::atomic_store_explicit(
            &domains_, std::shared_ptr<const registry_map>{std::move(next)},
            std::memory_order_release);
        return substore<immer::storage_ptr<void>,
                        shared_ephemeral_policy>{id};
    }

    substore<immer::storage_ptr<void>, shared_ephemeral_policy>
    attach_shared_ephemeral_root(stable_store_id id,
                                 std::uint64_t expected_layout_id,
                                 shared_ephemeral_storage& root_channel)
    {
        if (!id || expected_layout_id == 0)
            throw std::invalid_argument{
                "shared ephemeral root identity is invalid"};
        const auto root = root_channel.read_root(id.value);
        if (root.layout_id != expected_layout_id ||
            !extents_.pin().is_allocated(root.root))
            throw std::invalid_argument{
                "shared ephemeral root layout or mapping does not match"};

        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        std::lock_guard<std::mutex> registry_lock{registry_mutex_};
        auto current =
            std::atomic_load_explicit(&domains_, std::memory_order_acquire);
        if (current->find(id.value) != current->end())
            throw std::logic_error{"shared substore id is already registered"};
        auto next = std::make_shared<registry_map>(*current);
        auto local_channel =
            std::make_shared<shared_ephemeral_storage>(root_channel);
        auto value =
            std::make_shared<domain<immer::storage_ptr<void>,
                                    shared_ephemeral_policy>>(
                id,
                root.root,
                root.generation,
                root.read_watermark,
                root.layout_id,
                std::move(local_channel));
        next->emplace(id.value, std::move(value));
        const auto generation =
            global_generation_.load(std::memory_order_relaxed);
        if (root.generation > generation)
            global_generation_.store(root.generation,
                                     std::memory_order_release);
        std::atomic_store_explicit(
            &domains_, std::shared_ptr<const registry_map>{std::move(next)},
            std::memory_order_release);
        return substore<immer::storage_ptr<void>,
                        shared_ephemeral_policy>{id};
    }

    substore<immer::storage_ptr<void>, shared_persistent_policy>
    restore_persistent_root(const durable_root_descriptor& descriptor,
                            std::uint64_t expected_layout_id,
                            persistent_root_journal& journal)
    {
        if (descriptor.store_id == 0 || !descriptor.root ||
            descriptor.root.extent_id() == 0 || expected_layout_id == 0 ||
            descriptor.layout_id != expected_layout_id ||
            descriptor.generation == 0 ||
            descriptor.read_watermark >= descriptor.generation)
            throw std::invalid_argument{
                "recovered root does not match the active layout"};
        const auto extents = extents_.pin();
        if (!extents.is_durable(descriptor.root))
            throw std::invalid_argument{
                "recovered root does not point into a mapped durable extent"};
        const stable_store_id id{descriptor.store_id};
        auto value =
            std::make_shared<domain<immer::storage_ptr<void>,
                                    shared_persistent_policy>>(
                id,
                descriptor.root,
                descriptor.generation,
                descriptor.read_watermark,
                descriptor.layout_id);
        value->persistent_root_channel =
            std::make_shared<persistent_root_journal>(journal);
        {
            // Advance the process-wide generation floor before exposing this
            // recovered domain. Otherwise a concurrent create/commit could
            // publish a generation lower than a root that was just restored.
            std::lock_guard<std::mutex> generation_lock{generation_mutex_};
            std::lock_guard<std::mutex> lock{registry_mutex_};
            auto current = std::atomic_load_explicit(
                &domains_, std::memory_order_acquire);
            auto next = std::make_shared<registry_map>(*current);
            if (!next->emplace(id.value, std::move(value)).second)
                throw std::logic_error{
                    "shared substore id is already registered"};
            auto generation =
                global_generation_.load(std::memory_order_relaxed);
            if (descriptor.generation > generation)
                global_generation_.store(descriptor.generation,
                                         std::memory_order_release);
            std::atomic_store_explicit(
                &domains_,
                std::shared_ptr<const registry_map>{std::move(next)},
                std::memory_order_release);
        }
        return substore<immer::storage_ptr<void>,
                        shared_persistent_policy>{id};
    }

    substore<immer::storage_ptr<void>, shared_persistent_policy>
    create_persistent_root(stable_store_id id,
                           immer::storage_ptr<void> initial_root,
                           std::uint64_t layout_id,
                           persistent_root_journal& journal)
    {
        if (!id || !initial_root || initial_root.extent_id() == 0 ||
            layout_id == 0)
            throw std::invalid_argument{
                "persistent substore root identity is invalid"};

        const auto extents = extents_.pin();
        if (!extents.is_durable(initial_root))
            throw std::invalid_argument{
                "persistent root must point into a durable extent"};
        extents.flush_data();

        using root_handle = immer::storage_ptr<void>;
        using policy = shared_persistent_policy;
        using descriptor = shared_root_descriptor<root_handle>;
        const auto domain_id = id;

        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        std::lock_guard<std::mutex> registry_lock{registry_mutex_};
        auto current =
            std::atomic_load_explicit(&domains_, std::memory_order_acquire);
        auto next = std::make_shared<registry_map>(*current);
        if (next->find(id.value) != next->end())
            throw std::logic_error{"shared substore id is already registered"};

        auto value = std::make_shared<domain<root_handle, policy>>(
            domain_id, initial_root, 0, 0, layout_id);
        value->persistent_root_channel =
            std::make_shared<persistent_root_journal>(journal);
        auto root_descriptor = std::make_shared<descriptor>(descriptor{
            0, 0, std::make_shared<const root_handle>(initial_root), layout_id});
        next->emplace(id.value, value);

        const auto durable = journal.publish_next(
            durable_root_descriptor{id.value,
                                    0,
                                    0,
                                    initial_root,
                                    layout_id},
            global_generation_.load(std::memory_order_acquire),
            0);
        root_descriptor->generation = durable.generation;
        std::atomic_store_explicit(
            &value->current,
            std::shared_ptr<const descriptor>{std::move(root_descriptor)},
            std::memory_order_release);
        global_generation_.store(durable.generation,
                                 std::memory_order_release);
        std::atomic_store_explicit(
            &domains_,
            std::shared_ptr<const registry_map>{std::move(next)},
            std::memory_order_release);
        return substore<root_handle, policy>{id};
    }

    //! Reconcile a persistent substore with the append-only durable journal.
    //! Attached readers normally acquire the mapped root cache through
    //! `read(substore)` without filesystem I/O; this operation also repairs
    //! cache state after a writer exits between durable and volatile publish.
    std::uint64_t refresh_persistent_root(
        substore<immer::storage_ptr<void>, shared_persistent_policy> store,
        persistent_root_journal& journal)
    {
        using root_handle = immer::storage_ptr<void>;
        using policy = shared_persistent_policy;
        using descriptor = shared_root_descriptor<root_handle>;

        auto target = find_domain<root_handle, policy>(store.id());
        std::lock_guard<std::mutex> commit_lock{target->commit_mutex};
        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        const auto roots = journal.recover();
        const auto latest = std::find_if(
            roots.begin(), roots.end(), [&](const auto& root) {
                return root.store_id == store.id().value;
            });
        if (latest == roots.end())
            throw std::out_of_range{
                "persistent substore has no recoverable durable root"};

        const auto current = std::atomic_load_explicit(
            &target->current, std::memory_order_acquire);
        if (latest->layout_id != current->layout_id)
            throw std::invalid_argument{
                "durable root layout changed in another process"};
        if (latest->generation < current->generation)
            throw std::runtime_error{
                "persistent root journal is older than the active root"};
        const auto extents = extents_.pin();
        if (!extents.is_durable(latest->root))
            throw std::invalid_argument{
                "durable root is not in a mapped persistent extent"};
        if (latest->generation > current->generation) {
            auto replacement = std::make_shared<const descriptor>(descriptor{
                latest->generation,
                latest->read_watermark,
                std::make_shared<const root_handle>(latest->root),
                latest->layout_id});
            std::atomic_store_explicit(&target->current,
                                       std::move(replacement),
                                       std::memory_order_release);
        }
        const auto global = global_generation_.load(std::memory_order_relaxed);
        if (latest->generation > global)
            global_generation_.store(latest->generation,
                                     std::memory_order_release);
        return latest->generation;
    }

    template <typename T, typename Policy>
    shared_snapshot<T> read(substore<T, Policy> store) const
    {
        auto selected = find_domain<T, Policy>(store.id());
        std::shared_ptr<void> root_pin;
        if constexpr (std::is_same_v<T, immer::storage_ptr<void>> &&
                      std::is_same_v<Policy, shared_ephemeral_policy>) {
            if (selected->shared_root_channel) {
                auto pinned =
                    selected->shared_root_channel->pin_current_root(
                        store.id().value);
                const auto latest = pinned.descriptor;
                if (latest.layout_id !=
                    std::atomic_load_explicit(&selected->current,
                                              std::memory_order_acquire)
                        ->layout_id)
                    throw std::invalid_argument{
                        "shared ephemeral root layout changed in another process"};
                adopt_shared_ephemeral_root(selected, latest);
                root_pin = std::move(pinned.lifetime);
            }
        }
        if constexpr (std::is_same_v<T, immer::storage_ptr<void>> &&
                      std::is_same_v<Policy, shared_persistent_policy>) {
            if (selected->persistent_root_channel) {
                auto pinned = selected->persistent_root_channel->pin_current_root(
                    store.id().value);
                const auto latest = pinned.descriptor;
                if (latest.layout_id !=
                    std::atomic_load_explicit(&selected->current,
                                              std::memory_order_acquire)
                        ->layout_id)
                    throw std::invalid_argument{
                        "durable root layout changed in another process"};
                (void) extents_.pin().resolve(latest.root);
                adopt_persistent_root(selected, latest);
                root_pin = std::move(pinned.lifetime);
            }
        }
        return shared_snapshot<T>{
            std::atomic_load_explicit(&selected->current,
                                      std::memory_order_acquire),
            extents_.pin(),
            std::move(root_pin)};
    }

    //! Resolve a shared substore selected from an ordinary Lager reader or
    //! cursor through one of its standard lens paths.
    template <typename StateCursor,
              typename Path,
              typename Selected = std::decay_t<decltype(view(
                  std::declval<Path>(),
                  std::declval<const StateCursor&>().get()))>,
              std::enable_if_t<is_substore<Selected>::value, int> = 0>
    auto read(const StateCursor& state, Path&& path) const
    {
        return read(view(std::forward<Path>(path), state.get()));
    }

    template <typename T, typename Policy>
    shared_snapshot<T> read(
        substore<T, Policy> store,
        shared_transaction_scope& transaction,
        dependency_validation validation =
            dependency_validation::exact_generation) const
    {
        auto snapshot = read(store);
        transaction.record_read(
            store.id(),
            snapshot.generation(),
            validation,
            std::is_same_v<Policy, shared_persistent_policy>);
        return snapshot;
    }

    template <typename T, typename Policy>
    std::uint64_t commit(substore<T, Policy> store,
                         const shared_snapshot<T>& base,
                         T candidate,
                         const shared_transaction_scope& transaction)
    {
        static_assert(!std::is_same<Policy,
                                    shared_persistent_policy>::value,
                      "persistent substores must commit through their durable root journal");
        if (transaction.write_domain() != store.id())
            throw std::logic_error{
                "transaction write domain does not match committed substore"};

        if constexpr (std::is_same_v<T, immer::storage_ptr<void>> &&
                      std::is_same_v<Policy, shared_ephemeral_policy>) {
            auto selected = find_domain<T, Policy>(store.id());
            if (selected->shared_root_channel)
                return commit_shared_ephemeral_root(
                    store, base, candidate, transaction, selected);
        }

        // Candidate construction can be expensive. Keep it outside the
        // serialization window shared with writers and dependency domains.
        auto immutable_candidate =
            std::make_shared<const T>(std::move(candidate));

        auto target = find_domain<T, Policy>(store.id());
        auto locked_domains = domains_for_commit(transaction, target);
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(locked_domains.size());
        for (const auto& item : locked_domains)
            locks.emplace_back(item->commit_mutex);

        const auto current = std::atomic_load_explicit(
            &target->current, std::memory_order_acquire);
        if (!base.descriptor_ || current != base.descriptor_)
            throw shared_write_conflict{};

        for (const auto& dependency : transaction.reads()) {
            if (dependency.validation !=
                dependency_validation::exact_generation)
                continue;
            const auto actual = find_domain_base(dependency.store)
                                    ->current_generation();
            if (actual != dependency.generation)
                throw shared_write_conflict{};
        }

        auto published = std::make_shared<shared_root_descriptor<T>>(
            shared_root_descriptor<T>{0,
                                      transaction.read_watermark(),
                                      std::move(immutable_candidate),
                                      0});
        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        const auto generation =
            next_generation_unlocked(transaction.read_watermark());
        published->generation = generation;
        std::atomic_store_explicit(
            &target->current,
            std::shared_ptr<const shared_root_descriptor<T>>{
                std::move(published)},
            std::memory_order_release);
        return generation;
    }

    std::uint64_t commit_persistent_root(
        substore<immer::storage_ptr<void>, shared_persistent_policy> store,
        const shared_snapshot<immer::storage_ptr<void>>& base,
        immer::storage_ptr<void> candidate,
        std::uint64_t layout_id,
        const shared_transaction_scope& transaction,
        persistent_root_journal& journal)
    {
        // Persist mapped node data before the journal publishes its root. The
        // journal marker is the durable commit point.
        using root_handle = immer::storage_ptr<void>;
        using descriptor = shared_root_descriptor<root_handle>;

        if (transaction.write_domain() != store.id())
            throw std::logic_error{
                "transaction write domain does not match committed substore"};
        if (!candidate || layout_id == 0)
            throw std::invalid_argument{
                "persistent root handle and layout ID must be valid"};
        if (base.layout_id() != layout_id)
            throw std::invalid_argument{
                "persistent transaction layout does not match its root"};

        // Keep a speculative root visible to tracing collectors throughout
        // flush and publication. Slot reclamation stays disabled until the
        // runtime closes the allocation-to-pin race with a GC handshake.
        auto candidate_pin = journal.pin_root_handle(
            store.id().value, base.generation(), candidate, layout_id);
        (void) candidate_pin;

        const auto extents = extents_.pin();
        if (!extents.is_durable(candidate))
            throw std::invalid_argument{
                "persistent root must point into a durable extent"};
        extents.flush_data();

        auto published = std::make_shared<descriptor>(descriptor{
            0,
            transaction.read_watermark(),
            std::make_shared<const root_handle>(candidate),
            layout_id});

        auto target =
            find_domain<immer::storage_ptr<void>, shared_persistent_policy>(
                store.id());
        auto locked_domains = domains_for_commit(transaction, target);
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(locked_domains.size());
        for (const auto& item : locked_domains)
            locks.emplace_back(item->commit_mutex);

        const auto current = std::atomic_load_explicit(
            &target->current, std::memory_order_acquire);
        if (!base.descriptor_ || current != base.descriptor_)
            throw shared_write_conflict{};
        validate_dependencies(transaction);

        std::lock_guard<std::mutex> generation_lock{generation_mutex_};
        std::vector<durable_read_dependency> durable_reads;
        for (const auto& read : transaction.reads()) {
            if (read.persistent &&
                read.validation == dependency_validation::exact_generation)
                durable_reads.push_back(
                    {read.store.value, read.generation});
        }
        durable_root_descriptor published_root;
        try {
            published_root = journal.publish_next(
                durable_root_descriptor{store.id().value,
                                        0,
                                        transaction.read_watermark(),
                                        candidate,
                                        layout_id},
                global_generation_.load(std::memory_order_acquire),
                base.generation(),
                durable_reads);
        } catch (const durable_root_conflict&) {
            const auto latest_roots = journal.recover();
            const auto adopt_latest = [&](stable_store_id id) {
                const auto latest = std::find_if(
                    latest_roots.begin(),
                    latest_roots.end(),
                    [&](const auto& root) { return root.store_id == id.value; });
                if (latest == latest_roots.end())
                    throw std::runtime_error{
                        "durable root conflict has no recoverable root"};
                if (latest->layout_id != layout_id && id == store.id())
                    throw std::invalid_argument{
                        "durable root layout changed in another process"};

                auto dependency =
                    find_domain<root_handle, shared_persistent_policy>(id);
                const auto local = std::atomic_load_explicit(
                    &dependency->current, std::memory_order_acquire);
                if (latest->layout_id != local->layout_id)
                    throw std::invalid_argument{
                        "durable dependency layout changed in another process"};
                const auto latest_extents = extents_.pin();
                if (!latest_extents.is_durable(latest->root))
                    throw std::invalid_argument{
                        "new durable root is not in a mapped persistent extent"};
                if (latest->generation > local->generation) {
                    auto replacement = std::make_shared<const descriptor>(
                        descriptor{latest->generation,
                                   latest->read_watermark,
                                   std::make_shared<const root_handle>(
                                       latest->root),
                                   latest->layout_id});
                    std::atomic_store_explicit(
                        &dependency->current,
                        std::move(replacement),
                        std::memory_order_release);
                }
                const auto global = global_generation_.load(
                    std::memory_order_relaxed);
                if (latest->generation > global)
                    global_generation_.store(latest->generation,
                                             std::memory_order_release);
            };

            adopt_latest(store.id());
            for (const auto& read : transaction.reads())
                if (read.persistent &&
                    read.validation == dependency_validation::exact_generation)
                    adopt_latest(read.store);
            throw shared_write_conflict{};
        }
        const auto generation = published_root.generation;
        global_generation_.store(generation, std::memory_order_release);
        published->generation = generation;
        std::atomic_store_explicit(
            &target->current,
            std::shared_ptr<const descriptor>{std::move(published)},
            std::memory_order_release);
        return generation;
    }

    std::uint64_t global_generation() const noexcept
    {
        return global_generation_.load(std::memory_order_acquire);
    }

    storage_context_scope activate_storage() const
    {
        return extents_.pin().activate();
    }

    template <typename T, typename Policy>
    shared_transaction<T, Policy> begin_transaction(
        substore<T, Policy> store);

    //! Select a substore from an ordinary Lager reader/cursor with a lens.
    template <typename StateCursor, typename Path>
    auto begin_transaction(const StateCursor& state, Path&& path)
    {
        return begin_transaction(
            view(std::forward<Path>(path), state.get()));
    }

    void publish_extent(storage_extent extent)
    {
        extents_.publish(std::move(extent));
    }

private:
    using shared_ephemeral_handle = immer::storage_ptr<void>;
    using shared_ephemeral_domain =
        domain<shared_ephemeral_handle, shared_ephemeral_policy>;
    using shared_persistent_domain =
        domain<shared_ephemeral_handle, shared_persistent_policy>;

    void adopt_persistent_root(
        const std::shared_ptr<shared_persistent_domain>& target,
        const durable_root_descriptor& latest) const
    {
        using descriptor = shared_root_descriptor<shared_ephemeral_handle>;
        const auto current = std::atomic_load_explicit(
            &target->current, std::memory_order_acquire);
        if (latest.generation < current->generation)
            throw std::runtime_error{
                "durable root generation moved backwards"};
        if (latest.generation == current->generation) {
            if (latest.root != *current->root ||
                latest.read_watermark != current->read_watermark ||
                latest.layout_id != current->layout_id)
                throw std::runtime_error{
                    "durable root cache disagrees with its generation"};
        } else {
            auto replacement = std::make_shared<const descriptor>(descriptor{
                latest.generation,
                latest.read_watermark,
                std::make_shared<const shared_ephemeral_handle>(latest.root),
                latest.layout_id});
            std::atomic_store_explicit(&target->current,
                                       std::move(replacement),
                                       std::memory_order_release);
        }
        auto generation = global_generation_.load(std::memory_order_relaxed);
        while (generation < latest.generation &&
               !global_generation_.compare_exchange_weak(
                   generation,
                   latest.generation,
                   std::memory_order_release,
                   std::memory_order_relaxed))
            ;
    }

    void adopt_shared_ephemeral_root(
        const std::shared_ptr<shared_ephemeral_domain>& target,
        const shared_ephemeral_root_descriptor& latest) const
    {
        using descriptor = shared_root_descriptor<shared_ephemeral_handle>;
        const auto current = std::atomic_load_explicit(
            &target->current, std::memory_order_acquire);
        if (latest.generation < current->generation)
            throw std::runtime_error{
                "shared ephemeral root generation moved backwards"};
        if (latest.generation == current->generation) {
            if (latest.root != *current->root ||
                latest.read_watermark != current->read_watermark ||
                latest.layout_id != current->layout_id)
                throw std::runtime_error{
                    "shared ephemeral root descriptor changed without a new generation"};
        } else {
            auto replacement = std::make_shared<const descriptor>(descriptor{
                latest.generation,
                latest.read_watermark,
                std::make_shared<const shared_ephemeral_handle>(latest.root),
                latest.layout_id});
            std::atomic_store_explicit(&target->current,
                                       std::move(replacement),
                                       std::memory_order_release);
        }
        auto generation = global_generation_.load(std::memory_order_relaxed);
        while (generation < latest.generation &&
               !global_generation_.compare_exchange_weak(
                   generation,
                   latest.generation,
                   std::memory_order_release,
                   std::memory_order_relaxed))
            ;
    }

    std::uint64_t commit_shared_ephemeral_root(
        substore<shared_ephemeral_handle, shared_ephemeral_policy> store,
        const shared_snapshot<shared_ephemeral_handle>& base,
        shared_ephemeral_handle candidate,
        const shared_transaction_scope& transaction,
        const std::shared_ptr<shared_ephemeral_domain>& target)
    {
        if (!candidate || !base.descriptor_ ||
            base.layout_id() == 0 ||
            !extents_.pin().is_allocated(candidate))
            throw std::invalid_argument{
                "shared ephemeral candidate is not in an allocated shared extent"};

        auto candidate_pin = target->shared_root_channel->pin_root_handle(
            store.id().value,
            base.generation(),
            candidate,
            base.layout_id());
        (void) candidate_pin;

        std::lock_guard<std::mutex> commit_lock{target->commit_mutex};
        const auto channel = target->shared_root_channel;
        const auto latest = channel->read_root(store.id().value);
        if (latest.layout_id != base.layout_id())
            throw std::invalid_argument{
                "shared ephemeral transaction layout does not match its root"};
        adopt_shared_ephemeral_root(target, latest);
        if (latest.generation != base.generation() ||
            latest.root != base.get())
            throw shared_write_conflict{};

        std::vector<shared_ephemeral_read_dependency> exact_reads;
        for (const auto& read : transaction.reads()) {
            if (read.validation != dependency_validation::exact_generation)
                continue;
            auto dependency = find_domain_base(read.store);
            auto shared_dependency =
                std::dynamic_pointer_cast<shared_ephemeral_domain>(dependency);
            if (!shared_dependency || !shared_dependency->shared_root_channel ||
                shared_dependency->shared_root_channel->name() !=
                    channel->name())
                throw std::invalid_argument{
                    "exact shared ephemeral dependencies must use the same root channel"};
            exact_reads.push_back({read.store.value, read.generation});
        }

        shared_ephemeral_root_descriptor published;
        try {
            published = channel->publish_root(
                store.id().value,
                base.generation(),
                candidate,
                base.layout_id(),
                transaction.read_watermark(),
                exact_reads,
                global_generation_.load(std::memory_order_acquire));
        } catch (const shared_ephemeral_write_conflict&) {
            const auto current = channel->read_root(store.id().value);
            adopt_shared_ephemeral_root(target, current);
            throw shared_write_conflict{};
        }
        adopt_shared_ephemeral_root(target, published);
        return published.generation;
    }

    std::uint64_t next_generation_unlocked(std::uint64_t minimum = 0)
    {
        const auto current = global_generation_.load(std::memory_order_relaxed);
        const auto floor = std::max(current, minimum);
        if (floor == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error{"shared root generations are exhausted"};
        const auto next = floor + 1;
        global_generation_.store(next, std::memory_order_release);
        return next;
    }

    void validate_dependencies(const shared_transaction_scope& transaction)
        const
    {
        for (const auto& dependency : transaction.reads()) {
            if (dependency.validation !=
                dependency_validation::exact_generation)
                continue;
            const auto actual = find_domain_base(dependency.store)
                                    ->current_generation();
            if (actual != dependency.generation)
                throw shared_write_conflict{};
        }
    }

    std::shared_ptr<domain_base> find_domain_base(stable_store_id id) const
    {
        const auto domains =
            std::atomic_load_explicit(&domains_, std::memory_order_acquire);
        const auto found = domains->find(id.value);
        if (found == domains->end())
            throw std::out_of_range{"shared substore id is not registered"};
        return found->second;
    }

    template <typename T, typename Policy>
    std::shared_ptr<domain<T, Policy>> find_domain(stable_store_id id) const
    {
        auto found = find_domain_base(id);
        auto typed = std::dynamic_pointer_cast<domain<T, Policy>>(found);
        if (!typed)
            throw std::invalid_argument{
                "shared substore type or policy does not match its id"};
        return typed;
    }

    std::vector<std::shared_ptr<domain_base>> domains_for_commit(
        const shared_transaction_scope& transaction,
        const std::shared_ptr<domain_base>& target) const
    {
        auto result = std::vector<std::shared_ptr<domain_base>>{target};
        for (const auto& read : transaction.reads()) {
            if (read.validation == dependency_validation::none)
                continue;
            auto dependency = find_domain_base(read.store);
            if (std::none_of(result.begin(), result.end(), [&](const auto& x) {
                    return x->id == dependency->id;
                }))
                result.push_back(std::move(dependency));
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs,
                                                   const auto& rhs) {
            return lhs->id.value < rhs->id.value;
        });
        return result;
    }

    mutable std::mutex registry_mutex_;
    std::mutex generation_mutex_;
    std::shared_ptr<const registry_map> domains_ =
        std::make_shared<const registry_map>();
    extent_directory extents_;
    mutable std::atomic<std::uint64_t> global_generation_{0};
};

//! Typed transaction over one shared write domain and any number of reads.
template <typename T, typename Policy>
class shared_transaction
{
public:
    using store_type = substore<T, Policy>;
    using snapshot_type = shared_snapshot<T>;

    shared_transaction(shared_store_runtime& runtime, store_type store)
        : runtime_{&runtime}, store_{store}, base_{runtime.read(store)}
    {
        scope_.record_write(store.id());
        scope_.record_read(store.id(),
                           base_.generation(),
                           dependency_validation::exact_generation,
                           std::is_same<Policy,
                                        shared_persistent_policy>::value);
        read_pins_.push_back(std::make_shared<snapshot_type>(base_));
    }

    shared_transaction(const shared_transaction&) = delete;
    shared_transaction& operator=(const shared_transaction&) = delete;
    shared_transaction(shared_transaction&&) = default;
    shared_transaction& operator=(shared_transaction&&) = default;

    const snapshot_type& base() const noexcept { return base_; }
    const shared_transaction_scope& scope() const noexcept { return scope_; }

    template <typename U, typename ReadPolicy>
    shared_snapshot<U> read(
        substore<U, ReadPolicy> store,
        dependency_validation validation =
            dependency_validation::exact_generation)
    {
        ensure_open();
        auto snapshot = runtime_->read(store, scope_, validation);
        read_pins_.push_back(
            std::make_shared<shared_snapshot<U>>(snapshot));
        return snapshot;
    }

    //! Read a substore selected from a process-local Lager cursor by a lens.
    template <typename StateCursor,
              typename Path,
              typename Selected = std::decay_t<decltype(view(
                  std::declval<Path>(),
                  std::declval<const StateCursor&>().get()))>,
              std::enable_if_t<is_substore<Selected>::value, int> = 0>
    auto read(
        const StateCursor& state,
        Path&& path,
        dependency_validation validation =
            dependency_validation::exact_generation)
    {
        return read(view(std::forward<Path>(path), state.get()), validation);
    }

    //! Named form of the cursor-and-lens read overload.
    template <typename StateCursor, typename Path>
    auto read_path(const StateCursor& state,
                   Path&& path,
                   dependency_validation validation =
                       dependency_validation::exact_generation)
    {
        return read(state, std::forward<Path>(path), validation);
    }

    template <typename Fn,
              typename P = Policy,
              std::enable_if_t<!std::is_same<P,
                                             shared_persistent_policy>::value,
                               int> = 0>
    std::uint64_t update(Fn&& fn)
    {
        ensure_open();
        auto candidate = std::invoke(std::forward<Fn>(fn), base_.get());
        return commit(std::move(candidate));
    }

    //! Apply a Lager lens path as a structured update to a shared ephemeral
    //! substore.
    template <typename Path,
              typename Value,
              typename P = Policy,
              std::enable_if_t<!std::is_same<P,
                                             shared_persistent_policy>::value,
                               int> = 0>
    std::uint64_t set_path(Path&& path, Value&& value)
    {
        return update([&](const T& root) {
            return set(std::forward<Path>(path),
                       root,
                       std::forward<Value>(value));
        });
    }

    //! Apply an opaque function at a Lager lens path in a shared ephemeral
    //! substore.
    template <typename Path,
              typename Fn,
              typename P = Policy,
              std::enable_if_t<!std::is_same<P,
                                             shared_persistent_policy>::value,
                               int> = 0>
    std::uint64_t update_path(Path&& path, Fn&& fn)
    {
        return update([&](const T& root) {
            return over(std::forward<Path>(path),
                        root,
                        std::forward<Fn>(fn));
        });
    }

    //! Commit a path update only when its current value matches `expected`.
    template <typename Path,
              typename Expected,
              typename Desired,
              typename P = Policy,
              std::enable_if_t<!std::is_same<P,
                                             shared_persistent_policy>::value,
                               int> = 0>
    std::optional<std::uint64_t> compare_exchange_path(
        Path&& path, const Expected& expected, Desired&& desired)
    {
        ensure_open();
        if (!(view(path, base_.get()) == expected))
            return std::nullopt;
        auto candidate = set(path,
                             base_.get(),
                             std::forward<Desired>(desired));
        return commit(std::move(candidate));
    }

    template <typename U = T,
              typename P = Policy,
              std::enable_if_t<
                  std::is_same<P, shared_persistent_policy>::value &&
                      std::is_same<U, immer::storage_ptr<void>>::value,
                  int> = 0>
    std::uint64_t commit_persistent_root(
        immer::storage_ptr<void> candidate,
        std::uint64_t layout_id,
        persistent_root_journal& journal)
    {
        ensure_open();
        const auto generation = runtime_->commit_persistent_root(
            store_, base_, candidate, layout_id, scope_, journal);
        committed_ = true;
        return generation;
    }

    template <typename RootT,
              typename Fn,
              typename U = T,
              typename P = Policy,
              std::enable_if_t<
                  std::is_same<P, shared_persistent_policy>::value &&
                      std::is_same<U, immer::storage_ptr<void>>::value,
                  int> = 0>
    std::uint64_t update_root(Fn&& fn,
                              std::uint64_t layout_id,
                              persistent_root_journal& journal)
    {
        ensure_open();
        if (base_.layout_id() != layout_id)
            throw std::invalid_argument{
                "persistent transaction layout does not match its root"};
        auto storage_scope = runtime_->activate_storage();
        auto old_root = base_.get().template rebind<const RootT>();
        if (!old_root)
            throw std::logic_error{"persistent transaction has a null root"};
        RootT candidate{std::invoke(std::forward<Fn>(fn), *old_root)};
        const auto new_root = immer::make_storage<RootT>(
            std::move(candidate));
        return commit_persistent_root(new_root.template rebind<void>(),
                                      layout_id,
                                      journal);
    }

    //! Apply an existing Lager lens as a structured path set operation.
    template <typename RootT,
              typename Path,
              typename Value,
              typename U = T,
              typename P = Policy,
              std::enable_if_t<
                  std::is_same<P, shared_persistent_policy>::value &&
                      std::is_same<U, immer::storage_ptr<void>>::value,
                  int> = 0>
    std::uint64_t set_path(Path&& path,
                           Value&& value,
                           std::uint64_t layout_id,
                           persistent_root_journal& journal)
    {
        return update_root<RootT>(
            [&](const RootT& root) {
                return set(std::forward<Path>(path),
                           root,
                           std::forward<Value>(value));
            },
            layout_id,
            journal);
    }

    //! Apply an opaque function to the selected Lager lens path.
    template <typename RootT,
              typename Path,
              typename Fn,
              typename U = T,
              typename P = Policy,
              std::enable_if_t<
                  std::is_same<P, shared_persistent_policy>::value &&
                      std::is_same<U, immer::storage_ptr<void>>::value,
                  int> = 0>
    std::uint64_t update_path(Path&& path,
                              Fn&& fn,
                              std::uint64_t layout_id,
                              persistent_root_journal& journal)
    {
        return update_root<RootT>(
            [&](const RootT& root) {
                return over(std::forward<Path>(path),
                            root,
                            std::forward<Fn>(fn));
            },
            layout_id,
            journal);
    }

    //! Commit a path update only when its current value matches `expected`.
    template <typename RootT,
              typename Path,
              typename Expected,
              typename Desired,
              typename U = T,
              typename P = Policy,
              std::enable_if_t<
                  std::is_same<P, shared_persistent_policy>::value &&
                      std::is_same<U, immer::storage_ptr<void>>::value,
                  int> = 0>
    std::optional<std::uint64_t> compare_exchange_path(
        Path&& path,
        const Expected& expected,
        Desired&& desired,
        std::uint64_t layout_id,
        persistent_root_journal& journal)
    {
        ensure_open();
        auto storage_scope = runtime_->activate_storage();
        auto old_root = base_.get().template rebind<const RootT>();
        if (!old_root)
            throw std::logic_error{"persistent transaction has a null root"};
        if (!(view(std::forward<Path>(path), *old_root) == expected))
            return std::nullopt;
        RootT candidate{set(std::forward<Path>(path),
                            *old_root,
                            std::forward<Desired>(desired))};
        const auto new_root = immer::make_storage<RootT>(
            std::move(candidate));
        return commit_persistent_root(new_root.template rebind<void>(),
                                      layout_id,
                                      journal);
    }

    template <typename U = T,
              typename P = Policy,
              std::enable_if_t<!std::is_same<P,
                                             shared_persistent_policy>::value,
                               int> = 0>
    std::uint64_t commit(T candidate)
    {
        ensure_open();
        const auto generation =
            runtime_->commit(store_, base_, std::move(candidate), scope_);
        committed_ = true;
        return generation;
    }

private:
    void ensure_open() const
    {
        if (committed_)
            throw std::logic_error{"shared transaction was already committed"};
    }

    shared_store_runtime* runtime_;
    store_type store_;
    snapshot_type base_;
    shared_transaction_scope scope_;
    std::vector<std::shared_ptr<void>> read_pins_;
    bool committed_ = false;
};

template <typename T, typename Policy>
shared_transaction<T, Policy> shared_store_runtime::begin_transaction(
    substore<T, Policy> store)
{
    return shared_transaction<T, Policy>{*this, store};
}

} // namespace lager
