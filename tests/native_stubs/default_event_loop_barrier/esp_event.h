#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using esp_event_base_t = const char*;
using esp_event_handler_t = void (*)(void*, esp_event_base_t, int32_t, void*);
using esp_event_handler_instance_t = void*;
using TickType_t = uint32_t;

#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t name = #name

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t event_handler, void* event_handler_arg,
    esp_event_handler_instance_t* instance);
esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id,
                         const void* event_data, size_t event_data_size,
                         TickType_t ticks_to_wait);
esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_instance_t instance);
