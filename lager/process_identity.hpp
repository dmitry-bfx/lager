// lager - library for functional interactive c++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lager::detail {

struct process_identity
{
    std::uint64_t start_time = 0;
    bool running = false;
};

//! Linux process start time distinguishes a live PID from a reused PID.
inline std::optional<process_identity> inspect_process_identity(
    std::uint64_t process_id) noexcept
{
#if defined(__linux__)
    char path[64]{};
    const auto path_size = std::snprintf(path,
                                         sizeof(path),
                                         "/proc/%llu/stat",
                                         static_cast<unsigned long long>(
                                             process_id));
    if (path_size <= 0 || static_cast<std::size_t>(path_size) >= sizeof(path))
        return std::nullopt;
    const auto fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    char contents[4096]{};
    const auto bytes = ::read(fd, contents, sizeof(contents) - 1);
    ::close(fd);
    if (bytes <= 0)
        return std::nullopt;
    contents[bytes] = '\0';

    // comm (field 2) is parenthesized and may itself contain spaces or ')'.
    const auto* cursor = std::strrchr(contents, ')');
    if (cursor == nullptr || cursor[1] != ' ')
        return std::nullopt;
    cursor += 2; // field 3 (state)
    char state = '\0';
    std::uint64_t start_time = 0;
    for (unsigned field = 3; field <= 22; ++field) {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            return std::nullopt;
        const auto* first = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
        if (field == 3)
            state = *first;
        if (field == 22) {
            char* end = nullptr;
            const auto parsed = std::strtoull(first, &end, 10);
            if (end != cursor || parsed == 0)
                return std::nullopt;
            start_time = static_cast<std::uint64_t>(parsed);
        }
    }
    if (start_time == 0)
        return std::nullopt;
    return process_identity{start_time, state != 'Z' && state != 'X'};
#else
    (void) process_id;
    return std::nullopt;
#endif
}

} // namespace lager::detail
