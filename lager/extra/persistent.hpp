// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <boost/hana/accessors.hpp>
#include <boost/hana/at.hpp>
#include <boost/hana/first.hpp>
#include <boost/hana/length.hpp>
#include <boost/hana/second.hpp>
#include <boost/hana/core/to.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <immer/storage_ptr.hpp>
#include <immer/variant.hpp>
#include <immer/array.hpp>
#include <immer/map.hpp>
#include <immer/memory_policy.hpp>
#include <immer/set.hpp>
#include <immer/table.hpp>
#include <immer/vector.hpp>
#include <immer/flex_vector.hpp>

namespace lager {

//! A single reflected field's native layout and stable type identity.
struct persistent_field_layout
{
    const char* name;
    std::uint64_t type_id;   // Stable nominal ID used by migration rules.
    std::uint64_t schema_id; // Recursive field-layout identity.
    std::size_t offset;
    std::size_t size;
    std::size_t alignment;
};

//! Generated layout description for one LAGER_STRUCT type.
struct persistent_layout_descriptor
{
    std::uint64_t type_id;
    std::uint64_t fingerprint;
    std::uint32_t schema_version;
    std::size_t size;
    std::size_t alignment;
    std::vector<persistent_field_layout> fields;
};

//! Marker populated by LAGER_STRUCT for directly mapped application structs.
template <typename T, typename = void>
struct persistent_struct_info
{
    static constexpr bool reflected = false;
    static constexpr std::uint64_t type_id = 0;
};

//! Opt-in metadata for nested structs declared with
//! LAGER_PERSISTENT_STRUCT_NESTED.
template <typename T>
struct persistent_struct_info<
    T,
    std::void_t<decltype(T::lager_persistent_type_id)>>
{
    static constexpr bool reflected = true;
    static constexpr std::uint64_t type_id = T::lager_persistent_type_id;
};

template <typename T>
persistent_layout_descriptor persistent_layout(const T& object,
                                                std::uint32_t schema_version = 1);

namespace persistent_detail {

constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime  = 1099511628211ull;

constexpr std::uint64_t hash_bytes(std::string_view text,
                                   std::uint64_t seed = fnv_offset) noexcept
{
    for (const auto ch : text) {
        seed ^= static_cast<unsigned char>(ch);
        seed *= fnv_prime;
    }
    return seed;
}

constexpr std::uint64_t mix(std::uint64_t seed, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i != 8; ++i) {
        seed ^= static_cast<unsigned char>(value & 0xffu);
        seed *= fnv_prime;
        value >>= 8;
    }
    return seed;
}

template <typename T>
struct type_id
    : std::integral_constant<std::uint64_t, persistent_struct_info<T>::type_id>
{};

//! Recursive field-layout identity, separate from the nominal migration ID.
template <typename T, typename = void>
struct schema_type_id : type_id<T>
{};

#define LAGER_PERSISTENT_PRIMITIVE(type__, id__)                              \
    template <>                                                               \
    struct type_id<type__> : std::integral_constant<std::uint64_t, id__>      \
    {}

LAGER_PERSISTENT_PRIMITIVE(std::int8_t, 0x1101);
LAGER_PERSISTENT_PRIMITIVE(std::uint8_t, 0x1102);
LAGER_PERSISTENT_PRIMITIVE(std::int16_t, 0x1103);
LAGER_PERSISTENT_PRIMITIVE(std::uint16_t, 0x1104);
LAGER_PERSISTENT_PRIMITIVE(std::int32_t, 0x1105);
LAGER_PERSISTENT_PRIMITIVE(std::uint32_t, 0x1106);
LAGER_PERSISTENT_PRIMITIVE(std::int64_t, 0x1107);
LAGER_PERSISTENT_PRIMITIVE(std::uint64_t, 0x1108);
LAGER_PERSISTENT_PRIMITIVE(char, 0x1109);
LAGER_PERSISTENT_PRIMITIVE(bool, 0x110a);
LAGER_PERSISTENT_PRIMITIVE(float, 0x110b);
LAGER_PERSISTENT_PRIMITIVE(double, 0x110c);
LAGER_PERSISTENT_PRIMITIVE(std::byte, 0x110d);

#undef LAGER_PERSISTENT_PRIMITIVE

template <typename T>
struct type_id<immer::storage_ptr<T>>
    : std::integral_constant<
          std::uint64_t,
          mix(0x2201, std::is_void<T>::value ? 0 : type_id<T>::value)>
{};

template <typename T>
struct type_id<std::hash<T>>
    : std::integral_constant<std::uint64_t,
                             mix(0x2210, type_id<T>::value)>
{};

template <typename T>
struct type_id<std::equal_to<T>>
    : std::integral_constant<std::uint64_t,
                             mix(0x2211, type_id<T>::value)>
{};

template <typename T, typename U>
struct type_id<std::pair<T, U>>
    : std::integral_constant<std::uint64_t,
                             mix(mix(0x2212, type_id<T>::value),
                                 type_id<U>::value)>
{};

template <>
struct type_id<immer::table_key_fn>
    : std::integral_constant<std::uint64_t, 0x2213>
{};

template <typename T, std::size_t N>
struct type_id<std::array<T, N>>
    : std::integral_constant<std::uint64_t,
                             mix(mix(0x2202, type_id<T>::value), N)>
{};

template <std::uint64_t Seed, typename... Ts>
struct variant_type_id;

template <std::uint64_t Seed>
struct variant_type_id<Seed> : std::integral_constant<std::uint64_t, Seed>
{};

template <std::uint64_t Seed, typename T, typename... Ts>
struct variant_type_id<Seed, T, Ts...>
    : variant_type_id<mix(Seed, type_id<T>::value), Ts...>
{};

template <typename... Ts>
struct type_id<immer::variant<Ts...>>
    : variant_type_id<mix(0x2301, sizeof...(Ts)), Ts...>
{};

template <typename T, typename MemoryPolicy>
struct type_id<immer::array<T, MemoryPolicy>>
    : std::integral_constant<std::uint64_t,
                             mix(0x2302, type_id<T>::value)>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct type_id<immer::vector<T, MemoryPolicy, B, BL>>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(mix(0x2306, type_id<T>::value), B), BL)>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct type_id<immer::flex_vector<T, MemoryPolicy, B, BL>>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(mix(0x2307, type_id<T>::value), B), BL)>
{};

