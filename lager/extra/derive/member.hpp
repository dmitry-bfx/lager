// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <boost/preprocessor/control/iif.hpp>
#include <boost/preprocessor/punctuation/is_begin_parens.hpp>
#include <boost/preprocessor/tuple/elem.hpp>

#define LAGER_DERIVE_MEMBER_RAW__(elem__) elem__
#define LAGER_DERIVE_MEMBER_TYPED__(elem__) BOOST_PP_TUPLE_ELEM(2, 1, elem__)
#define LAGER_DERIVE_MEMBER_NAME__(elem__)                                    \
    BOOST_PP_IIF(BOOST_PP_IS_BEGIN_PARENS(elem__),                            \
                 LAGER_DERIVE_MEMBER_TYPED__,                                 \
                 LAGER_DERIVE_MEMBER_RAW__)(elem__)
