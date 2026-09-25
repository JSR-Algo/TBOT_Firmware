#include "audio/codecs/es8311_hardware_drain.h"
#include <cassert>
#include <chrono>
#include <future>

int main() {
    std::atomic<uint32_t> eof{0};
    Es8311HardwareDrain drain(eof, 6);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    drain.BeginWrite();
    drain.BeginWrite();
    drain.FinishWrite(true);
    eof.fetch_add(7);
    assert(drain.Snapshot().state == AudioOutputDrainState::Busy);
    drain.FinishWrite(true);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    assert(drain.Reset());
    eof.store(0);
    drain.BeginWrite();
    assert(drain.Snapshot().state == AudioOutputDrainState::Busy);
    assert(!drain.Reset());
    drain.FinishWrite(true);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    assert(drain.Reset());
    drain.BeginWrite();
    drain.FinishWrite(true);
    eof.store(6);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    eof.store(7);
    assert(drain.Snapshot().state == AudioOutputDrainState::Drained);
    drain.BeginWrite();
    drain.FinishWrite(true);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    eof.store(14);
    assert(drain.Snapshot().state == AudioOutputDrainState::Drained);
    drain.BeginWrite();
    drain.FinishWrite(false);
    drain.BeginWrite();
    drain.FinishWrite(true);
    eof.store(100);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    const auto epoch = drain.Snapshot().epoch;
    drain.Invalidate();
    assert(drain.Snapshot().epoch != epoch);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    assert(drain.Reset());
    eof.store(UINT32_MAX - 3);
    drain.BeginWrite();
    drain.FinishWrite(true);
    eof.store(2);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    eof.store(3);
    assert(drain.Snapshot().state == AudioOutputDrainState::Drained);
    drain.Invalidate();
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    std::promise<void> started, release;
    auto released = release.get_future();
    auto writer = std::async(std::launch::async, [&] {
        drain.BeginWrite();
        started.set_value();
        released.wait();
        drain.FinishWrite(true);
    });
    started.get_future().wait();
    auto snapshot = std::async(std::launch::async, [&] { return drain.Snapshot(); });
    assert(snapshot.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    assert(snapshot.get().state == AudioOutputDrainState::Busy);
    release.set_value();
    writer.get();
    // Reset cannot certify residual DMA as drained without a new submission.
    assert(drain.Reset());
    eof.fetch_add(100);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
}
