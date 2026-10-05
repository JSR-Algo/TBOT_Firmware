// Renderer-v6 scene player over a fake media provider and the real Farm scene:
// frame selection follows the browser seek rule, prepare is accepted only after
// frame zero was presented, refusals never consume a sequence, unchanged frames
// are not repainted, rewinds reopen, decode errors surface and recover, and
// terminal commands release every stream.
#include "checked_cjson.h"
#include "lesson_original_source_player.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace tbot;

namespace {
int failures = 0, checks = 0;
void Expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition && failures++ < 20) std::fprintf(stderr, "FAIL %s\n", message.c_str());
}
std::string Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
const cJSON* Get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

// Every original decodes to solid 16x16 RGBA frames every 40 ms (pts in ms) for
// 2 s; frame n of an original has colour (n, 255 - n, salt).
struct FakeMedia final : OriginalSourceMediaProvider {
    int live = 0, opened = 0;
    std::string fail_open, fail_decode_after_id;
    int fail_decode_after = -1;
    std::map<std::string, int> salt;

    struct Stream final : OriginalSourceStream {
        FakeMedia* owner;
        std::string id;
        int next = 0;
        std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(16 * 16 * 4);
        Stream(FakeMedia* media, std::string asset) : owner(media), id(std::move(asset)) { ++owner->live; }
        ~Stream() override { --owner->live; }
        OriginalSourceStatus Next(OriginalSourceFrame* frame) override {
            if (id == owner->fail_decode_after_id && next == owner->fail_decode_after) {
                owner->fail_decode_after = -1;
                return OriginalSourceStatus::kDecode;
            }
            // Teaching objects are single-frame images, like the pinned PNGs.
            if (next >= (id.rfind("30000000-", 0) == 0 ? 1 : 50)) return OriginalSourceStatus::kEnd;
            for (std::size_t index = 0; index < pixels.size(); index += 4) {
                pixels[index] = static_cast<std::uint8_t>(next * 5);
                pixels[index + 1] = static_cast<std::uint8_t>(255 - next * 5);
                pixels[index + 2] = static_cast<std::uint8_t>(owner->salt[id]);
                pixels[index + 3] = 255;
            }
            *frame = OriginalSourceFrame{};
            frame->planes[0] = pixels.data();
            frame->strides[0] = 64;
            frame->width = frame->height = 16;
            frame->rgba = true;
            frame->pts = next * 40;
            frame->time_base_num = 1;
            frame->time_base_den = 1000;
            ++next;
            return OriginalSourceStatus::kOk;
        }
    };
    std::string last_cache_key;
    OriginalSourceStatus Open(const std::string& cache_key, const OriginalSourceOriginal& original,
                              std::unique_ptr<OriginalSourceStream>* out) override {
        last_cache_key = cache_key;
        if (original.asset_version_id == fail_open) return OriginalSourceStatus::kIntegrity;
        ++opened;
        out->reset(new Stream(this, original.asset_version_id));
        return OriginalSourceStatus::kOk;
    }
};

