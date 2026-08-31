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
    TickType_t wait_ticks = 0;
    std::vector<Step> steps;
};

Environment environment;
esp_event_handler_t registered_handler = nullptr;
void* registered_argument = nullptr;

void Reset() {
    environment = Environment{};
    registered_handler = nullptr;
    registered_argument = nullptr;
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

void PostWaitAndUnregisterFailuresStillCleanUp() {
    Reset();
    environment.post_result = ESP_FAIL;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    const std::vector<Step> post_failure{
        Step::kCreate, Step::kRegister, Step::kPost,
        Step::kUnregister, Step::kDelete};
    assert(environment.steps == post_failure);

    Reset();
    environment.wait_succeeds = false;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    const std::vector<Step> wait_failure{
        Step::kCreate, Step::kRegister, Step::kPost, Step::kGive,
        Step::kTake, Step::kUnregister, Step::kDelete};
    assert(environment.steps == wait_failure);

    Reset();
    environment.unregister_result = ESP_FAIL;
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    const std::vector<Step> unregister_failure{
        Step::kCreate, Step::kRegister, Step::kPost, Step::kGive,
        Step::kTake, Step::kUnregister, Step::kDelete};
    assert(environment.steps == unregister_failure);
}

void SuccessIsFifoAndWaitIsCapped() {
    Reset();
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{5000}));
    const std::vector<Step> expected{
        Step::kCreate, Step::kRegister, Step::kPost, Step::kGive,
        Step::kTake, Step::kUnregister, Step::kDelete};
    assert(environment.steps == expected);
    assert(environment.wait_ticks == 1000);
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
    assert(environment.steps[1] == Step::kCreate);
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
    assert(environment.steps.back() == Step::kDelete);
    assert(!coordinator.CompleteDrain(acquired.lease, proof));
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
    environment.wait_ticks = ticks_to_wait;
    return semaphore->given && environment.wait_succeeds ? pdTRUE : pdFALSE;
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
                         const void*, size_t, TickType_t) {
    environment.steps.push_back(Step::kPost);
    if (environment.post_result == ESP_OK) {
        registered_handler(registered_argument, event_base, event_id, nullptr);
    }
    return environment.post_result;
}

esp_err_t esp_event_handler_instance_unregister(
        esp_event_base_t, int32_t, esp_event_handler_instance_t) {
    environment.steps.push_back(Step::kUnregister);
    registered_handler = nullptr;
    registered_argument = nullptr;
    return environment.unregister_result;
}

esp_err_t esp_wifi_scan_stop() {
    environment.steps.push_back(Step::kScanStop);
    return environment.scan_stop_result;
}

int main() {
    AllocationFailureReturnsFalseWithoutCleanup();
    RegistrationFailureDeletesSemaphore();
    PostWaitAndUnregisterFailuresStillCleanUp();
    SuccessIsFifoAndWaitIsCapped();
    ExecutorRejectsStaleTicketsAndStopsBeforeBarrier();
    ScanStopFailureStillRunsBarrierButCannotRelease();
    std::cout << "default_event_loop_barrier_host_test: PASS\n";
    return 0;
}
