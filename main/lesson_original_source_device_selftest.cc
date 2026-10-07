#include "lesson_original_source_device_selftest.h"

#include <cstring>
#include <set>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && defined(CONFIG_TBOT_LESSON_RENDERER_V6_DEVICE_SELFTEST)
#include <cJSON.h>
#include <dirent.h>
#include <driver/sdmmc_host.h>
#include <fcntl.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>

#include "lesson_asset_storage_coordinator.h"
#include "lesson_original_source_allocator.h"
#include "lesson_original_source_profile.h"
#include "lesson_original_source_runtime.h"
#endif

namespace tbot {
namespace {

std::uint32_t ReadU32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool SafeName(const std::string& name) {
    if (name.empty() || name[0] == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::uint32_t V6SelfTestMediaEntryCount(const std::uint8_t* header, std::size_t header_bytes) {
    if (header == nullptr || header_bytes < kV6SelfTestHeaderBytes) return 0;
    if (std::memcmp(header, kV6SelfTestMediaMagic, sizeof(kV6SelfTestMediaMagic)) != 0) return 0;
    if (ReadU32(header + 12) != 0) return 0;
    const std::uint32_t count = ReadU32(header + 8);
    return count <= kV6SelfTestMaxEntries ? count : 0;
}

std::string ParseV6SelfTestMediaIndex(const std::uint8_t* index, std::size_t index_bytes, std::size_t image_bytes,
                                      std::vector<V6SelfTestMediaEntry>& entries) {
    const std::uint32_t count = V6SelfTestMediaEntryCount(index, index_bytes);
    if (count == 0) return "not a v6 self-test media image";
    const std::size_t data_start = V6SelfTestIndexBytes(count);
    if (index_bytes < data_start || image_bytes < data_start) return "index truncated";
    std::vector<V6SelfTestMediaEntry> parsed;
    std::set<std::string> names;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* entry = index + kV6SelfTestHeaderBytes + static_cast<std::size_t>(i) * kV6SelfTestEntryBytes;
        const auto* name_end = static_cast<const std::uint8_t*>(std::memchr(entry, 0, kV6SelfTestNameBytes));
        if (name_end == nullptr) return "entry name is not terminated";
        for (const std::uint8_t* p = name_end; p < entry + kV6SelfTestNameBytes; ++p) {
            if (*p != 0) return "entry name padding is not zero";
        }
        V6SelfTestMediaEntry item;
        item.name.assign(reinterpret_cast<const char*>(entry), static_cast<std::size_t>(name_end - entry));
        if (!SafeName(item.name)) return "unsafe entry name";
        if (!names.insert(item.name).second) return "duplicate entry name";
        item.offset = ReadU32(entry + kV6SelfTestNameBytes);
        item.size = ReadU32(entry + kV6SelfTestNameBytes + 4);
        std::memcpy(item.sha256, entry + kV6SelfTestNameBytes + 8, sizeof(item.sha256));
        if (item.size == 0) return "empty entry";
        if (item.offset < data_start) return "entry overlaps the index";
        if (static_cast<std::uint64_t>(item.offset) + item.size > image_bytes) return "entry exceeds the image";
        parsed.push_back(std::move(item));
    }
    std::vector<const V6SelfTestMediaEntry*> order;
    for (const auto& item : parsed) order.push_back(&item);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->offset < b->offset; });
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (static_cast<std::uint64_t>(order[i - 1]->offset) + order[i - 1]->size > order[i]->offset) {
            return "entries overlap";
        }
    }
    if (names.count(kV6SelfTestPlanName) == 0) return "missing self-test plan";
    entries = std::move(parsed);
    return "";
}

