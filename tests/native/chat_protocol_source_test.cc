#include <atomic>
#include <mutex>
#define private public
#include "chat_protocol_signals.h"
#include "protocols/connection_source.h"
#undef private
#include <cassert>
#include <future>
int main() {
    ConnectionSourceSequence seq;
    const auto a=seq.Next(1), b=seq.Next(3);
    assert(a.source_id!=b.source_id && a.connection_epoch==1);
    seq.next_=UINT32_MAX-1;
    assert(!seq.Next(9).Valid() && !seq.Next(10).Valid());
    ChatProtocolSignals signals;
    assert(signals.EnableForSource(a));
    auto era=signals.Capture();
    assert(signals.PublishSource(era,a,ChatProtocolSignals::Error));
    uint32_t flags=77;
    std::promise<void> held, release;
    auto released=release.get_future().share();
    auto holder=std::async(std::launch::async,[&]{std::lock_guard<std::mutex> lock(signals.mutex_);held.set_value();released.wait();});
    held.get_future().wait();
    assert(!signals.Collect(flags) && flags==77);
    release.set_value(); holder.get();
    assert(signals.Collect(flags) && flags==ChatProtocolSignals::Error);
    assert(!signals.Publish(era,ChatProtocolSignals::Closed));
    std::unique_lock<std::mutex> held_for_publish(signals.mutex_);
    std::promise<void> entered;
    auto delayed=std::async(std::launch::async,[&]{entered.set_value();return signals.PublishSource(era,a,ChatProtocolSignals::Closed);});
    entered.get_future().wait();
    signals.Disable();
    assert(signals.EnableForSource(b));
    held_for_publish.unlock();
    assert(!delayed.get());
    assert(signals.Collect(flags) && flags==0);
    assert(!signals.PublishSource(signals.Capture(),a,ChatProtocolSignals::Closed));
    assert(signals.PublishSource(signals.Capture(),b,ChatProtocolSignals::Closed));
    assert(signals.Collect(flags) && flags==ChatProtocolSignals::Closed);
    signals.Disable(); assert(!signals.MatchesSource(b));
    assert(!signals.EnableForSource(b));
    signals.Enable(); assert(!signals.Publish(signals.Capture(),ChatProtocolSignals::Error));
    assert(!signals.EnableForSource({UINT32_MAX,4}));
}
