#define TBOT_LESSON_CINEMATIC_TRANSPORT_ONLY
#include "display/lcd_display.h"

#include <cstdlib>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lesson cinematic display transport test failed: " << message << '\n';
        std::exit(1);
    }
}

struct FakeTransport {
    int begins = 0;
    int queues = 0;
    int waits = 0;
    int ends = 0;
    int resumes = 0;
    bool begin_ok = true;
    bool queue_ok = true;
    bool wait_ok = true;
    bool end_ok = true;
    bool completion_ready = false;
    const std::uint16_t* queued_pixels = nullptr;
    std::uint32_t last_timeout_ms = 0;
    bool block_wait = false;
    bool wait_entered = false;
    bool release_wait = false;
    std::mutex mutex;
    std::condition_variable condition;
};

bool Begin(void* raw) {
    auto& fake = *static_cast<FakeTransport*>(raw);
    ++fake.begins;
    return fake.begin_ok;
}

bool Queue(void* raw, const std::uint16_t* pixels) {
    auto& fake = *static_cast<FakeTransport*>(raw);
    ++fake.queues;
    fake.queued_pixels = pixels;
    return fake.queue_ok;
}

bool Wait(void* raw, std::uint32_t timeout_ms) {
    auto& fake = *static_cast<FakeTransport*>(raw);
    ++fake.waits;
    fake.last_timeout_ms = timeout_ms;
    if (fake.block_wait) {
        std::unique_lock<std::mutex> lock(fake.mutex);
        fake.wait_entered = true;
        fake.condition.notify_all();
        fake.condition.wait(lock, [&fake] { return fake.release_wait; });
    }
    return fake.wait_ok && fake.completion_ready;
}

bool End(void* raw) {
    auto& fake = *static_cast<FakeTransport*>(raw);
    ++fake.ends;
    if (fake.end_ok) ++fake.resumes;
    return fake.end_ok;
}

LessonCinematicDisplayTransport MakeTransport(FakeTransport* fake) {
    return LessonCinematicDisplayTransport({fake, Begin, Queue, Wait, End});
}

void TestPanelIdleGenerationRejectsDelayedLvglCompletion() {
    LessonCinematicPanelCompletionGate gate;
    Require(!gate.OnPanelCompletion(), "delayed LVGL completion is not cinematic");
    gate.ArmNextCompletion();
    Require(gate.OnPanelCompletion(), "first post-barrier completion owns cinematic generation");
    Require(!gate.OnPanelCompletion(), "following LVGL completion is routed normally");
}

void TestOwnershipCompletionAndReuse() {
    FakeTransport fake;
    auto transport = MakeTransport(&fake);
    std::uint16_t pixels[1] = {};

    Require(!transport.QueueLessonCinematicFrame(pixels, 320, 480),
            "queue outside cinematic ownership is rejected");
    Require(transport.BeginLessonCinematic(), "begin acquires cinematic ownership");
    Require(!transport.BeginLessonCinematic(), "second begin is rejected");
    Require(transport.QueueLessonCinematicFrame(pixels, 320, 480),
            "first valid native frame is queued");
    Require(!transport.QueueLessonCinematicFrame(pixels, 320, 480),
            "in-flight buffer cannot be queued twice");
    fake.completion_ready = true;
    Require(transport.WaitLessonCinematicFrame(37), "completion releases buffer ownership");
    Require(fake.last_timeout_ms == 37, "wait forwards its timeout");
    fake.completion_ready = false;
    Require(transport.QueueLessonCinematicFrame(pixels, 320, 480),
            "completed buffer may be reused for a later frame");
    fake.completion_ready = true;
    Require(transport.WaitLessonCinematicFrame(1), "reused buffer completes normally");
    transport.EndLessonCinematic();
    transport.EndLessonCinematic();
    Require(fake.ends == 1, "duplicate end resumes LVGL exactly once");
}

void TestValidationAndCallbackBeforeWait() {
    FakeTransport fake;
    auto transport = MakeTransport(&fake);
    std::uint16_t pixels[1] = {};

    Require(transport.BeginLessonCinematic(), "begin succeeds");
    Require(!transport.QueueLessonCinematicFrame(nullptr, 320, 480),
            "null framebuffer is rejected");
    Require(!transport.QueueLessonCinematicFrame(pixels, 480, 320),
            "logical landscape geometry is rejected");
    Require(!transport.QueueLessonCinematicFrame(pixels, 320, 479),
            "wrong native height is rejected");
    fake.completion_ready = true;
    Require(transport.QueueLessonCinematicFrame(pixels, 320, 480),
            "callback may complete immediately after queue");
    Require(transport.WaitLessonCinematicFrame(0),
            "callback-before-wait remains observable");
    transport.EndLessonCinematic();
}

