// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <lager/extra/persistent.hpp>
#include <lager/shared_state.hpp>

#include <cstdint>
#include <utility>

namespace lager {

//! Access a persistent root using a fingerprint generated from its reflected
//! type, keeping layout selection out of ordinary root reads.
template <typename RootT, typename Fn>
decltype(auto) with_persistent_root(
    const shared_snapshot<immer::storage_ptr<void>>& snapshot,
    const RootT& layout_sample,
    Fn&& fn,
    std::uint32_t schema_version = 1)
{
    const auto layout = persistent_layout(layout_sample, schema_version);
    return snapshot.template with_root<RootT>(
        layout.fingerprint, std::forward<Fn>(fn));
}

//! Update a persistent root while deriving its layout fingerprint from the
//! reflected application type.
template <typename RootT, typename Fn>
std::uint64_t update_persistent_root(
    shared_transaction<immer::storage_ptr<void>, shared_persistent_policy>& tx,
    const RootT& layout_sample,
    Fn&& fn,
    persistent_root_journal& journal,
    std::uint32_t schema_version = 1)
{
    const auto layout = persistent_layout(layout_sample, schema_version);
    return tx.template update_root<RootT>(
        std::forward<Fn>(fn), layout.fingerprint, journal);
}

//! Create a persistent substore from a typed root handle. The layout
//! fingerprint is derived from the object named by the handle, preventing a
//! separate sample object from describing a different root type.
template <typename RootT,
          std::enable_if_t<!std::is_void<RootT>::value, int> = 0>
substore<immer::storage_ptr<void>, shared_persistent_policy>
create_persistent_substore(
    shared_store_runtime& runtime,
    stable_store_id id,
    immer::storage_ptr<RootT> root,
    persistent_root_journal& journal,
    std::uint32_t schema_version = 1)
{
    using root_type = std::remove_cv_t<RootT>;
    if (!root)
        throw std::invalid_argument{"persistent substore root is null"};
    auto storage_scope = runtime.activate_storage();
    const auto layout = persistent_layout<root_type>(*root, schema_version);
    return runtime.create_persistent_root(
        id, root.template rebind<void>(), layout.fingerprint, journal);
}

/*!
 * Create a persistent shared substore after validating the reflected state
 * type and recording its native-layout fingerprint with the initial root.
 */
template <typename T>
substore<immer::storage_ptr<void>, shared_persistent_policy>
create_persistent_substore(shared_store_runtime& runtime,
                           stable_store_id id,
                           immer::storage_ptr<void> root,
                           const T& layout_sample,
                           persistent_root_journal& journal,
                           std::uint32_t schema_version = 1)
{
    if (!root)
        throw std::invalid_argument{"persistent substore root is null"};
    auto storage_scope = runtime.activate_storage();
    const auto typed_root = root.template rebind<const T>();
    (void) typed_root.get();
    const auto layout = persistent_layout(layout_sample, schema_version);
    return runtime.create_persistent_root(
        id, root, layout.fingerprint, journal);
}

/*!
 * Restore a persistent shared substore only when its root was written with
 * the native layout generated for the current executable's reflected type.
 */
template <typename T>
substore<immer::storage_ptr<void>, shared_persistent_policy>
restore_persistent_substore(shared_store_runtime& runtime,
                            const durable_root_descriptor& descriptor,
                            const T& layout_sample,
                            std::uint32_t schema_version = 1,
                            persistent_root_journal* journal = nullptr)
{
    const auto layout = persistent_layout(layout_sample, schema_version);
    if (!descriptor.root)
        throw std::invalid_argument{"recovered persistent root is null"};
    auto storage_scope = runtime.activate_storage();
    auto typed_root = descriptor.root.template rebind<const T>();
    if (!typed_root)
        throw std::invalid_argument{"recovered persistent root is null"};
    (void) typed_root.get();
    return runtime.restore_persistent_root(
        descriptor, layout.fingerprint, journal);
}

template <typename T>
substore<immer::storage_ptr<void>, shared_persistent_policy>
restore_persistent_substore(shared_store_runtime& runtime,
                            const durable_root_descriptor& descriptor,
                            const T& layout_sample,
                            persistent_root_journal& journal,
                            std::uint32_t schema_version = 1)
{
    return restore_persistent_substore(runtime,
                                       descriptor,
                                       layout_sample,
                                       schema_version,
                                       &journal);
}

} // namespace lager
