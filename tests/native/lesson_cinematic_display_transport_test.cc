#define TBOT_LESSON_CINEMATIC_TRANSPORT_ONLY
#include "display/lcd_display.h"

#include <cstdlib>
#include <iostream>

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
    bool begin_ok = true;
    bool queue_ok = true;
    bool wait_ok = true;
    bool completion_ready = false;
    const std::uint16_t* queued_pixels = nullptr;
    std::uint32_t last_timeout_ms = 0;
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
    return fake.wait_ok && fake.completion_ready;
}

void End(void* raw) { ++static_cast<FakeTransport*>(raw)->ends; }

LessonCinematicDisplayTransport MakeTransport(FakeTransport* fake) {
    return LessonCinematicDisplayTransport({fake, Begin, Queue, Wait, End});
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
    auto timed_out = MakeTransport(&timeout);
    Require(timed_out.BeginLessonCinematic(), "timeout test acquires ownership");
    Require(timed_out.QueueLessonCinematicFrame(pixels, 320, 480),
            "timeout test queues frame");
    Require(!timed_out.WaitLessonCinematicFrame(9), "completion timeout is reported");
    Require(timeout.ends == 1, "completion timeout resumes LVGL exactly once");
    timed_out.EndLessonCinematic();
    Require(timeout.ends == 1, "end after timeout does not resume twice");
}

}  // namespace

int main() {
    TestOwnershipCompletionAndReuse();
    TestValidationAndCallbackBeforeWait();
    TestFailureCleanup();
    std::cout << "lesson cinematic display transport test passed\n";
    return 0;
}
