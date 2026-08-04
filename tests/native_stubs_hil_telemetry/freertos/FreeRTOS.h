#pragma once

#include <cstdint>

using BaseType_t = int;
using TickType_t = std::uint32_t;

inline constexpr BaseType_t pdFALSE = 0;
inline constexpr BaseType_t pdTRUE = 1;
inline constexpr TickType_t portMAX_DELAY = UINT32_MAX;