void TestFailureCleanup() {
    std::uint16_t pixels[1] = {};

    FakeTransport begin_failure;
    begin_failure.begin_ok = false;
    auto failed_begin = MakeTransport(&begin_failure);
    Require(!failed_begin.BeginLessonCinematic(), "begin failure is reported");
    Require(begin_failure.ends == 1, "begin failure runs resume cleanup exactly once");
    failed_begin.EndLessonCinematic();
    Require(begin_failure.ends == 1, "end after begin failure is idempotent");

    FakeTransport queue_failure;
    queue_failure.queue_ok = false;
    auto failed_queue = MakeTransport(&queue_failure);
    Require(failed_queue.BeginLessonCinematic(), "queue failure test acquires ownership");
    Require(!failed_queue.QueueLessonCinematicFrame(pixels, 320, 480),
            "panel submission failure is reported");
    Require(queue_failure.ends == 1, "queue failure resumes LVGL exactly once");
    Require(!failed_queue.QueueLessonCinematicFrame(pixels, 320, 480),
            "queue failure relinquishes ownership");

    FakeTransport timeout;
    timeout.end_ok = false;
    auto timed_out = MakeTransport(&timeout);
    Require(timed_out.BeginLessonCinematic(), "timeout test acquires ownership");
    Require(timed_out.QueueLessonCinematicFrame(pixels, 320, 480),
            "timeout test queues frame");
    Require(!timed_out.WaitLessonCinematicFrame(9), "completion timeout is reported");
    Require(timeout.ends == 0 && timeout.resumes == 0,
            "completion timeout leaves ownership quarantined without cleanup");
    Require(!timed_out.BeginLessonCinematic(),
            "unknown DMA completion rejects new cinematic ownership");
    Require(!timed_out.QueueLessonCinematicFrame(pixels, 320, 480),
            "unknown DMA completion keeps the framebuffer quarantined");
    timed_out.EndLessonCinematic();
    Require(timeout.ends == 1 && timeout.resumes == 0,
            "end while DMA is pending returns without resuming LVGL");
    timeout.completion_ready = true;
    Require(timed_out.WaitLessonCinematicFrame(0),
            "late callback releases the quarantined framebuffer");
    timeout.end_ok = true;
    timed_out.EndLessonCinematic();
    Require(timeout.resumes == 1, "late completion permits exactly one LVGL resume");
    Require(timed_out.BeginLessonCinematic(),
            "ownership can be reacquired after real DMA completion");
    timed_out.EndLessonCinematic();
}

void TestConcurrentEndDuringWaitSerializesCleanup() {
    FakeTransport fake;
    fake.block_wait = true;
    fake.completion_ready = true;
    auto transport = MakeTransport(&fake);
    std::uint16_t pixels[1] = {};
    Require(transport.BeginLessonCinematic(), "concurrent test begins ownership");
    Require(transport.QueueLessonCinematicFrame(pixels, 320, 480),
            "concurrent test queues frame");

    bool wait_result = false;
    std::thread waiter([&] { wait_result = transport.WaitLessonCinematicFrame(50); });
    {
        std::unique_lock<std::mutex> lock(fake.mutex);
        fake.condition.wait(lock, [&fake] { return fake.wait_entered; });
    }
    std::thread ender([&] { transport.EndLessonCinematic(); });
    ender.join();
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        fake.release_wait = true;
    }
    fake.condition.notify_all();
    waiter.join();

    Require(wait_result, "blocked wait observes the real completion");
    Require(fake.ends == 1 && fake.resumes == 1,
            "concurrent end is deferred and resumes exactly once after wait");
}

}  // namespace

int main() {
    TestPanelIdleGenerationRejectsDelayedLvglCompletion();
    TestOwnershipCompletionAndReuse();
    TestValidationAndCallbackBeforeWait();
    TestFailureCleanup();
    TestConcurrentEndDuringWaitSerializesCleanup();
    std::cout << "lesson cinematic display transport test passed\n";
    return 0;
}
