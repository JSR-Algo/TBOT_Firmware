#include "../../main/boards/common/blufi_wifi_scan_controller.h"

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

using Controller = BlufiWifiScanController;

namespace {

Controller::Request Request(uint32_t generation, uint64_t session,
                            uint64_t connection, bool send = true) {
    return Controller::Request{
        generation,
        session,
        connection,
        true,
        send,
    };
}

void ClaimStart(Controller& controller, uint64_t request_id) {
    assert(controller.ClaimStart(request_id).claimed);
}

class Barrier {
public:
    explicit Barrier(unsigned participants) : participants_(participants) {}

    void ArriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const unsigned generation = generation_;
        if (++arrived_ == participants_) {
            arrived_ = 0;
            ++generation_;
            condition_.notify_all();
            return;
        }
        condition_.wait(lock, [this, generation]() {
            return generation_ != generation;
        });
    }

private:
    const unsigned participants_;
    std::mutex mutex_;
    std::condition_variable condition_;
    unsigned arrived_ = 0;
    unsigned generation_ = 0;
};

class Signal {
public:
    void Notify() {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
        condition_.notify_one();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return ready_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool ready_ = false;
};

void DelayedOldCompletionCannotSatisfyNewRequest() {
    Controller controller;
    const auto first = controller.RequestScan(Request(1, 11, 101));
    assert(first.start_now);
    ClaimStart(controller, first.request_id);
    assert(controller.CommitStart(first.request_id, true).accepted);

    controller.InvalidateSession(2, 22, 202);
    const auto second = controller.RequestScan(Request(2, 22, 202));
    assert(second.queued);

    const auto old = controller.BeginCompletion(2, 22, 202);
    assert(old.owned_callback);
    assert(old.discard_results);
    assert(!old.save_results);
    assert(!old.send_list);

    const auto drained = controller.FinishCompletion(old.request_id);
    assert(drained.start_pending);
    assert(drained.request_id != old.request_id);
    assert(drained.pending.setup_generation == 2);
    assert(drained.pending.ble_connection_epoch == 202);
}

void InvalidateBeforeClaimRetiresReservation() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    assert(old.start_now);
    assert(!controller.BeginCompletion(1, 11, 101).owned_callback);
    assert(!controller.CommitStart(old.request_id, true).accepted);
    assert(controller.phase() == Controller::Phase::kStarting);

    const auto retired = controller.InvalidateSession(2, 22, 202);
    assert(!retired.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
    assert(!controller.ClaimStart(old.request_id).claimed);
    assert(!controller.CommitStart(old.request_id, true).accepted);
    assert(!controller.BeginCompletion(2, 22, 202).owned_callback);

    const auto current = controller.RequestScan(Request(2, 22, 202));
    assert(current.start_now);
    assert(current.request_id != old.request_id);
    ClaimStart(controller, current.request_id);
}

void InvalidateBeforeClaimPromotesValidPendingReservation() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    const auto queued = controller.RequestScan(Request(2, 22, 202));
    assert(queued.queued);

    const auto promoted = controller.InvalidateSession(2, 22, 202);
    assert(promoted.start_pending);
    assert(promoted.request_id != old.request_id);
    assert(promoted.pending.setup_generation == 2);
    assert(controller.phase() == Controller::Phase::kStarting);
    assert(!controller.ClaimStart(old.request_id).claimed);
    assert(controller.ClaimStart(promoted.request_id).claimed);
}

void InvalidateAfterClaimDrainsAcceptedSubmission() {
    Controller controller;
    const auto old = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, old.request_id);
    assert(!controller.ClaimStart(old.request_id).claimed);

    const auto invalidated = controller.InvalidateSession(2, 22, 202);
    assert(!invalidated.start_pending);
    const auto committed = controller.CommitStart(old.request_id, true);
    assert(committed.accepted);
    assert(committed.draining);

    const auto completion = controller.BeginCompletion(2, 22, 202);
    assert(completion.owned_callback);
    assert(completion.discard_results);
    assert(!completion.save_results);
    assert(!completion.send_list);
    controller.FinishCompletion(completion.request_id);
}

