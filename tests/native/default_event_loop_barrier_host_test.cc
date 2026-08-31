#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "../../components/esp-wifi-connect/default_event_loop_barrier.cc"

namespace {

enum class Step : uint8_t { kCreate, kRegister, kPost, kGive, kTake, kDelete };

struct Environment {
    bool semaphore_allocation_succeeds = true;
    esp_err_t register_result = ESP_OK;
    esp_err_t post_result = ESP_OK;
    TickType_t maximum_wait_ticks = 0;
    size_t create_count = 0;
    size_t register_count = 0;
    std::vector<Step> steps;
};

struct QueuedEvent {
    esp_event_base_t base = nullptr;
    int32_t id = 0;
    std::vector<uint8_t> data;
};

Environment environment;
std::mutex environment_mutex;
std::atomic<bool> fail_next_nothrow_allocation{false};
esp_event_handler_t registered_handler = nullptr;
void* registered_argument = nullptr;

std::mutex event_mutex;
std::condition_variable event_condition;
std::deque<QueuedEvent> event_queue;
size_t delivery_permits = 0;
size_t delivered_events = 0;
bool stop_event_worker = false;
std::thread event_worker;

void RecordStep(Step step) {
    std::lock_guard<std::mutex> lock(environment_mutex);
    environment.steps.push_back(step);
}

size_t CountStep(Step step) {
    std::lock_guard<std::mutex> lock(environment_mutex);
    return static_cast<size_t>(std::count(
        environment.steps.begin(), environment.steps.end(), step));
}

void ResetCallState() {
    std::lock_guard<std::mutex> lock(environment_mutex);
    environment.register_result = ESP_OK;
    environment.post_result = ESP_OK;
    environment.maximum_wait_ticks = 0;
    environment.steps.clear();
}

void AllowDeliveries(size_t count) {
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        delivery_permits += count;
    }
    event_condition.notify_all();
}

void WaitForQueuedEvents(size_t count) {
    std::unique_lock<std::mutex> lock(event_mutex);
    event_condition.wait(lock, [count]() { return event_queue.size() >= count; });
}

void WaitForDeliveredEvents(size_t count) {
    std::unique_lock<std::mutex> lock(event_mutex);
    event_condition.wait(lock, [count]() { return delivered_events >= count; });
}

void EventWorkerMain() {
    while (true) {
        QueuedEvent event;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_condition.wait(lock, []() {
                return stop_event_worker ||
                       (!event_queue.empty() && delivery_permits != 0);
            });
            if (stop_event_worker) {
                return;
            }
            event = std::move(event_queue.front());
            event_queue.pop_front();
            --delivery_permits;
        }
        registered_handler(registered_argument, event.base, event.id,
                           event.data.data());
        {
            std::lock_guard<std::mutex> lock(event_mutex);
            ++delivered_events;
        }
        event_condition.notify_all();
    }
}

void SingletonAndSemaphoreAllocationFailuresCanRetry() {
    ResetCallState();
    fail_next_nothrow_allocation.store(true, std::memory_order_release);
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
    assert(CountStep(Step::kCreate) == 0);

    ResetCallState();
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        environment.semaphore_allocation_succeeds = false;
    }
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
    assert(CountStep(Step::kCreate) == 1);
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        environment.semaphore_allocation_succeeds = true;
    }
}

void RegistrationFailureCleansUpAndCanRetry() {
    ResetCallState();
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        environment.register_result = ESP_FAIL;
    }
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
    assert(CountStep(Step::kCreate) == 1);
    assert(CountStep(Step::kRegister) == 1);
    assert(CountStep(Step::kDelete) == 1);

    ResetCallState();
    AllowDeliveries(1);
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(CountStep(Step::kCreate) == 1);
    assert(CountStep(Step::kRegister) == 1);
}

void SuccessfulCallsReuseOneRegistrationAndSemaphore() {
    size_t creates_before = 0;
    size_t registrations_before = 0;
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        creates_before = environment.create_count;
        registrations_before = environment.register_count;
    }
    ResetCallState();
    AllowDeliveries(2);
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
    assert(CountStep(Step::kCreate) == 0);
    assert(CountStep(Step::kRegister) == 0);
    assert(CountStep(Step::kPost) == 2);
    std::lock_guard<std::mutex> lock(environment_mutex);
    assert(environment.create_count == creates_before);
    assert(environment.register_count == registrations_before);
}

