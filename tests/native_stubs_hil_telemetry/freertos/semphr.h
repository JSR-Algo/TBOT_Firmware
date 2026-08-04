#pragma once

#include "FreeRTOS.h"

struct StaticSemaphore_t {
    bool held = false;
};

using SemaphoreHandle_t = StaticSemaphore_t*;

inline unsigned& HostEvidenceStaticMutexCreateCalls() {
    static unsigned calls = 0;
    return calls;
}

inline unsigned& HostEvidenceStaticMutexTakeCalls() {
    static unsigned calls = 0;
    return calls;
}

inline unsigned& HostEvidenceStaticMutexGiveCalls() {
    static unsigned calls = 0;
    return calls;
}

inline TickType_t& HostEvidenceStaticMutexLastWait() {
    static TickType_t wait = 0;
    return wait;
}

inline SemaphoreHandle_t& HostEvidenceStaticMutexHandle() {
    static SemaphoreHandle_t handle = nullptr;
    return handle;
}

inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
    ++HostEvidenceStaticMutexCreateCalls();
    storage->held = false;
    HostEvidenceStaticMutexHandle() = storage;
    return storage;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t wait) {
    ++HostEvidenceStaticMutexTakeCalls();
    HostEvidenceStaticMutexLastWait() = wait;
    if (handle == nullptr || handle->held) return pdFALSE;
    handle->held = true;
    return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
    ++HostEvidenceStaticMutexGiveCalls();
    if (handle == nullptr || !handle->held) return pdFALSE;
    handle->held = false;
    return pdTRUE;
}