template <typename K,
          typename V,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct type_id<immer::map<K, V, Hash, Equal, MemoryPolicy, B>>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(mix(mix(mix(0x2303, type_id<K>::value),
                           type_id<V>::value),
                      type_id<Hash>::value),
                 type_id<Equal>::value),
              B)>
{};

template <typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct type_id<immer::set<T, Hash, Equal, MemoryPolicy, B>>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(mix(mix(0x2304, type_id<T>::value),
                      type_id<Hash>::value),
                 type_id<Equal>::value),
              B)>
{};

template <typename T,
          typename KeyFn,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct type_id<immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(mix(mix(mix(0x2305, type_id<T>::value),
                           type_id<KeyFn>::value),
                      type_id<Hash>::value),
                 type_id<Equal>::value),
              B)>
{};

template <typename T, typename = void>
struct reflected_safe_type;

template <typename T, typename = void>
struct safe_type : std::integral_constant<bool, (type_id<T>::value != 0)>
{};

template <typename T,
          bool = persistent_struct_info<T>::reflected>
struct indirect_safe_type : safe_type<T>
{};

// A storage_ptr is a graph edge. Reflected targets are validated as their own
// LAGER_STRUCT declarations/layout roots instead of recursively embedding the
// target's fields into this value's safety check.
template <typename T>
struct indirect_safe_type<T, true> : std::true_type
{};

template <typename T>
struct safe_type<immer::storage_ptr<T>, void>
    : std::integral_constant<bool,
                             !std::is_void<std::remove_cv_t<T>>::value &&
                                 indirect_safe_type<
                                     std::remove_cv_t<T>>::value>
{};

template <typename T, std::size_t N>
struct safe_type<std::array<T, N>, void> : safe_type<T>
{};

template <typename T, typename U>
struct safe_type<std::pair<T, U>, void>
    : std::integral_constant<bool, safe_type<T>::value && safe_type<U>::value>
{};

template <typename... Ts>
struct all_safe_types;

template <>
struct all_safe_types<> : std::true_type
{};

template <typename T, typename... Ts>
struct all_safe_types<T, Ts...>
    : std::integral_constant<bool,
                             safe_type<T>::value && all_safe_types<Ts...>::value>
{};

