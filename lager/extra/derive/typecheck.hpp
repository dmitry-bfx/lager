// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <boost/preprocessor/control/iif.hpp>
#include <boost/preprocessor/punctuation/is_begin_parens.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/tuple/elem.hpp>

#include <type_traits>
#include <utility>

#include <lager/extra/derive/member.hpp>

#define LAGER_DERIVE_IMPL_TYPE_CHECK_RAW__(r__, type__, elem__)               \
    static_assert(true, "untyped LAGER_STRUCT member")

#define LAGER_DERIVE_IMPL_TYPE_CHECK_TYPED__(r__, type__, elem__)             \
    static_assert(                                                            \
        std::is_same_v<                                                       \
            std::remove_cvref_t<decltype(std::declval<type__ const&>().       \
                                             LAGER_DERIVE_MEMBER_NAME__(      \
                                                 elem__))>,                   \
            BOOST_PP_TUPLE_ELEM(2, 0, elem__)>,                               \
        "LAGER_STRUCT field type does not match its declared layout type")

#define LAGER_DERIVE_IMPL_TYPE_CHECK_ITER__(r__, type__, elem__)               \
    BOOST_PP_IIF(BOOST_PP_IS_BEGIN_PARENS(elem__),                            \
                 LAGER_DERIVE_IMPL_TYPE_CHECK_TYPED__,                        \
                 LAGER_DERIVE_IMPL_TYPE_CHECK_RAW__)(r__, type__, elem__);

#define LAGER_DERIVE_IMPL_TYPE_CHECK(r__, ns__, name__, members__)            \
    namespace ns__ {                                                          \
    BOOST_PP_SEQ_FOR_EACH_R(                                                   \
        r__, LAGER_DERIVE_IMPL_TYPE_CHECK_ITER__, name__, members__)           \
    }                                                                          \
    //

#define LAGER_DERIVE_TEMPLATE_IMPL_TYPE_CHECK(r__, ns__, tpl__, name__,       \
                                              members__)                      \
    static_assert(false,                                                      \
                  "typed LAGER_STRUCT_TEMPLATE metadata is not supported yet")

#define LAGER_DERIVE_NESTED_IMPL_TYPE_CHECK(r__, name__, members__)           \
    static_assert(false,                                                      \
                  "typed LAGER_STRUCT_NESTED metadata is not supported yet")
