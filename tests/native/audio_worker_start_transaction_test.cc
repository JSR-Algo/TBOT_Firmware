#include "audio/audio_worker_start_transaction.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Transaction = AudioWorkerStartTransaction;
using Worker = Transaction::Worker;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "audio worker start transaction test failed: "
                  << message << "\n";
        std::exit(1);
    }
}

const char* Name(Worker worker) {
    switch (worker) {
        case Worker::kOpusCodec: return "opus";
        case Worker::kAudioInput: return "input";
        case Worker::kAudioOutput: return "output";
    }
    return "unknown";
}

void CreatesLargestWorkerFirst() {
    std::vector<std::string> events;
    const bool started = Transaction::StartOnce(
        [&](Worker worker) {
            events.emplace_back(Name(worker));
            return true;
        },
        [&]() { events.emplace_back("rollback"); });

    Require(started, "complete creation succeeds");
    Require(events == std::vector<std::string>({"opus", "input", "output"}),
            "Opus is requested before smaller workers");
}

void FailureAtEachCreationPointRollsBack() {
    for (int fail_index = 0; fail_index < 3; ++fail_index) {
        std::vector<std::string> events;
        int create_index = 0;
        const bool started = Transaction::StartOnce(
            [&](Worker worker) {
                events.emplace_back(Name(worker));
                return create_index++ != fail_index;
            },
            [&]() { events.emplace_back("rollback"); });

        Require(!started, "injected creation failure fails closed");
        Require(events.back() == "rollback",
                "every creation failure invokes rollback");
        Require(static_cast<int>(events.size()) == fail_index + 2,
                "creation stops at the failed worker");
    }
}

void OneFailureRetriesAfterReclaim() {
    std::vector<std::string> events;
    const bool rearmed = Transaction::Rearm(
        [&](uint32_t delay_ms) {
            events.emplace_back("delay:" + std::to_string(delay_ms));
        },
        [&](uint32_t attempt) {
            events.emplace_back("attempt:" + std::to_string(attempt));
            if (attempt == 1) {
                events.emplace_back("cleanup");
                return false;
            }
            events.emplace_back("complete");
            return true;
        });

    Require(rearmed, "second complete attempt succeeds");
    Require(events == std::vector<std::string>({
        "delay:10", "attempt:1", "cleanup", "delay:10",
        "attempt:2", "complete"}),
        "retry occurs only after cleanup and idle reclaim delay");
}

void SecondFailureRemainsStopped() {
    std::vector<std::string> events;
    const bool rearmed = Transaction::Rearm(
        [&](uint32_t delay_ms) {
            events.emplace_back("delay:" + std::to_string(delay_ms));
        },
        [&](uint32_t attempt) {
            events.emplace_back("attempt:" + std::to_string(attempt));
            events.emplace_back("cleanup");
            return false;
        });

    Require(!rearmed, "two failed attempts return false");
    Require(events == std::vector<std::string>({
        "delay:10", "attempt:1", "cleanup", "delay:10",
        "attempt:2", "cleanup"}),
        "rearm is bounded to two attempts");
}

void IncompleteHandleSetCannotReportSuccess() {
    bool opus = false;
    bool input = false;
    bool output = false;
    int attempt_count = 0;

    const bool rearmed = Transaction::Rearm(
        [](uint32_t) {},
        [&](uint32_t) {
            ++attempt_count;
            opus = true;
            input = true;
            output = attempt_count == 2;
            return opus && input && output;
        });

    Require(rearmed, "complete retry reports success");
    Require(attempt_count == 2, "incomplete first set triggers one retry");
    Require(opus && input && output,
            "success is coupled to the complete worker invariant");
}

}  // namespace

int main() {
    CreatesLargestWorkerFirst();
    FailureAtEachCreationPointRollsBack();
    OneFailureRetriesAfterReclaim();
    SecondFailureRemainsStopped();
    IncompleteHandleSetCannotReportSuccess();
    std::cout << "audio worker start transaction test OK\n";
    return 0;
}