template <typename MemoryPolicy>
struct storage_policy_safe
    : std::integral_constant<
          bool,
          std::is_same<typename MemoryPolicy::pointer,
                       immer::storage_pointer_policy>::value &&
              std::is_same<typename MemoryPolicy::refcount,
                           immer::no_refcount_policy>::value &&
              std::is_same<typename MemoryPolicy::lock,
                           immer::no_lock_policy>::value &&
              std::is_same<typename MemoryPolicy::transience,
                           immer::no_transience_policy>::value &&
              (MemoryPolicy::prefer_fewer_bigger_objects ==
               immer::storage_memory_policy::prefer_fewer_bigger_objects) &&
              !MemoryPolicy::use_transient_rvalues &&
              std::is_same<typename MemoryPolicy::heap::type,
                           immer::storage_ptr_heap>::value>
{};

template <typename... Ts>
struct safe_type<immer::variant<Ts...>, void> : all_safe_types<Ts...>
{};

template <typename T, typename MemoryPolicy>
struct safe_type<immer::array<T, MemoryPolicy>, void>
    : std::integral_constant<bool,
                             storage_policy_safe<MemoryPolicy>::value &&
                                 safe_type<T>::value>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct safe_type<immer::vector<T, MemoryPolicy, B, BL>, void>
    : std::integral_constant<bool,
                             storage_policy_safe<MemoryPolicy>::value &&
                                 safe_type<T>::value>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct safe_type<immer::flex_vector<T, MemoryPolicy, B, BL>, void>
    : std::integral_constant<bool,
                             storage_policy_safe<MemoryPolicy>::value &&
                                 safe_type<T>::value>
{};

template <typename K,
          typename V,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct safe_type<immer::map<K, V, Hash, Equal, MemoryPolicy, B>, void>
    : std::integral_constant<
          bool,
          storage_policy_safe<MemoryPolicy>::value && safe_type<K>::value &&
              safe_type<V>::value && safe_type<Hash>::value &&
              safe_type<Equal>::value>
{};

template <typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct safe_type<immer::set<T, Hash, Equal, MemoryPolicy, B>, void>
    : std::integral_constant<
          bool,
          storage_policy_safe<MemoryPolicy>::value && safe_type<T>::value &&
              safe_type<Hash>::value && safe_type<Equal>::value>
{};

template <typename T,
          typename KeyFn,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct safe_type<immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>, void>
    : std::integral_constant<
          bool,
          storage_policy_safe<MemoryPolicy>::value && safe_type<T>::value &&
              safe_type<KeyFn>::value && safe_type<Hash>::value &&
              safe_type<Equal>::value>
{};

template <typename T>
struct safe_type<T, std::enable_if_t<persistent_struct_info<T>::reflected>>
    : reflected_safe_type<T>
{};

template <typename T>
struct member_pointer_value;

template <typename MemberT, typename OwnerT>
struct member_pointer_value<MemberT OwnerT::*>
{
    using type = MemberT;
};

template <typename T, std::size_t I>
using reflected_accessor_t = std::remove_cv_t<std::remove_reference_t<decltype(
    boost::hana::second(boost::hana::at_c<I>(boost::hana::accessors<T>())))>>;

template <typename T, std::size_t I>
using reflected_member_ref_t = typename member_pointer_value<
    typename reflected_accessor_t<T, I>::member_pointer_type>::type;

template <typename T, std::size_t I>
using reflected_member_t =
    std::remove_cv_t<std::remove_reference_t<reflected_member_ref_t<T, I>>>;

template <typename Seed, typename... Ts>
struct schema_variant_id;

template <std::uint64_t Seed>
struct schema_variant_id<std::integral_constant<std::uint64_t, Seed>>
    : std::integral_constant<std::uint64_t, Seed>
{};

template <std::uint64_t Seed, typename T, typename... Ts>
struct schema_variant_id<std::integral_constant<std::uint64_t, Seed>, T, Ts...>
    : schema_variant_id<
          std::integral_constant<std::uint64_t,
                                 mix(Seed, schema_type_id<T>::value)>,
          Ts...>
{};

template <typename... Ts>
struct schema_type_id<immer::variant<Ts...>, void>
    : schema_variant_id<
          std::integral_constant<std::uint64_t,
                                 mix(0x2401, sizeof...(Ts))>,
          Ts...>
{};

template <typename T>
struct schema_type_id<immer::storage_ptr<T>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(0x2402,
              std::is_void<std::remove_cv_t<T>>::value
                  ? 0
                  : type_id<std::remove_cv_t<T>>::value)>
{};

