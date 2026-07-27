#include "lesson_transport_epoch_gate.h"
#include "lesson_handler.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
int checks = 0;
void expect(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class ReusableWebsocketSource {
public:
    explicit ReusableWebsocketSource(LessonTransportEpochGate& gate) : gate_(gate) {}

    std::uint64_t Open() {
        return gate_.PublishedEpoch();
    }

private:
    LessonTransportEpochGate& gate_;
};
}

int main() {
    LessonTransportEpochGate gate;
    const auto original = gate.PublishedEpoch();
    expect(gate.WorkerAcceptFrame(original), "initial epoch accepted");
    const auto terminal = gate.PublishTerminalEpoch();
    expect(!gate.WorkerAcceptFrame(terminal), "future frame waits for control point");
    expect(gate.WorkerApplyTerminal(terminal), "terminal control advances worker epoch");
    expect(!gate.WorkerAcceptFrame(original), "queued stale frame rejected after control");
    expect(gate.WorkerAcceptFrame(terminal), "replacement epoch accepted after control");
    expect(!gate.WorkerApplyTerminal(original), "stale control cannot roll epoch back");
    const auto old_callback_epoch = original;
    const auto replacement_callback_epoch = terminal;
    expect(!gate.WorkerAcceptFrame(old_callback_epoch),
           "late old transport callback cannot borrow replacement epoch");
    expect(gate.WorkerAcceptFrame(replacement_callback_epoch),
           "replacement transport callback retains accepted epoch");

    ReusableWebsocketSource websocket(gate);
    const auto first_socket_callback_epoch = websocket.Open();
    const auto reconnect_terminal = gate.PublishTerminalEpoch();
    const auto reopened_socket_callback_epoch = websocket.Open();
    expect(first_socket_callback_epoch != reopened_socket_callback_epoch,
           "same protocol reopen captures a distinct current transport epoch");
    expect(gate.WorkerApplyTerminal(reconnect_terminal),
           "reconnect terminal control advances worker before replacement frames");
    expect(!gate.WorkerAcceptFrame(first_socket_callback_epoch),
           "late callback from old socket stays fenced after same protocol reopen");
    expect(gate.WorkerAcceptFrame(reopened_socket_callback_epoch),
           "frame from reopened socket is accepted with its captured epoch");

    static_assert(std::is_trivially_copyable<LessonQueueItem>::value,
                  "FreeRTOS queue items must carry completion identity by value");
    LessonQueueItem completed = MakeLessonVisualQueueItem(
        LessonQueueItemKind::kVisualCompleted,
        terminal,
        17,
        12,
        "assignment-id",
        "session-id",
        "step-id",
        LessonVisualCompletionResult::kApplied,
        nullptr);
    expect(completed.payload == nullptr, "completion item owns no serialized frame payload");
    expect(completed.transport_epoch == terminal && completed.visual_generation == 17,
           "completion item carries transport epoch and visual generation");
    expect(completed.server_sequence == 12 &&
               std::strcmp(completed.assignment_id, "assignment-id") == 0 &&
               std::strcmp(completed.session_id, "session-id") == 0 &&
               std::strcmp(completed.step_id, "step-id") == 0,
           "completion item carries server sequence and session identity by value");
    expect(completed.completion_result == LessonVisualCompletionResult::kApplied,
           "completion item carries its result by value");

    LessonTransportTerminalControl terminal_control;
    LessonTransportEpochGate coalesced_gate;
    const auto first_terminal = coalesced_gate.PublishTerminalEpoch();
    expect(terminal_control.Publish(first_terminal).enqueue_control,
           "first terminal request owns the queued front control");
    const auto second_terminal = coalesced_gate.PublishTerminalEpoch();
    expect(!terminal_control.Publish(second_terminal).enqueue_control,
           "newer terminal coalesces while the front control is pending");
    const auto coalesced_epoch = coalesced_gate.PublishedEpoch();
    expect(coalesced_epoch == second_terminal &&
               coalesced_gate.WorkerApplyTerminal(coalesced_epoch),
           "worker applies the newest coalesced epoch");
    expect(!terminal_control.FinishWorkerDrain(coalesced_gate, coalesced_epoch),
           "coalesced drain ends when no successor terminal exists");

    LessonQueueDataAdmission data_admission(kLessonMessageDataQueueDepth);
    std::atomic<bool> admission_start{false};
    std::atomic<int> admitted_frames{0};
    std::atomic<int> admitted_visuals{0};
    std::vector<std::thread> data_producers;
    for (int index = 0; index < 64; ++index) {
        data_producers.emplace_back([&, index]() {
            while (!admission_start.load(std::memory_order_acquire)) {}
            if (!data_admission.TryAcquire()) return;
            if ((index % 2) == 0) {
                admitted_frames.fetch_add(1, std::memory_order_relaxed);
            } else {
                admitted_visuals.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    admission_start.store(true, std::memory_order_release);
    for (auto& producer : data_producers) producer.join();
    expect(admitted_frames.load() + admitted_visuals.load() ==
               static_cast<int>(kLessonMessageDataQueueDepth),
           "mixed frame and visual producers cannot consume the reserved control slot");
    expect(admitted_frames.load() > 0 && admitted_visuals.load() > 0,
           "stress fills data capacity through both non-control producer paths");
    expect(!data_admission.TryAcquire(), "full data capacity rejects another visual completion");
    data_admission.Release();
    expect(data_admission.TryAcquire(), "worker receive returns one shared data admission slot");

    LessonTransportEpochGate failed_enqueue_gate;
    LessonTransportTerminalControl failed_enqueue_control;
    const auto stale_full_queue_epoch = failed_enqueue_gate.PublishedEpoch();
    const auto first_failed_enqueue_epoch = failed_enqueue_gate.PublishTerminalEpoch();
    expect(failed_enqueue_control.Publish(first_failed_enqueue_epoch).enqueue_control,
           "terminal publication owns a wakeup attempt");
    const auto latest_failed_enqueue_epoch = failed_enqueue_gate.PublishTerminalEpoch();
    expect(!failed_enqueue_control.Publish(latest_failed_enqueue_epoch).enqueue_control,
           "newest abandonment coalesces while mixed producers keep the data queue full");
    expect(failed_enqueue_control.WorkerShouldDrain(),
           "published terminal remains worker-visible when the queue wakeup fails");
    expect(failed_enqueue_gate.WorkerApplyTerminal(failed_enqueue_gate.PublishedEpoch()),
           "worker applies latest epoch without requiring a queued control item");
    expect(!failed_enqueue_control.FinishWorkerDrain(
               failed_enqueue_gate, failed_enqueue_gate.PublishedEpoch()),
           "fallback terminal drain converges without deadlock");
    expect(!failed_enqueue_gate.WorkerAcceptFrame(stale_full_queue_epoch),
           "fallback terminal drain rejects stale queued work");
    expect(failed_enqueue_gate.WorkerAcceptFrame(latest_failed_enqueue_epoch),
           "fallback terminal drain accepts work only from the newest replacement epoch");

    for (int iteration = 0; iteration < 1000; ++iteration) {
        LessonTransportEpochGate race_gate;
        LessonTransportTerminalControl race_control;
        const auto initial_terminal = race_gate.PublishTerminalEpoch();
        expect(race_control.Publish(initial_terminal).enqueue_control,
               "race setup owns the initial control");
        std::atomic<bool> release{false};
        LessonTransportTerminalControl::PublishResult successor{};
        std::thread requester([&]() {
            while (!release.load(std::memory_order_acquire)) {}
            const auto next_terminal = race_gate.PublishTerminalEpoch();
            successor = race_control.Publish(next_terminal);
        });
        release.store(true, std::memory_order_release);
        const auto applied = race_gate.PublishedEpoch();
        race_gate.WorkerApplyTerminal(applied);
        const bool worker_continues = race_control.FinishWorkerDrain(race_gate, applied);
        requester.join();
        const bool already_coalesced = !worker_continues && !successor.enqueue_control &&
                                       applied == race_gate.PublishedEpoch();
        expect(worker_continues != successor.enqueue_control || already_coalesced,
               "pending-clear race has one successor owner or already applied the newest epoch");
        if (worker_continues || successor.enqueue_control) {
            const auto latest = race_gate.PublishedEpoch();
            race_gate.WorkerApplyTerminal(latest);
            expect(!race_control.FinishWorkerDrain(race_gate, latest),
                   "successor owner drains to the newest terminal epoch");
        }
        expect(race_gate.WorkerAcceptFrame(race_gate.PublishedEpoch()),
               "pending-clear race converges on the newest epoch");
    }

    LessonQueueItem timed_out = completed;
    timed_out.kind = LessonQueueItemKind::kVisualTimedOut;
    timed_out.completion_result = LessonVisualCompletionResult::kPhaseTimeout;
    expect(timed_out.payload == nullptr &&
               timed_out.completion_result == LessonVisualCompletionResult::kPhaseTimeout,
           "timeout item carries a typed result without borrowing a frame");

    LessonTransportEpochGate completion_gate;
    const auto stale_completion_epoch = completion_gate.PublishedEpoch();
    const auto live_completion_epoch = completion_gate.PublishTerminalEpoch();
    expect(completion_gate.WorkerApplyTerminal(live_completion_epoch),
           "abandon control advances the worker before completion handling");
    expect(!completion_gate.WorkerAcceptFrame(stale_completion_epoch),
           "completion from the abandoned transport epoch is rejected");
    expect(completion_gate.WorkerAcceptFrame(live_completion_epoch),
           "completion from the current transport epoch remains eligible");

    std::atomic<bool> start{false};
    std::vector<std::thread> publishers;
    for (int index = 0; index < 8; ++index) {
        publishers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            gate.PublishTerminalEpoch();
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : publishers) thread.join();
    const auto newest = gate.PublishedEpoch();
    expect(newest == reconnect_terminal + publishers.size(), "terminal epochs are monotonic");
    expect(gate.WorkerApplyTerminal(newest), "worker applies newest concurrent epoch");
    expect(gate.WorkerAcceptFrame(newest), "newest replacement frame accepted");
    std::cout << "lesson transport epoch gate PASS checks=" << checks << '\n';
    return 0;
}
