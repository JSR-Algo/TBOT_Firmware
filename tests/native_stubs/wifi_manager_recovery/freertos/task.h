#pragma once
#include "freertos/FreeRTOS.h"
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
BaseType_t xTaskCreate(TaskFunction_t, const char*, uint32_t, void*, int,
                       TaskHandle_t*);
BaseType_t xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskDelay(TickType_t);
