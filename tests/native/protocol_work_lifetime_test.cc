#include "protocol_work_lifetime.h"
#include <cassert>

int main() {
    ProtocolWorkLifetime lifetime;
    auto queued = lifetime.Reserve();
    assert(lifetime.Busy() && !lifetime.BusyExcept(queued));
    assert(lifetime.BusyExcept(0));
    auto running = lifetime.Reserve();
    assert(lifetime.BusyExcept(queued) && lifetime.BusyExcept(running));
    assert(queued && running);
    lifetime.Request(ProtocolWorkLifetime::Action::kClose);
    assert(!lifetime.Reserve());
    assert(lifetime.TakeReady() == ProtocolWorkLifetime::Action::kNone);
    lifetime.Request(ProtocolWorkLifetime::Action::kReset);
    lifetime.Request(ProtocolWorkLifetime::Action::kReinitialize);
    assert(lifetime.Release(queued));
    assert(!lifetime.Release(queued));
    assert(lifetime.TakeReady() == ProtocolWorkLifetime::Action::kNone);
    lifetime.Request(ProtocolWorkLifetime::Action::kClose);
    assert(lifetime.Release(running));
    assert(lifetime.TakeReady() == ProtocolWorkLifetime::Action::kReinitialize);
    auto rejected = lifetime.Reserve();
    assert(lifetime.Release(rejected));
    assert(!lifetime.Busy());
    auto last = lifetime.Reserve();
    lifetime.Request(ProtocolWorkLifetime::Action::kReboot);
    lifetime.Request(ProtocolWorkLifetime::Action::kReset);
    assert(lifetime.TakeReady() == ProtocolWorkLifetime::Action::kNone);
    assert(lifetime.Release(last));
    assert(lifetime.TakeReady() == ProtocolWorkLifetime::Action::kReboot);
    assert(lifetime.TakeReady() == ProtocolWorkLifetime::Action::kNone);
}