template <typename T>
struct schema_type_id<std::hash<T>, void>
    : std::integral_constant<std::uint64_t,
                             mix(0x2410, schema_type_id<T>::value)>
{};

template <typename T>
struct schema_type_id<std::equal_to<T>, void>
    : std::integral_constant<std::uint64_t,
                             mix(0x2411, schema_type_id<T>::value)>
{};

template <typename T, typename U>
struct schema_type_id<std::pair<T, U>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(0x2412, schema_type_id<T>::value),
              schema_type_id<U>::value)>
{};

template <typename T, std::size_t N>
struct schema_type_id<std::array<T, N>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(0x2403, schema_type_id<T>::value), N)>
{};

template <typename T, typename MemoryPolicy>
struct schema_type_id<immer::array<T, MemoryPolicy>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(type_id<immer::array<T, MemoryPolicy>>::value,
              schema_type_id<T>::value)>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct schema_type_id<immer::vector<T, MemoryPolicy, B, BL>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(type_id<immer::vector<T, MemoryPolicy, B, BL>>::value,
              schema_type_id<T>::value)>
{};

template <typename T,
          typename MemoryPolicy,
          immer::detail::rbts::bits_t B,
          immer::detail::rbts::bits_t BL>
struct schema_type_id<immer::flex_vector<T, MemoryPolicy, B, BL>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(type_id<immer::flex_vector<T, MemoryPolicy, B, BL>>::value,
              schema_type_id<T>::value)>
{};

template <typename K,
          typename V,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct schema_type_id<immer::map<K, V, Hash, Equal, MemoryPolicy, B>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(mix(type_id<immer::map<K, V, Hash, Equal, MemoryPolicy, B>>::value,
                  schema_type_id<K>::value),
              mix(schema_type_id<V>::value,
                  mix(schema_type_id<Hash>::value,
                      schema_type_id<Equal>::value)))>
{};

template <typename T,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct schema_type_id<immer::set<T, Hash, Equal, MemoryPolicy, B>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(type_id<immer::set<T, Hash, Equal, MemoryPolicy, B>>::value,
              mix(schema_type_id<T>::value,
                  mix(schema_type_id<Hash>::value,
                      schema_type_id<Equal>::value)))>
{};

template <typename T,
          typename KeyFn,
          typename Hash,
          typename Equal,
          typename MemoryPolicy,
          immer::detail::hamts::bits_t B>
struct schema_type_id<
    immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>, void>
    : std::integral_constant<
          std::uint64_t,
          mix(type_id<immer::table<T, KeyFn, Hash, Equal, MemoryPolicy, B>>::value,
              mix(schema_type_id<T>::value,
                  mix(schema_type_id<KeyFn>::value,
                      mix(schema_type_id<Hash>::value,
                          schema_type_id<Equal>::value))))>
{};

template <typename T, std::size_t... I>
constexpr std::uint64_t reflected_schema_type_id(
    std::index_sequence<I...>)
{
    const auto accessors = boost::hana::accessors<T>();
    auto result = mix(mix(persistent_struct_info<T>::type_id, sizeof(T)),
                      alignof(T));
    result = mix(result, sizeof...(I));
    ((result = hash_bytes(
          boost::hana::to<const char*>(boost::hana::first(
              boost::hana::at_c<I>(accessors))),
          result),
      result = mix(result,
                   schema_type_id<reflected_member_t<T, I>>::value)),
     ...);
    return result;
}

template <typename T>
struct schema_type_id<
    T,
    std::enable_if_t<persistent_struct_info<T>::reflected>>
    : std::integral_constant<
          std::uint64_t,
          reflected_schema_type_id<T>(std::make_index_sequence<
              decltype(boost::hana::length(
                  boost::hana::accessors<T>()))::value>{})>
{};

template <typename T, typename Visitor, std::size_t... I>
void trace_reflected_fields_impl(const T& object,
                                 Visitor& visitor,
                                 std::index_sequence<I...>)
{
    const auto accessors = boost::hana::accessors<T>();
    (visitor.visit_value(boost::hana::second(boost::hana::at_c<I>(accessors))(
         object)),
     ...);
}

template <typename T, std::size_t... I>
constexpr bool reflected_fields_safe(std::index_sequence<I...>)
{
    return ((!std::is_reference<reflected_member_ref_t<T, I>>::value &&
             !std::is_volatile<reflected_member_ref_t<T, I>>::value &&
             safe_type<reflected_member_t<T, I>>::value) &&
            ...);
}

