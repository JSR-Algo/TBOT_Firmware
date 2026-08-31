#include "default_event_loop_barrier.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <new>

#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

ESP_EVENT_DEFINE_BASE(DEFAULT_EVENT_LOOP_BARRIER_EVENT);

constexpr int32_t kBarrierEventId = 1;
constexpr std::chrono::milliseconds kMaximumBarrierWait{1000};

class DefaultEventLoopBarrier {
public:
    bool Drain(std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Initialize()) {
            return false;
        }

        if (last_barrier_id_ == std::numeric_limits<uint64_t>::max()) {
            return false;
        }

        const uint64_t posted_barrier_id = ++last_barrier_id_;
        {
            std::lock_guard<std::mutex> handler_lock(handler_mutex_);
            DrainSemaphore();
            active_barrier_id_.store(posted_barrier_id,
                                     std::memory_order_release);
        }
        const esp_err_t post_result = esp_event_post(
            DEFAULT_EVENT_LOOP_BARRIER_EVENT, kBarrierEventId,
            &posted_barrier_id, sizeof(posted_barrier_id), 0);
        if (post_result != ESP_OK) {
            std::lock_guard<std::mutex> handler_lock(handler_mutex_);
            active_barrier_id_.store(0, std::memory_order_release);
            DrainSemaphore();
            return false;
        }

        const auto bounded_timeout = std::max(
            std::chrono::milliseconds::zero(),
            std::min(timeout, kMaximumBarrierWait));
        const bool barrier_drained =
            xSemaphoreTake(
                semaphore_, pdMS_TO_TICKS(bounded_timeout.count())) == pdTRUE;
        {
            std::lock_guard<std::mutex> handler_lock(handler_mutex_);
            active_barrier_id_.store(0, std::memory_order_release);
            DrainSemaphore();
        }
        return barrier_drained;
    }

private:
    bool Initialize() {
        if (semaphore_ != nullptr) {
            return true;
        }

        semaphore_ = xSemaphoreCreateBinary();
        if (semaphore_ == nullptr) {
            return false;
        }

        const esp_err_t register_result = esp_event_handler_instance_register(
            DEFAULT_EVENT_LOOP_BARRIER_EVENT, kBarrierEventId,
            &DefaultEventLoopBarrier::HandleEvent, this, &handler_);
        if (register_result != ESP_OK) {
            vSemaphoreDelete(semaphore_);
            semaphore_ = nullptr;
            return false;
        }
        return true;
    }

    static void HandleEvent(void* handler_arg, esp_event_base_t, int32_t,
                            void* event_data) {
        auto* self = static_cast<DefaultEventLoopBarrier*>(handler_arg);
        const auto posted_barrier_id = *static_cast<uint64_t*>(event_data);
        std::lock_guard<std::mutex> handler_lock(self->handler_mutex_);
        if (posted_barrier_id == self->active_barrier_id_.load(
                                     std::memory_order_acquire)) {
            xSemaphoreGive(self->semaphore_);
        }
    }

    void DrainSemaphore() {
        while (xSemaphoreTake(semaphore_, 0) == pdTRUE) {
        }
    }

    std::mutex mutex_;
    std::mutex handler_mutex_;
    SemaphoreHandle_t semaphore_ = nullptr;
    esp_event_handler_instance_t handler_ = nullptr;
    std::atomic<uint64_t> active_barrier_id_{0};
    uint64_t last_barrier_id_ = 0;
};

}  // namespace

bool DrainDefaultEventLoop(std::chrono::milliseconds timeout) {
    static DefaultEventLoopBarrier* barrier =
        new (std::nothrow) DefaultEventLoopBarrier();
    if (barrier == nullptr) {
        return false;
    }
    return barrier->Drain(timeout);
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
    return WifiScanLeaseCoordinator::DrainProof{
        drain.drain_id(), stop_result == ESP_OK && barrier_drained};
}