void SynchronousStartFailurePromotesPendingRequest() {
    Controller controller;
    const auto first = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, first.request_id);
    controller.InvalidateSession(2, 22, 202);
    const auto queued = controller.RequestScan(Request(2, 22, 202));
    assert(queued.queued);

    const auto failure = controller.CommitStart(first.request_id, false);
    assert(!failure.accepted);
    assert(!failure.send_failure);
    assert(failure.start_pending);
    assert(failure.pending_request_id != first.request_id);
    assert(failure.pending.setup_generation == 2);

    ClaimStart(controller, failure.pending_request_id);
    const auto current_failure =
        controller.CommitStart(failure.pending_request_id, false);
    assert(current_failure.send_failure);
    assert(!current_failure.start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void ConcurrentRequestsCoalesceToLatestPendingRequest() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    Barrier barrier(3);
    std::thread first([&]() {
        barrier.ArriveAndWait();
        assert(controller.RequestScan(Request(2, 22, 202)).queued);
    });
    std::thread second([&]() {
        barrier.ArriveAndWait();
        assert(controller.RequestScan(Request(3, 33, 303)).queued);
    });
    barrier.ArriveAndWait();
    first.join();
    second.join();

    const auto completion = controller.BeginCompletion(1, 11, 101);
    assert(completion.owned_callback);
    const auto next = controller.FinishCompletion(completion.request_id);
    assert(next.start_pending);
    assert(next.pending.setup_generation == 2 ||
           next.pending.setup_generation == 3);
    assert(controller.phase() == Controller::Phase::kStarting);
    assert(!controller.FinishCompletion(completion.request_id).start_pending);
}

void RunningAndDrainingCompletionsHaveDistinctDelivery() {
    Controller controller;
    const auto running = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, running.request_id);
    assert(controller.CommitStart(running.request_id, true).accepted);
    const auto current = controller.BeginCompletion(1, 11, 101);
    assert(current.owned_callback);
    assert(!current.discard_results);
    assert(current.save_results);
    assert(current.send_list);
    assert(!controller.FinishCompletion(current.request_id).start_pending);

    const auto draining = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, draining.request_id);
    assert(controller.CommitStart(draining.request_id, true).accepted);
    controller.InvalidateSession(2, 22, 202);
    const auto stale = controller.BeginCompletion(2, 22, 202);
    assert(stale.owned_callback);
    assert(stale.discard_results);
    assert(!stale.save_results);
    assert(!stale.send_list);
    assert(!controller.FinishCompletion(stale.request_id).start_pending);
}

void CallbackRacingCommitStartOwnsCompletionOnce() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        Controller controller;
        const auto request = controller.RequestScan(Request(1, 11, 101));
        ClaimStart(controller, request.request_id);
        Barrier barrier(3);
        Controller::StartDecision commit;
        Controller::CompletionDecision completion;
        std::thread starter([&]() {
            barrier.ArriveAndWait();
            commit = controller.CommitStart(request.request_id, true);
        });
        std::thread callback([&]() {
            barrier.ArriveAndWait();
            completion = controller.BeginCompletion(1, 11, 101);
        });
        barrier.ArriveAndWait();
        starter.join();
        callback.join();

        assert(commit.accepted);
        assert(!commit.draining);
        assert(completion.owned_callback);
        assert(!completion.discard_results);
        assert(!controller.BeginCompletion(1, 11, 101).owned_callback);
        assert(!controller.FinishCompletion(completion.request_id).start_pending);

        const auto late_commit =
            controller.CommitStart(request.request_id, true);
        assert(!late_commit.accepted);
        assert(controller.phase() == Controller::Phase::kIdle);
    }
}

void LostCallbackRecoveryAdvancesDriverBeforePendingStart() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    controller.InvalidateSession(2, 22, 202);
    assert(controller.RequestScan(Request(2, 22, 202)).queued);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    assert(!controller.BeginRecovery(owner.request_id).valid);

    const auto failed = controller.CompleteRecovery(ticket, false);
    assert(!failed.start_pending);
    assert(controller.phase() == Controller::Phase::kDraining);

    const auto retry = controller.BeginRecovery(owner.request_id);
    assert(retry.valid);
    assert(retry.driver_incarnation == ticket.driver_incarnation);
    const auto recovered = controller.CompleteRecovery(retry, true);
    assert(recovered.start_pending);
    assert(recovered.pending.setup_generation == 2);

    const auto next_ticket =
        controller.BeginRecovery(recovered.request_id);
    assert(!next_ticket.valid);  // A Starting request has no owed callback yet.
    assert(controller.driver_incarnation() == ticket.driver_incarnation + 1);
}