void PostFailureRetainsReusableRegistration() {
    ResetCallState();
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        environment.post_result = ESP_FAIL;
    }
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
    assert(CountStep(Step::kCreate) == 0);
    assert(CountStep(Step::kRegister) == 0);

    ResetCallState();
    AllowDeliveries(1);
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{100}));
}

void WaitIsCappedAtOneSecond() {
    ResetCallState();
    AllowDeliveries(1);
    assert(DrainDefaultEventLoop(std::chrono::milliseconds{5000}));
    std::lock_guard<std::mutex> lock(environment_mutex);
    assert(environment.maximum_wait_ticks == 1000);
}

void LateOldEventCannotSatisfySubsequentBarrier() {
    ResetCallState();
    size_t delivered_before = 0;
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        delivered_before = delivered_events;
    }
    assert(!DrainDefaultEventLoop(std::chrono::milliseconds{10}));
    WaitForQueuedEvents(1);

    std::atomic<bool> second_completed{false};
    bool second_result = false;
    std::thread second([&]() {
        second_result = DrainDefaultEventLoop(std::chrono::milliseconds{1000});
        second_completed.store(true, std::memory_order_release);
    });
    WaitForQueuedEvents(2);

    AllowDeliveries(1);
    WaitForDeliveredEvents(delivered_before + 1);
    assert(!second_completed.load(std::memory_order_acquire));
    AllowDeliveries(1);
    second.join();
    assert(second_result);
}

}  // namespace

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (fail_next_nothrow_allocation.exchange(false,
                                               std::memory_order_acq_rel)) {
        return nullptr;
    }
    return std::malloc(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

struct NativeSemaphore {
    std::mutex mutex;
    std::condition_variable condition;
    bool given = false;
};

SemaphoreHandle_t xSemaphoreCreateBinary() {
    RecordStep(Step::kCreate);
    std::lock_guard<std::mutex> lock(environment_mutex);
    ++environment.create_count;
    return environment.semaphore_allocation_succeeds ? new NativeSemaphore
                                                      : nullptr;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    RecordStep(Step::kGive);
    {
        std::lock_guard<std::mutex> lock(semaphore->mutex);
        semaphore->given = true;
    }
    semaphore->condition.notify_one();
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t ticks_to_wait) {
    RecordStep(Step::kTake);
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        environment.maximum_wait_ticks = std::max(
            environment.maximum_wait_ticks, ticks_to_wait);
    }
    std::unique_lock<std::mutex> lock(semaphore->mutex);
    if (!semaphore->given && ticks_to_wait != 0) {
        semaphore->condition.wait_for(
            lock, std::chrono::milliseconds{ticks_to_wait},
            [semaphore]() { return semaphore->given; });
    }
    if (!semaphore->given) {
        return pdFALSE;
    }
    semaphore->given = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    RecordStep(Step::kDelete);
    delete semaphore;
}

esp_err_t esp_event_handler_instance_register(
        esp_event_base_t, int32_t, esp_event_handler_t event_handler,
        void* event_handler_arg, esp_event_handler_instance_t* instance) {
    RecordStep(Step::kRegister);
    std::lock_guard<std::mutex> lock(environment_mutex);
    ++environment.register_count;
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
    RecordStep(Step::kPost);
    {
        std::lock_guard<std::mutex> lock(environment_mutex);
        if (environment.post_result != ESP_OK) {
            return environment.post_result;
        }
    }
    QueuedEvent event;
    event.base = event_base;
    event.id = event_id;
    const auto* bytes = static_cast<const uint8_t*>(event_data);
    event.data.assign(bytes, bytes + event_data_size);
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        event_queue.push_back(std::move(event));
    }
    event_condition.notify_all();
    return ESP_OK;
}

int main() {
    event_worker = std::thread(EventWorkerMain);
    SingletonAndSemaphoreAllocationFailuresCanRetry();
    RegistrationFailureCleansUpAndCanRetry();
    SuccessfulCallsReuseOneRegistrationAndSemaphore();
    PostFailureRetainsReusableRegistration();
    WaitIsCappedAtOneSecond();
    LateOldEventCannotSatisfySubsequentBarrier();
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        stop_event_worker = true;
    }
    event_condition.notify_all();
    event_worker.join();
    std::cout << "default_event_loop_barrier_host_test: PASS\n";
    return 0;
}
