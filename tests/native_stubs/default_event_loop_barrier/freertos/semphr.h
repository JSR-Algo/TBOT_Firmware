#pragma once

#include "freertos/FreeRTOS.h"

struct NativeSemaphore;
using SemaphoreHandle_t = NativeSemaphore*;

SemaphoreHandle_t xSemaphoreCreateBinary();
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t ticks_to_wait);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
