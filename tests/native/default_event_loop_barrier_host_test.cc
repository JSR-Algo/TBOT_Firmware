#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "../../components/esp-wifi-connect/default_event_loop_barrier.cc"

namespace {

enum class Step : uint8_t {
    kCreate,
    kRegister,
    kPost,
    kGive,
    kTake,
    kUnregister,
    kDelete,
    kScanStop,
};

struct Environment {
    bool allocation_succeeds = true;
    esp_err_t register_result = ESP_OK;
    esp_err_t post_result = ESP_OK;
    bool wait_succeeds = true;
    esp_err_t unregister_result = ESP_OK;
    esp_err_t scan_stop_result = ESP_OK;
    bool deliver_post = true;
    TickType_t wait_ticks = 0;
    std::vector<Step> steps;
};

Environment environment;
esp_event_handler_t registered_handler = nullptr;
void* registered_argument = nullptr;
std::vector<uint8_t> pending_event_data;
esp_event_base_t pending_event_base = nullptr;
int32_t pending_event_id = 0;

void DeliverPendingEvent() {
    assert(registered_handler != nullptr);
    registered_handler(registered_argument, pending_event_base,
                       pending_event_id, pending_event_data.data());
    pending_event_data.clear();
}

void Reset() {
    environment = Environment{};
    pending_event_data.clear();
    pending_event_base = nullptr;
    pending_event_id = 0;
}

void AllocationFailureReturnsFalseWithoutCleanup() {
    Reset();
    environment.allocation_succeeds = false;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(environment.steps == std::vector<Step>{Step::kCreate});
}

void RegistrationFailureDeletesSemaphore() {
    Reset();
    environment.register_result = ESP_FAIL;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    const std::vector<Step> expected{
        Step::kCreate, Step::kRegister, Step::kDelete};
    assert(environment.steps == expected);
}

void CallsReuseOneRegistrationAndSemaphore() {
    Reset();
    environment.unregister_result = ESP_FAIL;
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kCreate) == 1);
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kRegister) == 1);
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kPost) == 2);
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kUnregister) == 0);
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kDelete) == 0);
}

void PostFailureKeepsReusableRegistration() {
    Reset();
    environment.post_result = ESP_FAIL;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kCreate) == 0);
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kRegister) == 0);

    Reset();
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kCreate) == 0);
    assert(std::count(environment.steps.begin(), environment.steps.end(),
                      Step::kRegister) == 0);
}

void SuccessIsFifoAndWaitIsCapped() {
    Reset();
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{5000}));
    assert(environment.wait_ticks == 1000);
}

void LateTimedOutEventCannotSatisfyNextBarrier() {
    Reset();
    environment.deliver_post = false;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
    assert(!pending_event_data.empty());

    environment.deliver_post = true;
    DeliverPendingEvent();
    environment.wait_succeeds = false;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
}

void ExecutorRejectsStaleTicketsAndStopsBeforeBarrier() {
    using Coordinator = WifiScanLeaseCoordinator;
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(acquired.acquired);
    assert(coordinator.CommitSubmission(acquired.lease, false).drain_required);
    const auto first = coordinator.ArmDrainBarrier(acquired.lease);
    assert(first.armed());
    const auto current = coordinator.ArmDrainBarrier(acquired.lease);
    assert(current.armed());

    Reset();
    const auto stale_proof = DefaultEventLoopScanDrainExecutor::Execute(
        coordinator, acquired.lease, first);
    assert(environment.steps.empty());
    assert(!coordinator.CompleteDrain(acquired.lease, stale_proof));

    Reset();
    const auto proof = DefaultEventLoopScanDrainExecutor::Execute(
        coordinator, acquired.lease, current);
    assert(!environment.steps.empty());
    assert(environment.steps.front() == Step::kScanStop);
    assert(std::find(environment.steps.begin(), environment.steps.end(),
                     Step::kPost) != environment.steps.end());
    assert(coordinator.CompleteDrain(acquired.lease, proof));
}