#if defined(ESP_PLATFORM) && defined(CONFIG_TBOT_LESSON_RENDERER_V6_DEVICE_SELFTEST)
namespace {

constexpr char TAG[] = "V6SELFTEST";
constexpr char kPackRoot[] = "/sdcard/tbot/lesson-assets";
constexpr int kRuns = 1;
// Production calls Handle on the 32 KB lesson_worker stack; the self-test uses twice that
// and reports the high-water mark so an overflow there shows up as a number, not corruption.
constexpr unsigned kSelfTestStackBytes = 64 * 1024;
constexpr std::uint32_t kTickMs = 100;
constexpr std::size_t kCopyChunk = 16 * 1024;
std::atomic<bool> g_selftest_active{false};

struct Cue {
    std::string id;
    std::uint32_t duration_ms = 0;
};

struct Plan {
    std::string cache_key, scene_sha256;
    std::uint32_t scene_bytes = 0;
    std::vector<Cue> cues;
};

struct CJsonDeleter {
    void operator()(cJSON* json) const { cJSON_Delete(json); }
};

std::uint64_t NowMs() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }

std::string Hex(const std::uint8_t* bytes, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0xf]);
    }
    return out;
}

bool ParsePlan(const std::string& text, Plan& plan) {
    std::unique_ptr<cJSON, CJsonDeleter> root(cJSON_Parse(text.c_str()));
    if (!root) return false;
    const cJSON* key = cJSON_GetObjectItem(root.get(), "cacheKey");
    const cJSON* sha = cJSON_GetObjectItem(root.get(), "sceneSha256");
    const cJSON* bytes = cJSON_GetObjectItem(root.get(), "sceneBytes");
    const cJSON* cues = cJSON_GetObjectItem(root.get(), "cues");
    if (!cJSON_IsString(key) || !cJSON_IsString(sha) || !cJSON_IsNumber(bytes) || !cJSON_IsArray(cues)) return false;
    plan.cache_key = key->valuestring;
    plan.scene_sha256 = sha->valuestring;
    plan.scene_bytes = static_cast<std::uint32_t>(bytes->valuedouble);
    for (const cJSON* cue = cues->child; cue != nullptr; cue = cue->next) {
        const cJSON* id = cJSON_GetObjectItem(cue, "cueId");
        const cJSON* duration = cJSON_GetObjectItem(cue, "durationMs");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(duration) || duration->valuedouble <= 0) return false;
        plan.cues.push_back({id->valuestring, static_cast<std::uint32_t>(duration->valuedouble)});
    }
    return !plan.cache_key.empty() && plan.scene_sha256.size() == 64 && !plan.cues.empty();
}

// mkdir -p for every component of `path` below the pack root; records created directories.
bool MakeDirs(const std::string& path, std::vector<std::string>& created) {
    for (std::size_t pos = 1; pos != std::string::npos;) {
        pos = path.find('/', pos + 1);
        const std::string part = path.substr(0, pos);
        struct stat st {};
        if (stat(part.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
            continue;
        }
        if (mkdir(part.c_str(), 0775) != 0 && errno != EEXIST) return false;
        if (part.size() > std::strlen(kPackRoot)) created.push_back(part);
    }
    return true;
}

// `image` is the memory-mapped "v6media" partition (see CopyTask).
bool CopyEntry(const std::uint8_t* image, const V6SelfTestMediaEntry& entry, const std::string& path,
               std::uint8_t* buffer) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    bool ok = true;
    for (std::uint32_t done = 0; ok && done < entry.size;) {
        const std::uint32_t chunk = std::min<std::uint32_t>(kCopyChunk, entry.size - done);
        std::memcpy(buffer, image + entry.offset + done, chunk);
        ok = std::fwrite(buffer, 1, chunk, file) == chunk;
        if (ok) mbedtls_sha256_update(&sha, buffer, chunk);
        done += chunk;
    }
    std::uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    ok = ok && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    ok = (std::fclose(file) == 0) && ok;
    if (ok && std::memcmp(digest, entry.sha256, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "sha256 mismatch for %s: %s", entry.name.c_str(), Hex(digest, sizeof(digest)).c_str());
        ok = false;
    }
    if (!ok) unlink(path.c_str());
    return ok;
}

template <typename Body>
bool WithMutation(const char* what, Body body) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
        if (mutation) return body();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "%s: lesson storage mutation unavailable", what);
    return false;
}

struct RunResult {
    int errors = 0;
    std::uint64_t frames = 0;
};

