#include "../../main/boards/common/blufi_wifi_scan_lease_timer.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>

namespace {

using Timer = BlufiWifiScanLeaseTimer;

struct FakeDriver {
    std::mutex mutex;
    int creates = 0;
    int starts = 0;
    int stops = 0;
    bool fail_create = false;
    bool fail_start = false;
    void (*callback)(void*) = nullptr;
    void* callback_arg = nullptr;
    int handle = 1;
    std::atomic<int64_t> now_us{0};
    std::atomic<bool> block_before_claim{false};
    std::atomic<bool> entered_before_claim{false};
    std::atomic<bool> release_before_claim{false};
};

bool Create(void* context, void (*callback)(void*), void* callback_arg,
            void** handle) noexcept {
    auto& driver = *static_cast<FakeDriver*>(context);
    std::lock_guard<std::mutex> lock(driver.mutex);
    ++driver.creates;
    if (driver.fail_create) {
        return false;
    }
    driver.callback = callback;
    driver.callback_arg = callback_arg;
    *handle = &driver.handle;
    return true;
}

void Stop(void* context, void*) noexcept {
    auto& driver = *static_cast<FakeDriver*>(context);
    std::lock_guard<std::mutex> lock(driver.mutex);
    ++driver.stops;
}

bool StartOnce(void* context, void*, int64_t) noexcept {
    auto& driver = *static_cast<FakeDriver*>(context);
    std::lock_guard<std::mutex> lock(driver.mutex);
    ++driver.starts;
    return !driver.fail_start;
}

int64_t NowUs(void* context) noexcept {
    return static_cast<FakeDriver*>(context)->now_us.load();
}

void BeforeClaim(void* context) noexcept {
    auto& driver = *static_cast<FakeDriver*>(context);
    if (!driver.block_before_claim.load()) {
        return;
    }
    driver.entered_before_claim.store(true);
    while (!driver.release_before_claim.load()) {
        std::this_thread::yield();
    }
}

struct Sink {
    std::mutex mutex;
    Timer::ExactTuple signal;
    int signal_count = 0;
};

void Signal(void* context, Timer::ExactTuple tuple) noexcept {
    auto& sink = *static_cast<Sink*>(context);
    std::lock_guard<std::mutex> lock(sink.mutex);
    sink.signal = tuple;
    ++sink.signal_count;
}

Timer::ExactTuple Tuple(uint64_t request, uint64_t lease,
                        uint32_t incarnation) {
    return Timer::ExactTuple{
        request,
        WifiScanLeaseCoordinator::Lease{
            WifiScanLeaseCoordinator::Owner::kBlufi, lease, incarnation}};
}

void ConcurrentArmCreatesOneTimerAndSignalsOnlyCurrentTuple() {
    FakeDriver driver;
    Sink sink;
    Timer timer({&driver, Create, Stop, StartOnce, NowUs, BeforeClaim},
                &sink, Signal);
    std::vector<std::thread> workers;
    for (uint64_t id = 1; id <= 16; ++id) {
        workers.emplace_back([&, id]() {
            assert(timer.Arm(Tuple(id, id, 7), 50'000));
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    assert(driver.creates == 1);
    assert(driver.starts == 16);
    driver.now_us.store(50'000);
    driver.callback(driver.callback_arg);
    assert(sink.signal_count == 1);
    assert(sink.signal.request_id == sink.signal.lease.lease_id);
    const auto current = timer.CurrentExactTuple();
    assert(!current.has_value());
    assert(sink.signal.lease.driver_incarnation == 7);
}

void CreateAndStartFailureLeaveExactTupleForAllocationFreeFallback() {
    FakeDriver driver;
    Sink sink;
    Timer timer({&driver, Create, Stop, StartOnce, NowUs, BeforeClaim},
                &sink, Signal);
    const auto exact = Tuple(9, 99, 3);
    driver.fail_create = true;
    assert(!timer.Arm(exact, 50'000));
    assert(timer.FailedExactTuple().has_value());
    driver.fail_create = false;
    driver.fail_start = true;
    assert(!timer.Arm(exact, 50'000));
    assert(timer.FailedExactTuple().has_value());
    assert(sink.signal_count == 0);
}

void ExactDisarmCannotCancelReplacementTuple() {
    FakeDriver driver;
    Sink sink;
    Timer timer({&driver, Create, Stop, StartOnce, NowUs, BeforeClaim},
                &sink, Signal);
    const auto old_tuple = Tuple(1, 10, 1);
    const auto current_tuple = Tuple(2, 20, 1);
    assert(timer.Arm(old_tuple, 50'000));
    assert(timer.Arm(current_tuple, 50'000));
    assert(!timer.Disarm(old_tuple));
    assert(timer.Disarm(current_tuple));
    driver.now_us.store(50'000);
    driver.callback(driver.callback_arg);
    assert(sink.signal_count == 0);
}

void StaleCallbackCannotConsumeNewGenerationBeforeItsDeadline() {
    FakeDriver driver;
    Sink sink;
    Timer timer({&driver, Create, Stop, StartOnce, NowUs, BeforeClaim},
                &sink, Signal);
    assert(timer.Arm(Tuple(1, 10, 1), 50'000));
    driver.now_us.store(50'000);
    assert(timer.Arm(Tuple(2, 20, 1), 50'000));
    driver.callback(driver.callback_arg);  // queued callback from generation 1
    assert(sink.signal_count == 0);
    driver.now_us.store(100'000);
    driver.callback(driver.callback_arg);
    assert(sink.signal_count == 1);
    assert(sink.signal.request_id == 2);
    assert(sink.signal.lease.lease_id == 20);
}

void CallbackPausedAfterDeadlineCannotClaimConcurrentRearm() {
    FakeDriver driver;
    Sink sink;
    Timer timer({&driver, Create, Stop, StartOnce, NowUs, BeforeClaim},
                &sink, Signal);
    assert(timer.Arm(Tuple(1, 10, 1), 50'000));
    driver.now_us.store(50'000);
    driver.block_before_claim.store(true);
    std::thread stale_callback(
        [&]() { driver.callback(driver.callback_arg); });
    while (!driver.entered_before_claim.load()) {
        std::this_thread::yield();
    }
    assert(timer.Arm(Tuple(2, 20, 1), 50'000));
    driver.release_before_claim.store(true);
    stale_callback.join();
    assert(sink.signal_count == 0);
    driver.block_before_claim.store(false);
    driver.now_us.store(100'000);
    driver.callback(driver.callback_arg);
    assert(sink.signal_count == 1);
    assert(sink.signal.request_id == 2);
}

}  // namespace

int main() {
    ConcurrentArmCreatesOneTimerAndSignalsOnlyCurrentTuple();
    CreateAndStartFailureLeaveExactTupleForAllocationFreeFallback();
    ExactDisarmCannotCancelReplacementTuple();
    StaleCallbackCannotConsumeNewGenerationBeforeItsDeadline();
    CallbackPausedAfterDeadlineCannotClaimConcurrentRearm();
}