void ScanStopFailureStillRunsBarrierButCannotRelease() {
    using Coordinator = WifiScanLeaseCoordinator;
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kBlufi);
    assert(acquired.acquired);
    assert(coordinator.CommitSubmission(acquired.lease, true).accepted);
    assert(coordinator.BeginDrain(acquired.lease));
    const auto drain = coordinator.ArmDrainBarrier(acquired.lease);
    assert(drain.armed());

    Reset();
    environment.scan_stop_result = ESP_FAIL;
    const auto proof = DefaultEventLoopScanDrainExecutor::Execute(
        coordinator, acquired.lease, drain);
    assert(environment.steps.front() == Step::kScanStop);
    assert(environment.steps.back() == Step::kTake);
    assert(!coordinator.CompleteDrain(acquired.lease, proof));
}

void WifiStateStopFailureStillRunsBarrierButCannotRelease() {
    using Coordinator = WifiScanLeaseCoordinator;
    Coordinator coordinator;
    const auto acquired = coordinator.TryAcquire(Coordinator::Owner::kStation);
    assert(acquired.acquired);
    assert(coordinator.CommitSubmission(acquired.lease, true).accepted);
    assert(coordinator.BeginDrain(acquired.lease));
    const auto drain = coordinator.ArmDrainBarrier(acquired.lease);
    assert(drain.armed());

    Reset();
    environment.scan_stop_result = ESP_ERR_WIFI_STATE;
    const auto proof = DefaultEventLoopScanDrainExecutor::Execute(
        coordinator, acquired.lease, drain);
    assert(environment.steps.front() == Step::kScanStop);
    assert(environment.steps.back() == Step::kTake);
    assert(!coordinator.CompleteDrain(acquired.lease, proof));
    assert(!coordinator.TryAcquire(Coordinator::Owner::kBlufi).acquired);
}

}  // namespace

struct NativeSemaphore {
    bool given = false;
};

SemaphoreHandle_t xSemaphoreCreateBinary() {
    environment.steps.push_back(Step::kCreate);
    return environment.allocation_succeeds ? new NativeSemaphore : nullptr;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    environment.steps.push_back(Step::kGive);
    semaphore->given = true;
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t ticks_to_wait) {
    environment.steps.push_back(Step::kTake);
    environment.wait_ticks = std::max(environment.wait_ticks, ticks_to_wait);
    if (!semaphore->given ||
        (!environment.wait_succeeds && ticks_to_wait != 0)) {
        return pdFALSE;
    }
    semaphore->given = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    environment.steps.push_back(Step::kDelete);
    delete semaphore;
}

esp_err_t esp_event_handler_instance_register(
        esp_event_base_t, int32_t, esp_event_handler_t event_handler,
        void* event_handler_arg, esp_event_handler_instance_t* instance) {
    environment.steps.push_back(Step::kRegister);
    if (environment.register_result == ESP_OK) {
        registered_handler = event_handler;
        registered_argument = event_handler_arg;
        *instance = reinterpret_cast<void*>(1);
    }
    return environment.register_result;
}

esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id,
                         const void* event_data, size_t event_data_size,
                         TickType_t) {
    environment.steps.push_back(Step::kPost);
    if (environment.post_result == ESP_OK) {
        if (environment.deliver_post) {
            registered_handler(registered_argument, event_base, event_id,
                               const_cast<void*>(event_data));
        } else {
            const auto* bytes = static_cast<const uint8_t*>(event_data);
            pending_event_data.assign(bytes, bytes + event_data_size);
            pending_event_base = event_base;
            pending_event_id = event_id;
        }
    }
    return environment.post_result;
}

esp_err_t esp_event_handler_instance_unregister(
        esp_event_base_t, int32_t, esp_event_handler_instance_t) {
    environment.steps.push_back(Step::kUnregister);
    if (environment.unregister_result == ESP_OK) {
        registered_handler = nullptr;
        registered_argument = nullptr;
    }
    return environment.unregister_result;
}

esp_err_t esp_wifi_scan_stop() {
    environment.steps.push_back(Step::kScanStop);
    return environment.scan_stop_result;
}

int main() {
    AllocationFailureReturnsFalseWithoutCleanup();
    RegistrationFailureDeletesSemaphore();
    CallsReuseOneRegistrationAndSemaphore();
    PostFailureKeepsReusableRegistration();
    SuccessIsFifoAndWaitIsCapped();
    LateTimedOutEventCannotSatisfyNextBarrier();
    ExecutorRejectsStaleTicketsAndStopsBeforeBarrier();
    ScanStopFailureStillRunsBarrierButCannotRelease();
    WifiStateStopFailureStillRunsBarrierButCannotRelease();
    std::cout << "default_event_loop_barrier_host_test: PASS\n";
    return 0;
}
