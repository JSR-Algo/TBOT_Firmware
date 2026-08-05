#include "passive_websocket_liveness.h"
#include "connection_inbound_gate.h"
#include "passive_reconnect_policy.h"
#include "connection_close_state.h"
#include "protocol_lifetime_token.h"
#include "connect_close_deferral.h"
#include "esp_tcp_shutdown_state.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

bool Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "passive websocket liveness host test failed: %s\n", message);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    PassiveWebsocketLiveness liveness;
    liveness.OnOpened(1000);

    if (!Require(liveness.Poll(2999) == PassiveWebsocketLiveness::Action::kNone,
                 "probe must not fire before the interval") ||
        !Require(liveness.Poll(3000) == PassiveWebsocketLiveness::Action::kSendPing,
                 "probe must fire at the interval") ||
        !Require(liveness.Poll(12999) == PassiveWebsocketLiveness::Action::kNone,
                 "loaded-device pong wait must not fire before the grace deadline") ||
        !Require(liveness.Poll(13000) == PassiveWebsocketLiveness::Action::kTimedOut,
                 "missing pong must time out at the deadline")) {
        return 1;
    }

    liveness.OnOpened(10000);
    if (!Require(liveness.Poll(12000) == PassiveWebsocketLiveness::Action::kSendPing,
                 "reopened channel must send a fresh probe")) {
        return 1;
    }
    liveness.OnPong(12025);
    if (!Require(liveness.Poll(15999) == PassiveWebsocketLiveness::Action::kSendPing,
                 "pong must clear the outstanding probe and preserve periodic probing")) {
        return 1;
    }
    liveness.OnPong(16010);
    if (!Require(liveness.Poll(16011) == PassiveWebsocketLiveness::Action::kNone,
                 "fresh pong must keep the channel healthy")) {
        return 1;
    }

    ConnectionInboundGate inbound_gate;
    const uint32_t first_epoch = inbound_gate.BeginConnection();
    std::atomic<bool> lease_entered{false};
    std::atomic<bool> release_lease{false};
    std::atomic<bool> failure_started{false};
    std::atomic<bool> failure_completed{false};
    std::thread inbound([&]() {
        auto lease = inbound_gate.Acquire(first_epoch);
        if (!lease) {
            return;
        }
        lease_entered.store(true);
        while (!release_lease.load()) {
            std::this_thread::yield();
        }
    });
    while (!lease_entered.load()) {
        std::this_thread::yield();
    }
    std::thread failure([&]() {
        failure_started.store(true);
        inbound_gate.FailCurrent();
        failure_completed.store(true);
    });
    while (!failure_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!Require(!failure_completed.load(),
                 "failure must serialize behind an in-flight inbound dispatch")) {
        release_lease.store(true);
        inbound.join();
        failure.join();
        return 1;
    }
    release_lease.store(true);
    inbound.join();
    failure.join();
    if (!Require(failure_completed.load(), "failure must complete after the lease exits") ||
        !Require(!inbound_gate.Acquire(first_epoch),
                 "no inbound dispatch may start after failure linearizes")) {
        return 1;
    }
    const uint32_t second_epoch = inbound_gate.BeginConnection();
    bool old_epoch_allowed = false;
    {
        auto old_lease = inbound_gate.Acquire(first_epoch);
        old_epoch_allowed = static_cast<bool>(old_lease);
    }
    bool new_epoch_allowed = false;
    {
        auto new_lease = inbound_gate.Acquire(second_epoch);
        new_epoch_allowed = static_cast<bool>(new_lease);
    }
    if (!Require(!old_epoch_allowed,
                 "old connection callbacks must stay rejected after replacement") ||
        !Require(new_epoch_allowed,
                 "replacement connection must accept its own inbound callbacks")) {
        return 1;
    }

    const uint32_t reentrant_epoch = inbound_gate.BeginConnection();
    {
        auto callback_lease = inbound_gate.Acquire(reentrant_epoch);
        if (!Require(static_cast<bool>(callback_lease),
                     "callback must acquire a healthy connection lease")) {
            return 1;
        }
        inbound_gate.FailCurrent();
    }
    if (!Require(!inbound_gate.Acquire(reentrant_epoch),
                 "reentrant callback failure must close the gate without deadlock")) {
        return 1;
    }

    const uint32_t lifetime_epoch = inbound_gate.BeginConnection();
    std::atomic<bool> stale_callback_entered{false};
    std::atomic<bool> release_stale_callback{false};
    std::atomic<bool> replacement_started{false};
    std::atomic<bool> replacement_completed{false};
    std::thread stale_callback([&]() {
        auto stale_lease = inbound_gate.Acquire(lifetime_epoch);
        stale_callback_entered.store(true);
        while (!release_stale_callback.load()) {
            std::this_thread::yield();
        }
    });
    while (!stale_callback_entered.load()) {
        std::this_thread::yield();
    }
    std::thread replacement([&]() {
        replacement_started.store(true);
        auto mutation = inbound_gate.BeginConnectionMutation();
        (void)mutation.epoch();
        replacement_completed.store(true);
    });
    while (!replacement_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!Require(!replacement_completed.load(),
                 "replacement must wait until stale callback releases its lifetime lease")) {
        release_stale_callback.store(true);
        stale_callback.join();
        replacement.join();
        return 1;
    }
    release_stale_callback.store(true);
    stale_callback.join();
    replacement.join();
    if (!Require(replacement_completed.load(),
                 "replacement must complete after stale callback exits")) {
        return 1;
    }

    const uint32_t close_epoch = inbound_gate.BeginConnection();
    std::atomic<bool> close_callback_entered{false};
    std::atomic<bool> release_close_callback{false};
    std::atomic<bool> close_started{false};
    std::atomic<bool> close_completed{false};
    std::thread close_callback([&]() {
        auto callback_lease = inbound_gate.Acquire(close_epoch);
        close_callback_entered.store(true);
        while (!release_close_callback.load()) {
            std::this_thread::yield();
        }
    });
    while (!close_callback_entered.load()) {
        std::this_thread::yield();
    }
    std::thread close([&]() {
        close_started.store(true);
        auto mutation = inbound_gate.BeginFailureMutation();
        (void)mutation.epoch();
        close_completed.store(true);
    });
    while (!close_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!Require(!close_completed.load(),
                 "close mutation must wait for an in-flight callback lease")) {
        release_close_callback.store(true);
        close_callback.join();
        close.join();
        return 1;
    }
    release_close_callback.store(true);
    close_callback.join();
    close.join();
    if (!Require(close_completed.load(),
                 "close mutation must complete after the callback exits")) {
        return 1;
    }

    const uint32_t unhealthy_current_epoch = inbound_gate.BeginConnection();
    inbound_gate.FailCurrent();
    {
        auto unhealthy_current = inbound_gate.Acquire(unhealthy_current_epoch);
        if (!Require(!unhealthy_current,
                     "failed current connection must reject inbound dispatch") ||
            !Require(unhealthy_current.IsCurrentEpoch(),
                     "failed current disconnect remains distinct from stale epoch")) {
            return 1;
        }
    }

    const uint32_t callback_close_epoch = inbound_gate.BeginConnection();
    {
        auto callback_lease = inbound_gate.Acquire(callback_close_epoch);
        if (!Require(inbound_gate.CurrentThreadHasLease(),
                     "reentrant close must detect callback context")) {
            return 1;
        }
    }
    if (!Require(!inbound_gate.CurrentThreadHasLease(),
                 "callback context must clear when its lease exits")) {
        return 1;
    }

    ConnectionCloseState close_state;
    close_state.ResetForConnection();
    const uint32_t close_state_epoch = 41;
    if (!Require(close_state.MarkDeferred(close_state_epoch),
                 "first reentrant close must schedule deferred teardown") ||
        !Require(!close_state.MarkDeferred(close_state_epoch),
                 "duplicate reentrant close must not schedule twice") ||
        !Require(close_state.TakeDeferred(close_state_epoch),
                 "scheduled teardown must consume its deferred ownership") ||
        !Require(!close_state.TakeDeferred(close_state_epoch),
                 "deferred teardown ownership must be exactly once") ||
        !Require(close_state.TakeNotification(),
                 "explicit close must emit one close notification") ||
        !Require(!close_state.TakeNotification(),
                 "explicit close must not emit duplicate notification")) {
        return 1;
    }
    close_state.ResetForConnection();
    if (!Require(!close_state.TakeDeferred(close_state_epoch),
                 "replacement connection must invalidate stale deferred teardown") ||
        !Require(close_state.TakeNotification(),
                 "new connection must re-arm exactly-once close notification")) {
        return 1;
    }

    ConnectionCloseState interleaving_close_state;
    const uint32_t queued_epoch = inbound_gate.BeginConnection();
    interleaving_close_state.ResetForConnection();
    if (!Require(interleaving_close_state.MarkDeferred(queued_epoch),
                 "queued close must own its source connection epoch")) {
        return 1;
    }
    const uint32_t replacement_epoch = inbound_gate.BeginConnection();
    interleaving_close_state.ResetForConnection();
    {
        auto stale_close = inbound_gate.BeginFailureMutationIfCurrent(queued_epoch);
        if (!Require(!stale_close.Matched(),
                     "queued close must not mutate a replacement epoch")) {
            return 1;
        }
    }
    if (!Require(static_cast<bool>(inbound_gate.Acquire(replacement_epoch)),
                 "replacement must remain healthy after stale queued close") ||
        !Require(!interleaving_close_state.TakeDeferred(queued_epoch),
                 "replacement reset must invalidate old deferred ownership")) {
        return 1;
    }

    const uint32_t winning_close_epoch = inbound_gate.BeginConnection();
    interleaving_close_state.ResetForConnection();
    if (!Require(interleaving_close_state.MarkDeferred(winning_close_epoch),
                 "current queued close must record its epoch")) {
        return 1;
    }
    {
        auto current_close = inbound_gate.BeginFailureMutationIfCurrent(winning_close_epoch);
        if (!Require(current_close.Matched(),
                     "current queued close must win conditional failure mutation") ||
            !Require(interleaving_close_state.TakeDeferred(winning_close_epoch),
                     "winning close must consume exactly its own epoch")) {
            return 1;
        }
    }

    ConnectionInboundGate notification_gate;
    ConnectionCloseState notification_state;
    const uint32_t notification_epoch = notification_gate.BeginConnection();
    notification_state.ResetForConnection();
    std::atomic<bool> old_teardown_before_notify{false};
    std::atomic<bool> allow_old_notify{false};
    std::atomic<bool> replacement_notify_started{false};
    std::atomic<bool> replacement_notify_completed{false};
    std::atomic<int> old_notification_count{0};
    std::thread old_teardown([&]() {
        auto failure_mutation =
            notification_gate.BeginFailureMutationIfCurrent(notification_epoch);
        if (!failure_mutation.Matched()) {
            return;
        }
        old_teardown_before_notify.store(true);
        while (!allow_old_notify.load()) {
            std::this_thread::yield();
        }
        if (notification_state.TakeNotification()) {
            old_notification_count.fetch_add(1);
        }
        // Reentrant close/cleanup must not deadlock under the same mutation.
        auto reentrant_failure = notification_gate.BeginFailureMutation();
        (void)reentrant_failure.epoch();
    });
    while (!old_teardown_before_notify.load()) {
        std::this_thread::yield();
    }
    std::thread notification_replacement([&]() {
        replacement_notify_started.store(true);
        auto replacement_mutation = notification_gate.BeginConnectionMutation();
        notification_state.ResetForConnection();
        replacement_notify_completed.store(true);
    });
    while (!replacement_notify_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!Require(!replacement_notify_completed.load(),
                 "replacement must wait while old teardown is paused before notify")) {
        allow_old_notify.store(true);
        old_teardown.join();
        notification_replacement.join();
        return 1;
    }
    allow_old_notify.store(true);
    old_teardown.join();
    notification_replacement.join();
    if (!Require(old_notification_count.load() == 1,
                 "old teardown must notify exactly once before replacement") ||
        !Require(replacement_notify_completed.load(),
                 "replacement must complete after old notification exits") ||
        !Require(notification_state.TakeNotification(),
                 "old teardown must not consume replacement notification state")) {
        return 1;
    }

    int same_address_protocol = 0;
    const uint64_t old_generation = 7;
    const uint64_t new_generation = 8;
    if (!Require(!ProtocolLifetimeMatches(
                     &same_address_protocol, &same_address_protocol,
                     new_generation, old_generation),
                 "same-address replacement must reject stale queued protocol work") ||
        !Require(ProtocolLifetimeMatches(
                     &same_address_protocol, &same_address_protocol,
                     new_generation, new_generation),
                 "current pointer and generation must accept queued protocol work")) {
        return 1;
    }

    ConnectCloseDeferral connect_close;
    int connect_worker_close_count = 0;
    if (!Require(!connect_close.Request(true),
                 "network drop must not close while connect worker owns socket") ||
        !Require(connect_close.TakeAfterWorker(),
                 "worker exit must observe deferred close ownership")) {
        return 1;
    }

    ConnectCloseDeferral publication_close;
    bool online_intent = false;
    int heartbeat_count = 0;
    int publication_close_count = 0;
    int reconnect_count = 0;
    if (!Require(!publication_close.Request(true),
                 "close during handshake must defer transport teardown") ||
        !Require(publication_close.Pending(),
                 "deferred close must suppress handshake success publication")) {
        return 1;
    }

    EspTcpShutdownState tcp_shutdown;
    tcp_shutdown.TaskStarted();
    if (!Require(!tcp_shutdown.CanDeleteSynchronization(),
                 "live receive task must retain synchronization ownership")) {
        return 1;
    }
    if (!Require(tcp_shutdown.TaskWillExit(),
                 "receive task must claim cooperative exit ownership")) {
        return 1;
    }
    tcp_shutdown.TaskExited();
    if (!Require(!tcp_shutdown.CanDeleteSynchronization(),
                 "exited receive task remains unsafe until its signal is joined")) {
        return 1;
    }
    tcp_shutdown.TaskJoined();
    if (!Require(tcp_shutdown.CanDeleteSynchronization(),
                 "joined receive task must release synchronization ownership")) {
        return 1;
    }
    EspTcpShutdownState callback_shutdown;
    callback_shutdown.TaskStarted();
    std::atomic<bool> callback_entered{false};
    std::atomic<bool> release_callback{false};
    std::atomic<bool> callback_completed{false};
    std::atomic<bool> exit_signaled{false};
    std::atomic<bool> waiter_destroyed{false};
    std::thread receive_exit([&]() {
        callback_entered.store(true);
        while (!release_callback.load()) {
            std::this_thread::yield();
        }
        callback_completed.store(true);
        callback_shutdown.TaskWillExit();
        callback_shutdown.TaskExited();
        exit_signaled.store(true);
    });
    while (!callback_entered.load()) {
        std::this_thread::yield();
    }
    std::thread exit_waiter([&]() {
        while (!exit_signaled.load()) {
            std::this_thread::yield();
        }
        callback_shutdown.TaskJoined();
        waiter_destroyed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!Require(!waiter_destroyed.load(),
                 "timeout must not force-delete a task while its callback is running") ||
        !Require(callback_shutdown.NeedsJoin(),
                 "running callback must retain task and synchronization ownership")) {
        release_callback.store(true);
        receive_exit.join();
        exit_waiter.join();
        return 1;
    }
    release_callback.store(true);
    receive_exit.join();
    exit_waiter.join();
    if (!Require(callback_completed.load() && waiter_destroyed.load(),
                 "waiter may destroy only after callback completion and exit signal")) {
        return 1;
    }

    EspSslShutdownState wss_shutdown;
    wss_shutdown.TaskStarted();
    std::mutex wss_callback_mutex;
    std::atomic<bool> wss_callback_entered{false};
    std::atomic<bool> release_wss_callback{false};
    std::atomic<bool> wss_exit_signaled{false};
    std::thread wss_receive_exit([&]() {
        std::lock_guard<std::mutex> callback_guard(wss_callback_mutex);
        wss_callback_entered.store(true);
        while (!release_wss_callback.load()) {
            std::this_thread::yield();
        }
        wss_shutdown.TaskWillExit();
        wss_shutdown.TaskExited();
        wss_exit_signaled.store(true);
    });
    while (!wss_callback_entered.load()) {
        std::this_thread::yield();
    }
    if (!Require(wss_shutdown.NeedsJoin() &&
                     !wss_shutdown.CanDeleteSynchronization(),
                 "WSS timeout must fail fast instead of deleting a TLS task in callback/lock scope")) {
        release_wss_callback.store(true);
        wss_receive_exit.join();
        return 1;
    }
    release_wss_callback.store(true);
    wss_receive_exit.join();
    if (!Require(wss_exit_signaled.load(),
                 "WSS callback must complete before TLS exit publication")) {
        return 1;
    }
    wss_shutdown.TaskJoined();
    if (!Require(wss_shutdown.CanDeleteSynchronization(),
                 "WSS TLS synchronization is deletable only after joined exit")) {
        return 1;
    }

    std::mutex transport_send_mutex;
    std::atomic<int> owned_fd{41};
    std::atomic<bool> stop_transport{false};
    std::atomic<bool> sender_loaded_fd{false};
    std::atomic<bool> release_sender{false};
    std::atomic<int> sender_observed_fd{-1};
    std::thread in_flight_sender([&]() {
        std::lock_guard<std::mutex> send_guard(transport_send_mutex);
        if (!stop_transport.load()) {
            sender_observed_fd.store(owned_fd.load());
            sender_loaded_fd.store(true);
            while (!release_sender.load()) {
                std::this_thread::yield();
            }
        }
    });
    while (!sender_loaded_fd.load()) {
        std::this_thread::yield();
    }
    stop_transport.store(true);
    if (!Require(owned_fd.load() == 41,
                 "shutdown must retain fd ownership until receive join and send drain")) {
        release_sender.store(true);
        in_flight_sender.join();
        return 1;
    }
    release_sender.store(true);
    in_flight_sender.join();
    {
        std::lock_guard<std::mutex> send_guard(transport_send_mutex);
        owned_fd.store(-1);
    }
    owned_fd.store(41);  // Simulate descriptor reuse only after the old sender drained.
    if (!Require(sender_observed_fd.load() == 41 && owned_fd.load() == 41,
                 "old send must finish before a reused descriptor is published")) {
        return 1;
    }
    if (!publication_close.Pending()) {
        online_intent = true;
        ++heartbeat_count;
    }
    if (publication_close.TakeAfterWorker()) {
        ++publication_close_count;
    }
    if (online_intent) {
        ++reconnect_count;
    }
    if (!Require(!online_intent,
                 "cancelled handshake must leave online intent false") ||
        !Require(heartbeat_count == 0,
                 "cancelled handshake must publish zero heartbeats") ||
        !Require(publication_close_count == 1,
                 "cancelled handshake must close exactly once") ||
        !Require(reconnect_count == 0,
                 "cancelled handshake close must not schedule reconnect")) {
        return 1;
    }
    ++connect_worker_close_count;
    if (!Require(!connect_close.TakeAfterWorker(),
                 "deferred close must drain exactly once") ||
        !Require(connect_worker_close_count == 1,
                 "worker cleanup must close transport exactly once") ||
        !Require(connect_close.Request(false),
                 "idle transport may close synchronously")) {
        return 1;
    }

    if (!Require(PassiveReconnectHasOwner(true, false),
                 "armed reconnect timer owns reconnect") ||
        !Require(PassiveReconnectHasOwner(false, true),
                 "replacement worker owns reconnect after pending exchange") ||
        !Require(!PassiveReconnectHasOwner(false, false),
                 "disconnect may schedule only when no timer or worker owns reconnect")) {
        return 1;
    }

    std::puts("passive websocket liveness host test passed");
    return 0;
}
