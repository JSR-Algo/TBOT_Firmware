#include "lesson_transport_epoch_gate.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
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

    LessonTransportEpochGate coalesced_gate;
    LessonTransportTerminalControl terminal_control;
    const auto socket_a_epoch = coalesced_gate.PublishedEpoch();
    const auto socket_a_terminal = coalesced_gate.PublishTerminalEpoch();
    const auto socket_a_request = terminal_control.Publish(socket_a_terminal);
    expect(socket_a_request.enqueue_control,
           "first terminal request owns the single queued control");
    const auto socket_b_epoch = coalesced_gate.PublishedEpoch();
    expect(socket_b_epoch == socket_a_terminal,
           "replacement socket opens on first pending terminal epoch");
    const auto socket_b_terminal = coalesced_gate.PublishTerminalEpoch();
    const auto socket_b_request = terminal_control.Publish(socket_b_terminal);
    expect(!socket_b_request.enqueue_control,
           "newer terminal coalesces while older control remains pending");
    expect(coalesced_gate.WorkerApplyTerminal(coalesced_gate.PublishedEpoch()),
           "pending control coalesces worker directly to newest terminal epoch");
    expect(!coalesced_gate.WorkerAcceptFrame(socket_a_epoch),
           "first abandoned socket remains fenced after coalesced terminal");
    expect(!coalesced_gate.WorkerAcceptFrame(socket_b_epoch),
           "newer dead socket frame is fenced even while older control was pending");
    expect(coalesced_gate.WorkerAcceptFrame(socket_b_terminal),
           "next live socket may use newest coalesced epoch");
    expect(!terminal_control.FinishWorkerDrain(coalesced_gate, socket_b_terminal),
           "worker drain completes when no newer terminal was published");

    for (int iteration = 0; iteration < 1000; ++iteration) {
        LessonTransportEpochGate race_gate;
        LessonTransportTerminalControl race_control;
        const auto first_terminal = race_gate.PublishTerminalEpoch();
        expect(race_control.Publish(first_terminal).enqueue_control,
               "race setup owns initial control");
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
        expect(worker_continues != successor.enqueue_control,
               "pending-clear race has exactly one control owner");
        if (worker_continues) {
            const auto latest = race_gate.PublishedEpoch();
            race_gate.WorkerApplyTerminal(latest);
            expect(!race_control.FinishWorkerDrain(race_gate, latest),
                   "worker-owned successor drain terminates");
        } else if (successor.enqueue_control) {
            const auto latest = race_gate.PublishedEpoch();
            race_gate.WorkerApplyTerminal(latest);
            expect(!race_control.FinishWorkerDrain(race_gate, latest),
                   "requester-owned successor control terminates");
        }
        expect(race_gate.WorkerAcceptFrame(race_gate.PublishedEpoch()),
               "pending-clear race converges worker to newest epoch");
    }

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