std::uint16_t Rgb565(int r, int g, int b) {
    return static_cast<std::uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;  // original-source-scene vectors
    CheckedCJsonPtr scenes(cJSON_Parse(Read(argv[1]).c_str()));
    if (!scenes) return 3;
    const cJSON* farm = Get(Get(scenes.get(), "scenes"), "valid")->child;
    const std::string scene_json = Get(farm, "canonicalJson")->valuestring;
    const std::string sha = Get(farm, "canonicalSha256")->valuestring;
    const auto loader = [&](const std::string&, const std::string& digest, std::uint32_t, std::string* json) -> const char* {
        if (digest != sha) return "unknown scene";
        *json = scene_json;
        return nullptr;
    };
    // Control bodies exactly as original-source-scene.v1 shapes them.
    const auto control = [&](const char* command, const char* cue, int sequence) {
        const std::string name = command;
        std::string phase = std::string("\"command\":\"") + command + "\",\"cueId\":\"" + cue +
                            "\",\"commandSequenceId\":" + std::to_string(sequence);
        if (name == "pause") return CheckedCJsonPtr(cJSON_Parse(("{" + phase + "}").c_str()));
        if (name == "prepare") {
            phase += ",\"scene\":{\"cacheKey\":\"farm-original/v1-" + std::string(64, 'a') + "\",\"sha256\":\"" + sha +
                     "\",\"bytes\":" + std::to_string(scene_json.size()) + "}";
        }
        const std::string reason = name == "stop" ? "\"reason\":\"COMPLETED\"," : "";
        return CheckedCJsonPtr(cJSON_Parse(("{" + reason + "\"cinematicPhase\":{" + phase + "}}").c_str()));
    };

    FakeMedia media;
    media.salt = {{"10000000-0000-4000-8000-000000000001", 11}};
    std::vector<std::uint16_t> last;
    int presents = 0;
    bool panel_ok = true;
    OriginalSourceScenePlayer player(loader, &media, nullptr, [&](const std::uint16_t* rgb565, int w, int h) {
        last.assign(rgb565, rgb565 + w * h);
        ++presents;
        return panel_ok;
    });
    // Background pixel no layer covers during the teach cue.
    const auto background_at = [&]() { return last.empty() ? 0 : last[310 * 480 + 470]; };
    const auto expected_background = [&](double media_ms) {
        const int frame = std::min(49, static_cast<int>(std::max(0.0, media_ms) / 40 + 1e-9));
        return Rgb565(frame * 5, 255 - frame * 5, 11);
    };

    // A robot clip that cannot be opened refuses the prepare before any ACK and
    // without consuming the sequence.
    media.fail_open = "20000000-0000-4000-8000-000000000003";  // greeting-teaching
    {
        auto prepare = control("prepare", "barn-teach", 1);
        const auto refused = player.Handle("lesson_prepare", prepare.get(), 0);
        Expect(!refused.accepted && refused.error == "media: integrity" && player.controller().active_cue() == nullptr,
               "unopenable original refuses prepare: " + refused.error);
    }
    media.fail_open.clear();
    const int presents_before = presents;
    {
        auto prepare = control("prepare", "barn-teach", 1);
        const auto accepted = player.Handle("lesson_prepare", prepare.get(), 0);
        Expect(accepted.accepted && accepted.asset_pack_ready, "prepare accepted once frame zero is ready");
        Expect(presents == presents_before + 1, "frame zero presented before the ACK");
        // The single-frame teaching object keeps its decoded copy and releases its
        // decoder and snapshot; background and robot streams stay open.
        Expect(media.live == 2, "ended streams are released (live " + std::to_string(media.live) + ")");
        Expect(media.last_cache_key == "farm-original/v1-" + std::string(64, 'a'), "streams open from the prepared pack");
    }
    TVideoFrameState state;
    TVideoFrameLayout layout;
    player.controller().FrameAt(0, &state, &layout);
    Expect(background_at() == expected_background(layout.background_media_time_ms), "frame zero background frame");
    Expect(player.Tick(50) == nullptr && presents == presents_before + 1, "prepared cue is not repainted");

    const int opened_after_prepare = media.opened;
    {
        auto start = control("start", "barn-teach", 2);
        Expect(player.Handle("lesson_start", start.get(), 1000).accepted, "start");
    }
    int repaint_checks = 0;
    for (std::uint64_t now = 1000; now <= 1000 + 2400; now += 37) {
        const int before = presents;
        Expect(player.Tick(now) == nullptr, "tick " + std::to_string(now));
        player.controller().FrameAt(now, &state, &layout);
        Expect(background_at() == expected_background(layout.background_media_time_ms),
               "browser seek rule at " + std::to_string(now));
        const int again = presents;
        player.Tick(now);
        Expect(presents == again, "no repaint within a frame");
        repaint_checks += presents > before;
    }
    Expect(repaint_checks > 10, "frames advance on the 100 ms clock");
    Expect(media.opened == opened_after_prepare, "an ended layer is not reopened while its frame stays valid");

    // Pause holds the frame; resume continues from it.
    {
        auto pause = control("pause", "barn-teach", 3);
        Expect(player.Handle("lesson_cinematic_control", pause.get(), 3500).accepted, "pause");
        const int before = presents;
        player.Tick(3500);
        const auto held = background_at();
        player.Tick(9000);
        Expect(background_at() == held && presents <= before + 1, "pause holds the frame");
    }

    // A looping cue rewinds its media: the stream is reopened at the wrap.
    {
        auto prepare = control("prepare", "barn-greet", 4);
        auto start = control("start", "barn-greet", 5);
        Expect(player.Handle("lesson_prepare", prepare.get(), 10000).accepted, "prepare greet");
        Expect(player.Handle("lesson_start", start.get(), 10000).accepted, "start greet");
        const double duration = TVideoEffectDurationMs(TVideoEffect::kGreet);
        const std::uint64_t opened = player.opened_streams();
        for (std::uint64_t t = 0; t <= static_cast<std::uint64_t>(duration) + 300; t += 100) {
            Expect(player.Tick(10000 + t) == nullptr, "greet tick");
        }
        player.controller().FrameAt(10000 + static_cast<std::uint64_t>(duration) + 300, &state, &layout);
        Expect(player.opened_streams() > opened, "loop wrap reopens a rewound stream");
        Expect(background_at() == expected_background(layout.background_media_time_ms), "after wrap");
    }

    // A decode failure surfaces once, then the layer reopens and recovers.
    {
        auto prepare = control("prepare", "barn-teach", 6);
        auto start = control("start", "barn-teach", 7);
        player.Handle("lesson_prepare", prepare.get(), 20000);
        player.Handle("lesson_start", start.get(), 20000);
        media.fail_decode_after_id = "10000000-0000-4000-8000-000000000001";
        media.fail_decode_after = 6;
        const char* error = nullptr;
        std::uint64_t now = 20000;
        for (; now < 22000 && error == nullptr; now += 100) error = player.Tick(now);
        Expect(error != nullptr && std::string(error) == "decode", "decode error surfaces");
        Expect(player.Tick(now) == nullptr, "next frame recovers by reopening");
        player.controller().FrameAt(now, &state, &layout);
        Expect(background_at() == expected_background(layout.background_media_time_ms), "recovered frame");
    }

    // A refused panel write is an error; the frame is retried next tick.
    panel_ok = false;
    Expect(player.Tick(30000) != nullptr, "panel refusal is reported");
    panel_ok = true;
    Expect(player.Tick(30000) == nullptr, "frame retried after panel refusal");

    // Terminal commands release every stream.
    {
        auto stop = control("stop", "barn-teach", 8);
        const auto stopped = player.Handle("lesson_stop", stop.get(), 31000);
        Expect(stopped.accepted && stopped.terminal && media.live == 0, "stop releases streams");
        Expect(player.Tick(32000) == nullptr && player.controller().active_cue() == nullptr, "idle after stop");
    }

    // A new lesson session restarts command sequences and reports its asset pack:
    // Reset forgets the previous session's ordering state and scene.
    {
        player.Reset();
        auto prepare = control("prepare", "barn-greet", 1);
        const auto fresh = player.Handle("lesson_prepare", prepare.get(), 40000);
        Expect(fresh.accepted && fresh.asset_pack_ready, "next session restarts at sequence 1 with its pack ACK");
        auto start = control("start", "barn-greet", 2);
        Expect(player.Handle("lesson_start", start.get(), 40000).accepted, "next session start");
        player.Reset();
        Expect(player.controller().active_cue() == nullptr && media.live == 0, "reset releases streams and the cue");
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failures of %d checks\n", failures, checks);
        return 1;
    }
    std::printf("PASS original-source player: %d checks (%llu frames presented)\n", checks,
                static_cast<unsigned long long>(player.presented_frames()));
    return 0;
}
