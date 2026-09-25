#pragma once

#include <cstdio>

// Evaluate production log arguments so ASan checks their deferred lifetimes.
#define ESP_LOGI(tag, format, ...) std::printf(format "\n", __VA_ARGS__)