void RecoveryIncarnationWrapSkipsZero() {
    Controller controller(std::numeric_limits<uint32_t>::max());
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    assert(ticket.driver_incarnation == std::numeric_limits<uint32_t>::max());
    controller.CompleteRecovery(ticket, true);
    assert(controller.driver_incarnation() == 1);
}

void WrongRequestRecoveryIsRejected() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);
    assert(!controller.BeginRecovery(owner.request_id + 1).valid);

    const auto ticket = controller.BeginRecovery(owner.request_id);
    assert(ticket.valid);
    auto wrong_incarnation = ticket;
    ++wrong_incarnation.driver_incarnation;
    assert(!controller.CompleteRecovery(wrong_incarnation, true).start_pending);
    auto wrong = ticket;
    ++wrong.request_id;
    assert(!controller.CompleteRecovery(wrong, true).start_pending);
    assert(controller.phase() == Controller::Phase::kDraining);
    assert(!controller.BeginRecovery(owner.request_id).valid);
    assert(!controller.CompleteRecovery(ticket, true).start_pending);
    assert(controller.phase() == Controller::Phase::kIdle);
}

void CallbackBeforeInvalidatePreservesCurrentOwnerDelivery() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    Signal callback_claimed;
    Controller::CompletionDecision completion;
    std::thread callback([&]() {
        completion = controller.BeginCompletion(1, 11, 101);
        callback_claimed.Notify();
    });
    std::thread invalidator([&]() {
        callback_claimed.Wait();
        controller.InvalidateSession(2, 22, 202);
    });
    callback.join();
    invalidator.join();

    assert(completion.owned_callback);
    assert(!completion.discard_results);
    assert(completion.save_results);
    assert(completion.send_list);
    controller.FinishCompletion(completion.request_id);
}

void InvalidateBeforeCallbackDiscardsCurrentOwnerDelivery() {
    Controller controller;
    const auto owner = controller.RequestScan(Request(1, 11, 101));
    ClaimStart(controller, owner.request_id);
    assert(controller.CommitStart(owner.request_id, true).accepted);

    Signal invalidated;
    Controller::CompletionDecision completion;
    std::thread invalidator([&]() {
        controller.InvalidateSession(2, 22, 202);
        invalidated.Notify();
    });
    std::thread callback([&]() {
        invalidated.Wait();
        completion = controller.BeginCompletion(1, 11, 101);
    });
    invalidator.join();
    callback.join();

    assert(completion.owned_callback);
    assert(completion.discard_results);
    assert(!completion.save_results);
    assert(!completion.send_list);
    controller.FinishCompletion(completion.request_id);
}

void SimultaneousCallbackAndInvalidateHaveConsistentLinearization() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        Controller controller;
        const auto owner = controller.RequestScan(Request(1, 11, 101));
        ClaimStart(controller, owner.request_id);
        assert(controller.CommitStart(owner.request_id, true).accepted);

        Barrier barrier(3);
        Controller::CompletionDecision completion;
        std::thread callback([&]() {
            barrier.ArriveAndWait();
            completion = controller.BeginCompletion(1, 11, 101);
        });
        std::thread invalidator([&]() {
            barrier.ArriveAndWait();
            controller.InvalidateSession(2, 22, 202);
        });
        barrier.ArriveAndWait();
        callback.join();
        invalidator.join();

        assert(completion.owned_callback);
        const bool delivered = !completion.discard_results;
        assert(completion.save_results == delivered);
        assert(completion.send_list == delivered);
        controller.FinishCompletion(completion.request_id);
    }
}

}  // namespace

int main() {
    DelayedOldCompletionCannotSatisfyNewRequest();
    InvalidateBeforeClaimRetiresReservation();
    InvalidateBeforeClaimPromotesValidPendingReservation();
    InvalidateAfterClaimDrainsAcceptedSubmission();
    SynchronousStartFailurePromotesPendingRequest();
    ConcurrentRequestsCoalesceToLatestPendingRequest();
    RunningAndDrainingCompletionsHaveDistinctDelivery();
    CallbackRacingCommitStartOwnsCompletionOnce();
    LostCallbackRecoveryAdvancesDriverBeforePendingStart();
    RecoveryIncarnationWrapSkipsZero();
    WrongRequestRecoveryIsRejected();
    CallbackBeforeInvalidatePreservesCurrentOwnerDelivery();
    InvalidateBeforeCallbackDiscardsCurrentOwnerDelivery();
    SimultaneousCallbackAndInvalidateHaveConsistentLinearization();
    std::cout << "PASS: BluFi WiFi scan controller host model\n";
    return 0;
}
