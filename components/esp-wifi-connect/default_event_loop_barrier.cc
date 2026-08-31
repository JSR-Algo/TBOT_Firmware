#include "default_event_loop_barrier.h"

#include <algorithm>
#include <chrono>

#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

ESP_EVENT_DEFINE_BASE(DEFAULT_EVENT_LOOP_BARRIER_EVENT);

constexpr int32_t kBarrierEventId = 1;
constexpr std::chrono::milliseconds kMaximumBarrierWait{1000};

void HandleBarrierEvent(void* handler_arg, esp_event_base_t, int32_t, void*) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(handler_arg));
}

}  // namespace

bool DrainDefaultEventLoop(std::chrono::milliseconds timeout) {
    SemaphoreHandle_t semaphore = xSemaphoreCreateBinary();
    if (semaphore == nullptr) {
        return false;
    }

    esp_event_handler_instance_t handler = nullptr;
    const esp_err_t register_result = esp_event_handler_instance_register(
        DEFAULT_EVENT_LOOP_BARRIER_EVENT, kBarrierEventId,
        &HandleBarrierEvent, semaphore, &handler);
    if (register_result != ESP_OK) {
        vSemaphoreDelete(semaphore);
        return false;
    }

    const esp_err_t post_result = esp_event_post(
        DEFAULT_EVENT_LOOP_BARRIER_EVENT, kBarrierEventId, nullptr, 0, 0);
    const auto bounded_timeout = std::max(
        std::chrono::milliseconds::zero(),
        std::min(timeout, kMaximumBarrierWait));
    bool barrier_drained = false;
    if (post_result == ESP_OK) {
        barrier_drained = xSemaphoreTake(
            semaphore, pdMS_TO_TICKS(bounded_timeout.count())) == pdTRUE;
    }

    const esp_err_t unregister_result =
        esp_event_handler_instance_unregister(
            DEFAULT_EVENT_LOOP_BARRIER_EVENT, kBarrierEventId, handler);
    vSemaphoreDelete(semaphore);
    return post_result == ESP_OK && barrier_drained &&
           unregister_result == ESP_OK;
}

WifiScanLeaseCoordinator::DrainProof DefaultEventLoopScanDrainExecutor::Execute(
        WifiScanLeaseCoordinator& coordinator,
        const WifiScanLeaseCoordinator::Lease& lease,
        const WifiScanLeaseCoordinator::DrainDecision& drain) {
    if (!coordinator.IsCurrentDrain(lease, drain)) {
        return WifiScanLeaseCoordinator::DrainProof{};
    }

    const esp_err_t stop_result = esp_wifi_scan_stop();
    const bool barrier_drained =
        DrainDefaultEventLoop(kMaximumBarrierWait);
    const bool scan_stopped =
        stop_result == ESP_OK || stop_result == ESP_ERR_WIFI_STATE;
    return WifiScanLeaseCoordinator::DrainProof{
        drain.drain_id(), scan_stopped && barrier_drained};
}
