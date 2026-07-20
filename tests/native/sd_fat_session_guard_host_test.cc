#include "sd_fat_session_guard.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

int main() {
    tbot::SdFatSessionGuard guard;
    auto storage = guard.Acquire();
    const auto generation = guard.RecordMounted(storage);
    assert(generation == 1);
    auto mounted = guard.Snapshot(storage);
    assert(mounted.present);
    assert(mounted.mount_generation == generation);
    bool card_present = true;
    assert(guard.SetPresenceProbe(storage, [&]() { return card_present; }));
    std::uint32_t volume_serial = 7;
    assert(guard.SetVolumeProbe(storage, [&]() {
        return tbot::SdFatVolumeMetadata{volume_serial, "TBOT"};
    }));
    assert(guard.Snapshot(storage).present);
    assert(guard.Snapshot(storage).volume->serial == 7);
    assert(!guard.TryAcquire());

    std::atomic<bool> attempted{false};
    std::atomic<bool> inspected{false};
    std::thread inspector([&]() {
        attempted = true;
        auto inspection = guard.Acquire();
        const auto snapshot = guard.Snapshot(inspection);
        assert(snapshot.present);
        inspected = true;
    });
    while (!attempted) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!inspected);
    storage = {};
    inspector.join();
    assert(inspected);

    auto cross_thread = guard.Acquire();
    std::thread force_end([lease = std::move(cross_thread)]() mutable {
        lease = {};
    });
    force_end.join();
    auto after_force_end = guard.TryAcquire();
    assert(after_force_end);
    after_force_end = {};

    auto unmount = guard.Acquire();
    volume_serial = 8;
    assert(guard.Snapshot(unmount).volume->serial == 8);
    card_present = false;
    assert(!guard.Snapshot(unmount).present);
    guard.RecordUnmounted(unmount);
    const auto absent = guard.Snapshot(unmount);
    assert(!absent.present);
    assert(absent.mount_generation == generation);
    const auto remount_generation = guard.RecordMounted(unmount);
    assert(remount_generation == generation + 1);
    int stale_presence_calls = 0;
    int stale_volume_calls = 0;
    assert(guard.SetPresenceProbe(unmount, [&]() {
        ++stale_presence_calls;
        return true;
    }));
    assert(guard.SetVolumeProbe(unmount, [&]() {
        ++stale_volume_calls;
        return tbot::SdFatVolumeMetadata{9, "STALE"};
    }));
    assert(guard.RecordMounted(unmount) == remount_generation + 1);
    const auto failed_remount = guard.Snapshot(unmount);
    assert(failed_remount.present);
    assert(!failed_remount.volume.has_value());
    assert(stale_presence_calls == 0);
    assert(stale_volume_calls == 0);
    return 0;
}