RunResult PlayRun(int run, const Plan& plan) {
    // Playback uses the production route: the lesson_cinematic task (PSRAM stack, 10 ms
    // esp_timer) ticks the runtime; this task only sends the frames and samples.
    RunResult result;
    auto* runtime = ActiveOriginalSourceRuntime();
    auto& storage = LessonAssetStorageCoordinator::GetInstance();
    const std::string assignment = "assignment-v6-selftest";
    const std::string session = "session-v6-selftest-" + std::to_string(run);
    const LessonAssetSessionResult reservation = storage.TryBeginLessonSession(assignment, session);
    if (runtime == nullptr || !reservation.acquired) {
        ESP_LOGE(TAG, "run=%d runtime=%d session=%d", run, runtime != nullptr, reservation.acquired);
        result.errors = 1;
        return result;
    }
    runtime->DiscardSession();
    ConfigureProductionOriginalSourceSession(assignment, session, reservation.generation);
    OriginalSourceAllocatorStats stats{};
    ProductionOriginalSourceAllocatorStats(&stats, true);

    const std::size_t psram_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const std::size_t internal_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    std::size_t psram_min = psram_start, internal_min = internal_start;
    std::size_t internal_largest_min = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    std::uint64_t sequence = 0;
    double min_fps = 1e9;
    const std::uint64_t frames_start = runtime->PresentedFrames();
    const std::uint64_t run_start = NowMs();
    SetLessonCinematicTimerRouteV6(true);
    for (const Cue& cue : plan.cues) {
        const std::string prepare = "{\"cinematicPhase\":{\"command\":\"prepare\",\"cueId\":\"" + cue.id +
                                    "\",\"commandSequenceId\":" + std::to_string(++sequence) +
                                    ",\"scene\":{\"cacheKey\":\"" + plan.cache_key + "\",\"sha256\":\"" +
                                    plan.scene_sha256 + "\",\"bytes\":" + std::to_string(plan.scene_bytes) + "}}}";
        const std::string start = "{\"cinematicPhase\":{\"command\":\"start\",\"cueId\":\"" + cue.id +
                                  "\",\"commandSequenceId\":" + std::to_string(++sequence) + "}}";
        std::unique_ptr<cJSON, CJsonDeleter> prepare_json(cJSON_Parse(prepare.c_str()));
        std::unique_ptr<cJSON, CJsonDeleter> start_json(cJSON_Parse(start.c_str()));
        const std::uint64_t cue_frames = runtime->PresentedFrames();
        const OriginalSourcePlayerTimings cue_start = runtime->Timings();
        const OriginalSourceProfile profile_start = OriginalSourceProfileCounters();
        const std::int64_t prepare_us = esp_timer_get_time();
        const auto prepared = runtime->Handle("lesson_prepare", prepare_json.get(), NowMs());
        const double prepare_ms = (esp_timer_get_time() - prepare_us) / 1000.0;
        const unsigned stack_free = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
        const auto started = runtime->Handle("lesson_start", start_json.get(), NowMs());
        const std::uint64_t origin = NowMs();
        std::string error;
        TickType_t wake = xTaskGetTickCount();
        while (prepared.accepted && started.accepted && NowMs() - origin < cue.duration_ms) {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(kTickMs));
            if (auto pending = runtime->PendingRuntimeError()) {
                error = "runtime error code " + std::to_string(static_cast<int>(pending->error));
                break;
            }
            psram_min = std::min(psram_min, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            internal_min = std::min(internal_min, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            internal_largest_min =
                std::min(internal_largest_min, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        const std::uint64_t elapsed = std::max<std::uint64_t>(1, NowMs() - origin);
        const std::uint64_t frames = runtime->PresentedFrames() - cue_frames;
        const double fps = frames * 1000.0 / elapsed;
        const bool failed = !prepared.accepted || !started.accepted || !error.empty();
        if (!failed) min_fps = std::min(min_fps, fps);
        result.errors += failed;
        ESP_LOGI(TAG,
                 "cue run=%d id=%s prepared=%d started=%d frames=%lu ms=%lu fps=%.1f prepareMs=%.1f "
                 "stackUsedBytes=%u error=%s%s%s",
                 run, cue.id.c_str(), prepared.accepted, started.accepted, static_cast<unsigned long>(frames),
                 static_cast<unsigned long>(elapsed), fps, prepare_ms, kSelfTestStackBytes - stack_free,
                 error.c_str(),
                 prepared.accepted ? "" : prepared.error.c_str(), started.accepted ? "" : started.error.c_str());
        const OriginalSourcePlayerTimings cue_end = runtime->Timings();
        ESP_LOGI(TAG,
                 "timing run=%d id=%s openMs=%lu opens=%lu decodeMs=%lu decoded=%lu paintMs=%lu convertMs=%lu "
                 "presentMs=%lu renders=%lu",
                 run, cue.id.c_str(), static_cast<unsigned long>((cue_end.open_us - cue_start.open_us) / 1000),
                 static_cast<unsigned long>(cue_end.opens - cue_start.opens),
                 static_cast<unsigned long>((cue_end.decode_us - cue_start.decode_us) / 1000),
                 static_cast<unsigned long>(cue_end.decoded_frames - cue_start.decoded_frames),
                 static_cast<unsigned long>((cue_end.paint_us - cue_start.paint_us) / 1000),
                 static_cast<unsigned long>((cue_end.convert_us - cue_start.convert_us) / 1000),
                 static_cast<unsigned long>((cue_end.present_us - cue_start.present_us) / 1000),
                 static_cast<unsigned long>(cue_end.renders - cue_start.renders));
        const OriginalSourceProfile& profile = OriginalSourceProfileCounters();
        ESP_LOGI(TAG,
                 "profile run=%d id=%s readMs=%lu hashMs=%lu demuxOpenMs=%lu codecOpenMs=%lu drawMediaMs=%lu "
                 "fillMs=%lu textMs=%lu firstReadMs=%lu maxReadMsSoFar=%lu slowReads=%lu",
                 run, cue.id.c_str(), static_cast<unsigned long>((profile.read_us - profile_start.read_us) / 1000),
                 static_cast<unsigned long>((profile.hash_us - profile_start.hash_us) / 1000),
                 static_cast<unsigned long>((profile.demux_open_us - profile_start.demux_open_us) / 1000),
                 static_cast<unsigned long>((profile.codec_open_us - profile_start.codec_open_us) / 1000),
                 static_cast<unsigned long>((profile.draw_media_us - profile_start.draw_media_us) / 1000),
                 static_cast<unsigned long>((profile.fill_us - profile_start.fill_us) / 1000),
                 static_cast<unsigned long>((profile.text_us - profile_start.text_us) / 1000),
                 static_cast<unsigned long>((profile.first_read_us - profile_start.first_read_us) / 1000),
                 static_cast<unsigned long>(profile.max_read_us / 1000),
                 static_cast<unsigned long>(profile.slow_reads - profile_start.slow_reads));
        if (!error.empty()) break;
    }
    SetLessonCinematicTimerRouteV6(false);
    vTaskDelay(pdMS_TO_TICKS(200));
    result.frames = runtime->PresentedFrames() - frames_start;
    const std::uint64_t run_ms = NowMs() - run_start;
    ProductionOriginalSourceAllocatorStats(&stats, false);
    runtime->DiscardSession();
    storage.EndLessonSession(assignment, session, reservation.generation);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG,
             "summary run=%d cues=%u errors=%d frames=%lu runMs=%lu meanFps=%.1f minCueFps=%.1f streams=%lu "
             "decoderPeakBytes=%u decoderLiveBytes=%u decoderFailures=%u psramStart=%u psramMin=%u psramEnd=%u "
             "internalStart=%u internalMin=%u internalLargestMin=%u stackFreeMinBytes=%u",
             run, static_cast<unsigned>(plan.cues.size()), result.errors,
             static_cast<unsigned long>(result.frames), static_cast<unsigned long>(run_ms),
             result.frames * 1000.0 / std::max<std::uint64_t>(1, run_ms), min_fps > 1e8 ? 0.0 : min_fps,
             static_cast<unsigned long>(runtime->OpenedStreams()), static_cast<unsigned>(stats.peak_charged),
             static_cast<unsigned>(stats.live_charged), static_cast<unsigned>(stats.failures),
             static_cast<unsigned>(psram_start), static_cast<unsigned>(psram_min),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), static_cast<unsigned>(internal_start),
             static_cast<unsigned>(internal_min), static_cast<unsigned>(internal_largest_min),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    return result;
}

// Flash and MMU operations (esp_partition_mmap) freeze the cache, which a PSRAM task stack
// must not do: the media copy runs in a short internal-stack task.
struct CopyJob {
    std::vector<V6SelfTestMediaEntry>* entries;
    Plan* plan;
    std::vector<std::string>* files;
    std::vector<std::string>* dirs;
    SemaphoreHandle_t done;
    bool ok = false;
};

void CopyTask(void* raw) {
    auto* job = static_cast<CopyJob*>(raw);
    job->ok = false;
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "v6media");
    const void* mapped = nullptr;
    esp_partition_mmap_handle_t mapping = 0;
    std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> buffer(
        static_cast<std::uint8_t*>(heap_caps_malloc(kCopyChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (partition != nullptr &&
        esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &mapped, &mapping) != ESP_OK) {
        mapped = nullptr;
    }
    const auto* image = static_cast<const std::uint8_t*>(mapped);
    do {
        if (image == nullptr || !buffer) {
            ESP_LOGE(TAG, "v6media partition mapping or copy buffer unavailable");
            break;
        }
        const std::uint32_t count = V6SelfTestMediaEntryCount(image, kV6SelfTestHeaderBytes);
        if (count == 0) {
            ESP_LOGE(TAG, "media header invalid");
            break;
        }
        const std::string parse =
            ParseV6SelfTestMediaIndex(image, V6SelfTestIndexBytes(count), partition->size, *job->entries);
        if (!parse.empty()) {
            ESP_LOGE(TAG, "media index: %s", parse.c_str());
            break;
        }
        const auto plan_entry = std::find_if(job->entries->begin(), job->entries->end(),
                                             [](const auto& e) { return e.name == kV6SelfTestPlanName; });
        const std::string plan_text(reinterpret_cast<const char*>(image) + plan_entry->offset, plan_entry->size);
        if (!ParsePlan(plan_text, *job->plan)) {
            ESP_LOGE(TAG, "self-test plan invalid");
            break;
        }
        const std::string pack = std::string(kPackRoot) + "/" + job->plan->cache_key;
        const std::int64_t copy_us = esp_timer_get_time();
        job->ok = WithMutation("copy", [&]() {
            if (!MakeDirs(pack, *job->dirs)) {
                ESP_LOGE(TAG, "cannot create %s", pack.c_str());
                return false;
            }
            for (const auto& entry : *job->entries) {
                if (entry.name == kV6SelfTestPlanName) continue;
                const std::string path = pack + "/" + entry.name;
                if (!CopyEntry(image, entry, path, buffer.get())) {
                    ESP_LOGE(TAG, "copy failed: %s", entry.name.c_str());
                    return false;
                }
                job->files->push_back(path);
            }
            return true;
        });
        if (job->ok) {
            ESP_LOGI(TAG, "pack ready cacheKey=%s files=%u copyMs=%ld", job->plan->cache_key.c_str(),
                     static_cast<unsigned>(job->files->size()),
                     static_cast<long>((esp_timer_get_time() - copy_us) / 1000));
        }
    } while (false);
    if (mapped != nullptr) esp_partition_munmap(mapping);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

// SD throughput with nothing else running: the SDMMC clock, a 256 KiB write + fsync, and
// reads of that file via POSIX read() into internal DMA memory and via unbuffered fread()
// into PSRAM (as OriginalSourceSession::Open reads).
void SdBenchmark(const std::string& cache_key) {
    for (int slot : {0, 1}) {
        int khz = 0;
        const esp_err_t err = sdmmc_host_get_real_freq(slot, &khz);
        ESP_LOGI(TAG, "sdbench slot=%d realFreqKhz=%d err=%d", slot, khz, static_cast<int>(err));
    }
    constexpr std::size_t kBytes = 256 * 1024, kChunk = 4096;
    const std::string path = std::string(kPackRoot) + "/" + cache_key + "/sdbench.tmp";
    std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> internal(
        static_cast<std::uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)), heap_caps_free);
    std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> psram(
        static_cast<std::uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!internal || !psram) {
        ESP_LOGW(TAG, "sdbench buffers unavailable");
        return;
    }
    std::memset(psram.get(), 0x5a, kChunk);
    WithMutation("sdbench", [&]() {
        std::int64_t at = esp_timer_get_time();
        FILE* out = std::fopen(path.c_str(), "wb");
        bool ok = out != nullptr;
        for (std::size_t done = 0; ok && done < kBytes; done += kChunk) ok = std::fwrite(psram.get(), 1, kChunk, out) == kChunk;
        ok = ok && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        if (out) std::fclose(out);
        ESP_LOGI(TAG, "sdbench write bytes=%u ms=%ld ok=%d", static_cast<unsigned>(kBytes),
                 static_cast<long>((esp_timer_get_time() - at) / 1000), ok);
        at = esp_timer_get_time();
        const int fd = ::open(path.c_str(), O_RDONLY);
        std::size_t total = 0;
        std::int64_t slowest = 0;
        for (ssize_t got = 1; fd >= 0 && got > 0;) {
            const std::int64_t t = esp_timer_get_time();
            got = ::read(fd, internal.get(), kChunk);
            slowest = std::max<std::int64_t>(slowest, esp_timer_get_time() - t);
            if (got > 0) total += static_cast<std::size_t>(got);
        }
        if (fd >= 0) ::close(fd);
        ESP_LOGI(TAG, "sdbench posixReadInternal bytes=%u ms=%ld slowestReadMs=%ld", static_cast<unsigned>(total),
                 static_cast<long>((esp_timer_get_time() - at) / 1000), static_cast<long>(slowest / 1000));
        at = esp_timer_get_time();
        FILE* in = std::fopen(path.c_str(), "rb");
        total = 0;
        slowest = 0;
        if (in) {
            std::setvbuf(in, nullptr, _IONBF, 0);
            for (std::size_t got = kChunk; got == kChunk;) {
                const std::int64_t t = esp_timer_get_time();
                got = std::fread(psram.get(), 1, kChunk, in);
                slowest = std::max<std::int64_t>(slowest, esp_timer_get_time() - t);
                total += got;
            }
            std::fclose(in);
        }
        ESP_LOGI(TAG, "sdbench freadPsramUnbuffered bytes=%u ms=%ld slowestReadMs=%ld", static_cast<unsigned>(total),
                 static_cast<long>((esp_timer_get_time() - at) / 1000), static_cast<long>(slowest / 1000));
        unlink(path.c_str());
        return true;
    });
}

void SelfTestTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    g_selftest_active.store(true);
    ESP_LOGI(TAG, "start");
    std::vector<V6SelfTestMediaEntry> entries;
    Plan plan;
    std::vector<std::string> files, dirs;
    CopyJob job{&entries, &plan, &files, &dirs, xSemaphoreCreateBinary()};
    bool ok = job.done != nullptr &&
              xTaskCreate(&CopyTask, "v6_selftest_copy", 8192, &job, tskIDLE_PRIORITY + 2, nullptr) == pdPASS &&
              xSemaphoreTake(job.done, portMAX_DELAY) == pdTRUE && job.ok;
    if (ok) {
        SdBenchmark(plan.cache_key);
        int errors = 0;
        for (int run = 1; run <= kRuns; ++run) errors += PlayRun(run, plan).errors;
        ok = errors == 0;
    }
    const bool cleaned = WithMutation("cleanup", [&]() {
        bool all = true;
        for (const auto& file : files) all = (unlink(file.c_str()) == 0) && all;
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) all = (rmdir(it->c_str()) == 0) && all;
        return all;
    });
    g_selftest_active.store(false);
    ESP_LOGI(TAG, "complete pass=%d cleaned=%d", ok, cleaned);
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

void StartOriginalSourceDeviceSelfTest() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    if (xTaskCreateWithCaps(&SelfTestTask, "v6_selftest", kSelfTestStackBytes, nullptr, tskIDLE_PRIORITY + 2, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
    }
}
bool OriginalSourceDeviceSelfTestActive() { return g_selftest_active.load(); }
#else
void StartOriginalSourceDeviceSelfTest() {}
bool OriginalSourceDeviceSelfTestActive() { return false; }
#endif

}  // namespace tbot
