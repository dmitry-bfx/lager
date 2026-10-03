// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <lager/extra/persistent.hpp>
#include <lager/shared_state.hpp>
#include <lager/storage.hpp>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lager {
namespace gc_detail {

template <typename T, typename Visitor, typename = void>
struct has_trace_children : std::false_type
{};

template <typename T, typename Visitor>
struct has_trace_children<
    T,
    Visitor,
    std::void_t<decltype(std::declval<const T&>().trace_children(
        std::declval<Visitor&>()))>> : std::true_type
{};

template <typename T>
struct is_storage_ptr : std::false_type
{};

template <typename T>
struct is_storage_ptr<immer::storage_ptr<T>> : std::true_type
{};

template <typename T>
struct is_typed_storage_ptr : std::false_type
{};

template <typename T>
struct is_typed_storage_ptr<immer::storage_ptr<T>>
    : std::integral_constant<bool, !std::is_void<T>::value>
{};

template <typename T>
struct is_std_array : std::false_type
{};
template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type
{};

template <typename T>
struct is_std_pair : std::false_type
{};
template <typename T, typename U>
struct is_std_pair<std::pair<T, U>> : std::true_type
{};

} // namespace gc_detail

//! Visitor for Immer's node tracing hooks and reflected application values.
class storage_mark_visitor
{
public:
    explicit storage_mark_visitor(const extent_snapshot& extents)
        : extents_{extents}
    {}

    template <typename T>
    void visit_node(immer::storage_ptr<T> handle)
    {
        if (handle)
            extents_.mark(handle.template rebind<void>());
    }

    template <typename T>
    void visit_auxiliary(immer::storage_ptr<T> handle)
    {
        if (handle)
            extents_.mark(handle.template rebind<void>());
    }

    template <typename T>
    void visit_node(T*)
    {
        throw std::logic_error{
            "storage GC received a raw Immer node pointer"};
    }

    template <typename T>
    void visit_auxiliary(T*)
    {
        throw std::logic_error{
            "storage GC received a raw Immer auxiliary pointer"};
    }

    template <typename T>
    void visit_value(const T& value)
    {
        trace_value(value);
    }

    template <typename T>
    void trace_root(immer::storage_ptr<T> root)
    {
        if (!root)
            return;
        if constexpr (std::is_void<std::remove_cv_t<T>>::value) {
            throw std::logic_error{
                "storage GC root must have a concrete reflected type"};
        } else if (extents_.mark(root.template rebind<void>())) {
            trace_value(*root);
        }
    }

private:
    template <typename T>
    void trace_value(const immer::storage_ptr<T>& handle)
    {
        if (!handle)
            return;
        if constexpr (std::is_void<std::remove_cv_t<T>>::value) {
            throw std::logic_error{
                "storage GC cannot trace an erased storage_ptr field"};
        } else {
            static_assert(is_persistent_safe<T>::value,
                          "storage GC target type is not persistent-safe");
            if (extents_.mark(handle.template rebind<void>()))
                trace_value(*handle);
        }
    }

    template <typename T>
    void trace_value(const T& value)
    {
        if constexpr (gc_detail::has_trace_children<T,
                                                    storage_mark_visitor>::value) {
            value.trace_children(*this);
        } else if constexpr (persistent_struct_info<T>::reflected) {
            static_assert(is_persistent_safe<T>::value,
                          "storage GC reflected root is not persistent-safe");
            trace_reflected_fields(value, *this);
        } else if constexpr (gc_detail::is_storage_ptr<T>::value) {
            trace_value(value);
        } else if constexpr (gc_detail::is_std_array<T>::value) {
            for (const auto& item : value)
                trace_value(item);
        } else if constexpr (gc_detail::is_std_pair<T>::value) {
            trace_value(value.first);
            trace_value(value.second);
        }
    }

    const extent_snapshot& extents_;
};

/*!
 * Mark and sweep arena slots reachable from an explicit set of typed roots.
 * Callers must supply every current root, pinned snapshot root, live
 * transaction root, and explicit snapshot root before collection begins.
 * Allocations made during marking are retained for this cycle.
 */
class tracing_collector
{
public:
    explicit tracing_collector(extent_snapshot extents)
        : extents_{std::move(extents)}
    {
        if (!extents_)
            throw std::invalid_argument{"collector requires pinned extents"};
    }

    template <typename T>
    void add_root(immer::storage_ptr<T> root)
    {
        roots_.emplace_back([root](storage_mark_visitor& visitor) {
            visitor.trace_root(root);
        });
    }

    //! Pin a persistent snapshot until this collector has finished tracing.
    template <typename T,
              std::enable_if_t<gc_detail::is_typed_storage_ptr<T>::value,
                               int> = 0>
    void add_root(shared_snapshot<T> snapshot)
    {
        auto pin = std::make_shared<shared_snapshot<T>>(std::move(snapshot));
        roots_.emplace_back([pin = std::move(pin)](
                                storage_mark_visitor& visitor) {
            visitor.trace_root(pin->get());
        });
    }

    //! Pin an erased persistent snapshot and trace it as its reflected root.
    template <typename RootT>
    void add_root_as(shared_snapshot<immer::storage_ptr<void>> snapshot)
    {
        static_assert(!std::is_void<RootT>::value,
                      "a persistent snapshot root must have a concrete type");
        auto pin = std::make_shared<
            shared_snapshot<immer::storage_ptr<void>>>(std::move(snapshot));
        roots_.emplace_back([pin = std::move(pin)](
                                storage_mark_visitor& visitor) {
            visitor.trace_root(pin->get().template rebind<RootT>());
        });
    }

    template <typename T>
    void add_root_as(immer::storage_ptr<void> root)
    {
        add_root(root.template rebind<T>());
    }

    std::size_t collect()
    {
        bool marking = false;
        try {
            extents_.begin_mark();
            marking = true;
            auto storage_scope = extents_.activate();
            auto visitor = storage_mark_visitor{extents_};
            for (auto& trace_root : roots_)
                trace_root(visitor);
            return extents_.sweep();
        } catch (...) {
            if (marking)
                extents_.cancel_mark();
            throw;
        }
    }

private:
    extent_snapshot extents_;
    std::vector<std::function<void(storage_mark_visitor&)>> roots_;
};

} // namespace lager