template <typename T>
std::uint64_t field_schema_id(const T& value)
{
    if constexpr (persistent_struct_info<T>::reflected)
        return persistent_layout(value, 1).fingerprint;
    else
        return schema_type_id<T>::value;
}

template <typename T, typename>
struct reflected_safe_type : std::false_type
{};

template <typename T>
struct reflected_safe_type<
    T,
    std::enable_if_t<persistent_struct_info<T>::reflected>>
    : std::integral_constant<
          bool,
          std::is_standard_layout<T>::value && !std::is_union<T>::value &&
              reflected_fields_safe<T>(std::make_index_sequence<
                  decltype(boost::hana::length(
                      boost::hana::accessors<T>()))::value>{})>
{};

template <typename T, std::size_t I>
persistent_field_layout make_field_layout(const T& object)
{
    const auto accessors = boost::hana::accessors<T>();
    const auto accessor  = boost::hana::at_c<I>(accessors);
    const auto& field    = boost::hana::second(accessor)(object);
    using field_t        = reflected_member_t<T, I>;
    const auto* base     = reinterpret_cast<const std::byte*>(&object);
    const auto* address = reinterpret_cast<const std::byte*>(
        std::addressof(field));
    return {boost::hana::to<const char*>(boost::hana::first(accessor)),
            type_id<field_t>::value,
            field_schema_id(field),
            static_cast<std::size_t>(address - base),
            sizeof(field_t),
            alignof(field_t)};
}

template <typename T, std::size_t... I>
auto make_field_layouts(const T& object, std::index_sequence<I...>)
{
    return std::array<persistent_field_layout, sizeof...(I)>{
        make_field_layout<T, I>(object)...};
}

} // namespace persistent_detail

//! Visit every field of a reflected object through a tracing visitor.
template <typename T, typename Visitor>
void trace_reflected_fields(const T& object, Visitor& visitor)
{
    static_assert(persistent_struct_info<T>::reflected,
                  "trace_reflected_fields requires a LAGER_STRUCT type");
    persistent_detail::trace_reflected_fields_impl(
        object,
        visitor,
        std::make_index_sequence<
            decltype(boost::hana::length(boost::hana::accessors<T>()))::value>{});
}

template <typename T>
struct is_persistent_safe
    : persistent_detail::safe_type<std::remove_cv_t<T>>
{};

template <typename T>
struct persistent_type_id : persistent_detail::type_id<std::remove_cv_t<T>>
{};

template <typename T>
constexpr void validate_persistent_type()
{
    static_assert(is_persistent_safe<T>::value,
                  "type is not safe for shared or persistent Lager storage");
    static_assert(!persistent_struct_info<T>::reflected ||
                      std::is_standard_layout_v<T>,
                  "persistent reflected structs must have standard layout");
}

template <typename T>
persistent_layout_descriptor persistent_layout(const T& object,
                                                std::uint32_t schema_version)
{
    static_assert(persistent_struct_info<T>::reflected,
                  "persistent_layout requires a LAGER_STRUCT type");
    validate_persistent_type<T>();
    if (schema_version == 0)
        throw std::invalid_argument{"persistent schema version must be nonzero"};
    auto fields = persistent_detail::make_field_layouts<T>(
        object,
        std::make_index_sequence<
            decltype(boost::hana::length(boost::hana::accessors<T>()))::value>{});
    auto fingerprint = persistent_detail::mix(
        persistent_struct_info<T>::type_id, sizeof(T));
    fingerprint = persistent_detail::mix(fingerprint, alignof(T));
    fingerprint = persistent_detail::mix(fingerprint, schema_version);
    fingerprint = persistent_detail::mix(fingerprint, fields.size());
    for (const auto& field : fields) {
        fingerprint = persistent_detail::hash_bytes(field.name, fingerprint);
        fingerprint = persistent_detail::mix(fingerprint, field.type_id);
        fingerprint = persistent_detail::mix(fingerprint, field.schema_id);
        fingerprint = persistent_detail::mix(fingerprint, field.offset);
        fingerprint = persistent_detail::mix(fingerprint, field.size);
        fingerprint = persistent_detail::mix(fingerprint, field.alignment);
    }
    return {persistent_struct_info<T>::type_id,
            fingerprint,
            schema_version,
            sizeof(T),
            alignof(T),
            {fields.begin(), fields.end()}};
}

} // namespace lager
