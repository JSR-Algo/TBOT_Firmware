#include "protocols/connection_inbound_gate.h"
#include <cassert>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;
using Gate = ConnectionInboundGate;

int main() {
    Gate gate;
    assert(gate.HealthyEpoch() == 0);
    assert(!gate.TryAcquire(0));
    const auto old = gate.BeginConnection();
    assert(gate.HealthyEpoch() == old);
    {
        auto outer = gate.Acquire(old);
        assert(outer && gate.CurrentThreadHasLease());
        {
            auto inner = gate.TryAcquire(old);
            assert(inner && inner.status() == Gate::LeaseStatus::Allowed);
            auto moved = std::move(inner);
            assert(moved);
        }
        assert(gate.CurrentThreadHasLease());
    }
    assert(!gate.CurrentThreadHasLease());
    std::promise<void> held, release;
    auto released = release.get_future();
    std::thread holder([&] {
        auto lease = gate.Acquire(old);
        held.set_value();
        released.wait();
    });
    held.get_future().wait();
    auto probe = std::async(std::launch::async, [&] {
        auto lease = gate.TryAcquire(old);
        assert(!lease && lease.status() == Gate::LeaseStatus::Busy);
        assert(!gate.CurrentThreadHasLease());
        assert(gate.HealthyEpoch() == old);
    });
    assert(probe.wait_for(1s) == std::future_status::ready);
    probe.get();
    std::promise<void> reconnect_entered;
    auto reconnect = std::async(std::launch::async, [&] {
        reconnect_entered.set_value();
        return gate.BeginConnection();
    });
    reconnect_entered.get_future().wait();
    assert(reconnect.wait_for(30ms) == std::future_status::timeout);
    release.set_value();
    holder.join();
    const auto current = reconnect.get();
    assert(current != old && gate.HealthyEpoch() == current);
    {
        auto stale = gate.TryAcquire(old);
        assert(!stale && stale.status() == Gate::LeaseStatus::Stale);
    }
    assert(!gate.BeginFailureMutationIfCurrent(old).Matched());
    assert(gate.HealthyEpoch() == current);
    gate.FailCurrent();
    assert(gate.HealthyEpoch() == 0 && gate.CurrentEpoch() == current);
    assert(!gate.TryAcquire(current));
    {
        const auto live = gate.BeginConnection();
        auto lease = gate.Acquire(live);
        gate.FailCurrent();
        auto read = std::async(std::launch::async, [&] { return gate.HealthyEpoch(); });
        assert(read.wait_for(1s) == std::future_status::ready);
        assert(read.get() == 0);
    }
    gate.BeginConnection();
    {
        auto failure = gate.BeginFailureMutation();
        auto read = std::async(std::launch::async, [&] { return gate.HealthyEpoch(); });
        assert(read.wait_for(1s) == std::future_status::ready);
        assert(read.get() == 0);
    }
    const auto next = gate.BeginConnection();
    assert(gate.BeginFailureMutationIfCurrent(next).Matched());
    assert(gate.HealthyEpoch() == 0 && !gate.TryAcquire(next));
}
