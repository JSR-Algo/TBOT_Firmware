#pragma once

#include <cstdint>

using esp_timer_handle_t = struct HostEspTimer*;
using esp_err_t = int;
#ifndef ESP_OK
#define ESP_OK 0
#endif
constexpr int ESP_TIMER_TASK = 0;

struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    int dispatch_method = 0;
    const char* name;
    bool skip_unhandled_events = false;
};

struct HostEspTimer {
    void (*callback)(void*) = nullptr;
    void* arg = nullptr;
    bool armed = false;
};

inline bool& HostEspTimerCreateOk() { static bool v = true; return v; }
inline bool& HostEspTimerStartOk() { static bool v = true; return v; }
inline HostEspTimer*& HostEspLastTimer() { static HostEspTimer* v = nullptr; return v; }
inline int64_t& HostEspNowUs() { static int64_t v = 0; return v; }
struct HostEspQueuedCallback { void (*callback)(void*); void* arg; };
inline HostEspQueuedCallback HostEspQueueTimerCallback() {
    auto* timer = HostEspLastTimer();
    return {timer->callback, timer->arg};
}
inline void HostEspInvokeQueuedCallback(const HostEspQueuedCallback& queued) {
    queued.callback(queued.arg);
}
inline void HostEspAdvanceTimeUs(int64_t delta) { HostEspNowUs() += delta; }
inline esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    if (!HostEspTimerCreateOk()) return -1;
    auto* timer = new HostEspTimer{args->callback, args->arg, false};
    HostEspLastTimer() = timer;
    *out = timer;
    return ESP_OK;
}
inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer) timer->armed = false;
    return ESP_OK;
}
inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (HostEspLastTimer() == timer) HostEspLastTimer() = nullptr;
    delete timer;
    return ESP_OK;
}
inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t) {
    if (!HostEspTimerStartOk()) return -1;
    timer->armed = true;
    return ESP_OK;
}
inline void HostEspFireTimer() {
    auto* timer = HostEspLastTimer();
    if (timer && timer->armed) {
        timer->armed = false;
        timer->callback(timer->arg);
    }
}

inline int64_t esp_timer_get_time() {
    return HostEspNowUs();
}
