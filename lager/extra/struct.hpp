//
// lager - library for functional interactive c++ programs
// Copyright (C) 2017 Juan Pedro Bolivar Puente
//
// This file is part of lager.
//
// lager is free software: you can redistribute it and/or modify
// it under the terms of the MIT License, as detailed in the LICENSE
// file located at the root of this source code distribution,
// or here: <https://github.com/arximboldi/lager/blob/master/LICENSE>
//

#pragma once

#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/punctuation/remove_parens.hpp>

#include <lager/extra/derive.hpp>
#include <lager/extra/derive/eq.hpp>
#include <lager/extra/derive/hana.hpp>
#include <lager/extra/persistent.hpp>
#include <lager/extra/derive/size_check.hpp>
#include <lager/extra/derive/typecheck.hpp>

/*!
 * This macro declares the struct as a Boost.Hana sequence, so it can be
 * introspected via metaprogramming.  This macro has similar syntax to
 * BOOST_HANA_ADAPT_STRUCT.
 */
#define LAGER_STRUCT(ns__, name__, ...)                                        \
    LAGER_DERIVE((EQ, HANA, SIZE_CHECK, TYPE_CHECK),                           \
                 ns__,                                                        \
                 name__ __VA_OPT__(, ) __VA_ARGS__);                          \
    namespace lager {                                                          \
    template <>                                                                \
    struct persistent_struct_info<ns__::name__>                               \
    {                                                                          \
        static constexpr bool reflected = true;                                \
        static constexpr std::uint64_t type_id =                              \
            persistent_detail::hash_bytes(                                    \
                BOOST_PP_STRINGIZE(ns__) "::" BOOST_PP_STRINGIZE(name__));   \
    };                                                                         \
    static_assert(is_persistent_safe<ns__::name__>::value,                    \
                  "LAGER_STRUCT contains a field unsafe for shared storage"); \
    static_assert(std::is_standard_layout_v<ns__::name__>,                     \
                  "persistent reflected structs must have standard layout"); \
    }

/*!
 * Like LAGER_STRUCT but for templates.
 */
#define LAGER_STRUCT_TEMPLATE(...)                                             \
    LAGER_STRUCT_TEMPLATE_IMPL__(__VA_ARGS__)

#define LAGER_STRUCT_TEMPLATE_IMPL__(ns__, tpl__, name__, ...)                 \
    LAGER_DERIVE_TEMPLATE((EQ, HANA),                                         \
                          ns__,                                               \
                          tpl__,                                              \
                          name__ __VA_OPT__(, ) __VA_ARGS__);                 \
    namespace lager {                                                         \
    template <BOOST_PP_REMOVE_PARENS(tpl__)>                                  \
    struct persistent_struct_info<                                           \
        ns__::BOOST_PP_REMOVE_PARENS(name__)>                                 \
    {                                                                         \
        static constexpr bool reflected = true;                               \
        static constexpr std::uint64_t type_id =                             \
            persistent_detail::hash_bytes(                                   \
                BOOST_PP_STRINGIZE(ns__) "::"                                \
                BOOST_PP_STRINGIZE(BOOST_PP_REMOVE_PARENS(name__)));          \
        static_assert(                                                        \
            is_persistent_safe<                                               \
                ns__::BOOST_PP_REMOVE_PARENS(name__)>::value,                 \
            "LAGER_STRUCT_TEMPLATE contains a field unsafe for shared storage"); \
        static_assert(std::is_standard_layout_v<                               \
                          ns__::BOOST_PP_REMOVE_PARENS(name__)>,              \
                      "persistent reflected structs must have standard layout"); \
    };                                                                        \
    }

/*!
 * Like LAGER_STRUCT but for it is used nested in the struct itself.  It
 * seamlessly supports combinations of templates and nested types that are not
 * supported by the above macros.
 */
#define LAGER_STRUCT_NESTED(...) LAGER_DERIVE_NESTED((EQ, HANA), __VA_ARGS__)

/*!
 * Declare an explicitly persistent nested struct. `stable_name__` is hashed
 * into its schema identity and must remain stable across source refactors.
 * Persistent safety and standard-layout checks run when this type is used in
 * a persistent layout.
 */
#define LAGER_PERSISTENT_STRUCT_NESTED(...)                                   \
    LAGER_PERSISTENT_STRUCT_NESTED_IMPL__(__VA_ARGS__)

#define LAGER_PERSISTENT_STRUCT_NESTED_IMPL__(type__, stable_name__, ...)     \
    LAGER_DERIVE_NESTED((EQ, HANA), type__, __VA_ARGS__);                     \
    static constexpr std::uint64_t lager_persistent_type_id =                \
        ::lager::persistent_detail::hash_bytes(                               \
            BOOST_PP_STRINGIZE(stable_name__))
