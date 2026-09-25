#include "audio/codecs/es8311_data_adapter.h"
#include <cassert>
#include <future>
#include <chrono>

static audio_codec_data_if_t inner{};
static int tx_error = 0;
static bool partial = false;
static int calls = 0;
static bool blocked = false;
static std::promise<void> started, release_write;
static auto write_released = release_write.get_future();
static int driver_error = 0;
static bool blocked_read = false;
static std::promise<void> read_started, release_read;
static auto read_released = release_read.get_future();
static int direction(const audio_codec_data_if_t* h, esp_codec_dev_type_t t, bool) {
    assert(h == &inner);
    assert(t == ESP_CODEC_DEV_TYPE_IN_OUT);
    ++calls;
    return tx_error;
}
static int format(const audio_codec_data_if_t* h, esp_codec_dev_type_t t,
                  esp_codec_dev_sample_info_t*) { return direction(h, t, false); }
static int write_driver(void* h, const void*, size_t size, size_t* written, uint32_t timeout) {
    assert(h == &inner);
    assert(timeout == 1000);
    if (blocked) {
        started.set_value();
        write_released.wait();
    }
    *written = partial ? size - 1 : size;
    return driver_error;
}
int main() {
    std::atomic<uint32_t> eof{0};
    Es8311HardwareDrain drain(eof, 6);
    inner.enable = direction;
    inner.set_fmt = format;
    inner.open = [](const audio_codec_data_if_t* h, void*, int) { assert(h == &inner); return 0; };
    inner.is_open = [](const audio_codec_data_if_t* h) { assert(h == &inner); return true; };
    inner.close = [](const audio_codec_data_if_t* h) { assert(h == &inner); return 0; };
    inner.read = [](const audio_codec_data_if_t* h, uint8_t*, int) {
        assert(h == &inner);
        if (blocked_read) {
            read_started.set_value();
            read_released.wait();
        }
        return 0;
    };
    Es8311DataAdapter adapter(&inner, &inner, write_driver, drain);
    const audio_codec_data_if_t* api = &adapter;
    uint8_t bytes[4]{};
    assert(api->open(api, nullptr, 0) == 0);
    assert(api->is_open(api));
    assert(api->read(api, bytes, 4) == 0);
    blocked_read = true;
    auto reader = std::async(std::launch::async, [&] { return api->read(api, bytes, 4); });
    read_started.get_future().wait();
    auto duplex_writer = std::async(std::launch::async, [&] { return api->write(api, bytes, 4); });
    assert(duplex_writer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    assert(duplex_writer.get() == 0);
    release_read.set_value();
    assert(reader.get() == 0);
    blocked_read = false;
    assert(api->write(api, bytes, 4) == 0);
    eof.store(6);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    eof.store(7);
    assert(drain.Snapshot().state == AudioOutputDrainState::Drained);
    blocked = true;
    auto writer = std::async(std::launch::async, [&] { return api->write(api, bytes, 4); });
    started.get_future().wait();
    auto snapshot = std::async(std::launch::async, [&] { return drain.Snapshot(); });
    assert(snapshot.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    assert(snapshot.get().state == AudioOutputDrainState::Busy);
    assert(!drain.Reset());
    release_write.set_value();
    assert(writer.get() == 0);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    blocked = false;
    drain.Reset();
    driver_error = -2;
    assert(api->write(api, bytes, 4) == -2);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    driver_error = 0;
    drain.Reset();
    partial = true;
    assert(api->write(api, bytes, 4) != 0);
    partial = false;
    assert(api->write(api, bytes, 4) == 0);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    assert(drain.Reset());
    tx_error = -1;
    assert(api->enable(api, ESP_CODEC_DEV_TYPE_IN_OUT, true) == -1);
    assert(calls == 1);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    drain.Reset();
    esp_codec_dev_sample_info_t fs{};
    assert(api->set_fmt(api, ESP_CODEC_DEV_TYPE_IN_OUT, &fs) == -1);
    assert(drain.Snapshot().state == AudioOutputDrainState::Failed);
    drain.Reset();
    const auto epoch = drain.Snapshot().epoch;
    assert(api->close(api) == 0);
    assert(drain.Snapshot().epoch != epoch);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    assert(api->write(api, bytes, 4) == 0);
    eof.fetch_add(7);
    assert(drain.Snapshot().state == AudioOutputDrainState::Drained);
    tx_error = 0;
    assert(api->enable(api, ESP_CODEC_DEV_TYPE_IN_OUT, false) == 0);
    assert(drain.Snapshot().state == AudioOutputDrainState::Pending);
    int stage = 0;
    bool released = false;
    auto disable = [&] { assert(stage++ == 0); return true; };
    auto unregister = [&] { assert(stage++ == 1); return true; };
    auto remove = [&] { assert(stage++ == 2); return true; };
    auto release = [&] { assert(stage == 3); released = true; };
    assert(Es8311ReleaseIsrContext(disable, unregister, remove, release));
    assert(released);
    stage = 0;
    released = false;
    assert(!Es8311ReleaseIsrContext(disable, unregister, [] { return false; }, release));
    assert(!released);
    stage = 0;
    assert(!Es8311ReleaseIsrContext(disable, [] { return false; }, [] { return true; }, release));
    assert(!released);
}
