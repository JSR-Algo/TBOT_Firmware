#include "application.h"
#include "chat_runtime_timing.h"
#include "lesson_queue_producer.h"
#include "wifi_config_entry_policy.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/lvgl_display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "robot_uart.h"
#include "app_manager.h"
#include "tbot_connect_mapper.h"
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
#include "lesson_heap_probe.h"
#include "lesson_asset_storage_coordinator.h"
#endif
#include "passive_reconnect_policy.h"
#include "protocol_lifetime_token.h"
#include "esp_build_identity.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "boards/common/blufi.h"
#include "boards/common/system_reset.h"
#include "boards/common/wifi_board.h"
#include <ssid_manager.h>
#include <wifi_manager.h>
#endif

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#if CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS
#include "course_mode_hil_sd_reader.h"
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#endif

#define TAG "Application"

static constexpr uint32_t kListenPlaybackDrainTimeoutMs = 650;
static constexpr uint32_t kSpeakingTimeoutMs = 12000;
static constexpr uint32_t kTtsStopPlaybackDrainTimeoutMs = 2000;
static constexpr uint32_t kListeningNoSpeechTimeoutMs = 15000;
static constexpr uint32_t kListeningAutoStopMaxTurnMs = 10000;
static constexpr uint32_t kListeningMaxTurnMs = 60000;
static constexpr uint32_t kListeningRealtimeNoSpeechTimeoutMs = 15000;

static void CancelLessonRobotEntranceOnDisplay() {
    Display* display = Board::GetInstance().GetDisplay();
    if (auto* lvgl_display = dynamic_cast<LvglDisplay*>(display)) {
        lvgl_display->CancelLessonRobotEntrance();
    }
}

static void SecureClearString(std::string& value) {
    if (!value.empty()) {
        volatile char* bytes = &value[0];
        for (std::size_t i = 0; i < value.size(); ++i) {
            bytes[i] = '\0';
        }
    }
    value.clear();
}

class SecureStringScope {
public:
    explicit SecureStringScope(std::string& value) : value_(value) {}
    ~SecureStringScope() { SecureClearString(value_); }

    SecureStringScope(const SecureStringScope&) = delete;
    SecureStringScope& operator=(const SecureStringScope&) = delete;

private:
    std::string& value_;
};

static constexpr int kWakeWordAudioChannelOpenMaxAttempts = 3;
static constexpr uint32_t kWakeWordAudioChannelRetryDelayMs = 700;
static constexpr uint64_t kConnectWatchdogTimeoutUs = 35ULL * 1000000ULL;
static constexpr uint32_t kMaxAudioPacketsPerMainLoop = 4;
static constexpr uint32_t kOpenChannelWorkerStackDepth = 8192;
static constexpr uint32_t kChatOutboundWorkerStackDepth = 8192;
static constexpr uint32_t kChatAudioCleanupWorkerStackDepth = 8192;
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
static constexpr UBaseType_t kLessonMessageQueueDepth = kLessonMessageDataQueueDepth;
static constexpr uint32_t kLessonMessageWorkerStackDepth = 32768;
static constexpr uint32_t kLessonMessageWorkerMinimumFreeStackBytes = 4096;
#endif

namespace {
enum class NetworkWorkKind : uint8_t {
    kOpenChannel,
    kHeartbeat,
    kProtocolCleanup,
};

struct NetworkWorkItem {
    NetworkWorkKind kind;
    void* context;
};

DRAM_ATTR StaticTask_t open_channel_task_buffer;
DRAM_ATTR StackType_t open_channel_task_stack[kOpenChannelWorkerStackDepth];
DRAM_ATTR StaticQueue_t open_channel_queue_buffer;
DRAM_ATTR NetworkWorkItem open_channel_queue_storage[2];
QueueHandle_t open_channel_queue = nullptr;
TaskHandle_t open_channel_task = nullptr;
DRAM_ATTR StaticTask_t chat_outbound_task_buffer;
DRAM_ATTR StackType_t chat_outbound_task_stack[kChatOutboundWorkerStackDepth];
DRAM_ATTR StaticTask_t chat_audio_cleanup_task_buffer;
DRAM_ATTR StackType_t chat_audio_cleanup_task_stack[kChatAudioCleanupWorkerStackDepth];

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
DRAM_ATTR StaticTask_t lesson_message_task_buffer;
DRAM_ATTR StaticQueue_t lesson_message_queue_buffer;
StackType_t* lesson_message_task_stack = nullptr;
uint8_t* lesson_message_queue_storage = nullptr;

void LogLessonWorkerStackWatermark(const char* stage) {
    const UBaseType_t free_stack_bytes = uxTaskGetStackHighWaterMark(nullptr);
    if (free_stack_bytes < kLessonMessageWorkerMinimumFreeStackBytes) {
        ESP_LOGE(TAG, "lesson_worker stack low stage=%s free=%u threshold=%u",
                 stage, static_cast<unsigned>(free_stack_bytes),
                 static_cast<unsigned>(kLessonMessageWorkerMinimumFreeStackBytes));
        return;
    }
    ESP_LOGI(TAG, "lesson_worker stack stage=%s free=%u", stage,
             static_cast<unsigned>(free_stack_bytes));
}
#endif
}  // namespace

static constexpr int kOtaCheckMaxAttempts = 3;
static constexpr int kOtaRetryDelaysSeconds[] = {2, 4};
static constexpr int kOtaCheckPhaseBudgetMs =
    kOtaCheckMaxAttempts * Ota::kHttpTimeoutMs +
    (kOtaRetryDelaysSeconds[0] + kOtaRetryDelaysSeconds[1]) * 1000;
static_assert(kOtaCheckPhaseBudgetMs <= 60000,
              "OTA activation check must remain inside the boot phase budget");

// TBOT claim poll (C4): cadence 10s. The backend's 5-minute cap applies only
// after a pending claim exists; unclaimed standby must keep polling so a late
// phone scan can still find and claim the robot.
static constexpr uint64_t kClaimPollIntervalUs =
    10ULL *
    1000000ULL;  // 10s (was 4s: blocking HTTP/TLS poll was hammering main task + flaky backend)
// "Hi ESP needs many tries" fix: once the realtime WS is up (online_intent_)
// the device is fully functional and the claim poll is pure background. Back it
// off hard so a residual still-polling state (e.g. online-but-not-yet-claimed)
// cannot keep waking the network stack every 10s and jittering live audio.
static constexpr uint64_t kClaimPollIntervalIdleUs = 60ULL * 1000000ULL;  // 60s while online
static constexpr int64_t kClaimPollWindowMs = 5LL * 60LL * 1000LL;        // 5 min cap
// The phone creates the backend claim before handing credentials/token to the
// robot. Allow one slow visibility retry, then recover discovery while enough
// internal heap remains to initialize Bluedroid.
static constexpr int64_t kClaimVisibilityRetryWindowMs = 20LL * 1000LL;

// TBOT heartbeat (C5): POST /v1/device/heartbeat every 20s while claimed/online.
static constexpr uint64_t kHeartbeatIntervalUs = 20ULL * 1000000ULL;  // 20s

Application::Application() {
    event_group_ = xEventGroupCreate();
    if (!InitializeChatOutboundWorker()) {
        ESP_LOGE(TAG, "Failed to create persistent chat outbound worker");
    }

    open_channel_queue = xQueueCreateStatic(
        2, sizeof(NetworkWorkItem), reinterpret_cast<uint8_t*>(open_channel_queue_storage),
        &open_channel_queue_buffer);
    if (open_channel_queue != nullptr) {
        open_channel_task = xTaskCreateStatic(
            &Application::OpenChannelTask, "lesson_ws", kOpenChannelWorkerStackDepth, this,
            tskIDLE_PRIORITY + 3, open_channel_task_stack, &open_channel_task_buffer);
    }
    if (open_channel_queue == nullptr || open_channel_task == nullptr) {
        ESP_LOGE(TAG, "Failed to create persistent internal websocket worker");
    }

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    constexpr uint32_t kLessonWorkerMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    lesson_message_task_stack = static_cast<StackType_t*>(heap_caps_malloc(
        kLessonMessageWorkerStackDepth * sizeof(StackType_t), kLessonWorkerMemoryCaps));
    lesson_message_queue_storage = static_cast<uint8_t*>(heap_caps_malloc(
        (kLessonMessageQueueDepth + 1) * sizeof(LessonQueueItem),
        kLessonWorkerMemoryCaps));
    if (lesson_message_task_stack != nullptr && lesson_message_queue_storage != nullptr) {
        lesson_message_queue_ = xQueueCreateStatic(
            kLessonMessageQueueDepth + 1, sizeof(LessonQueueItem),
            lesson_message_queue_storage, &lesson_message_queue_buffer);
    }
    if (lesson_message_queue_ != nullptr && lesson_message_task_stack != nullptr) {
        lesson_message_task_handle_ = xTaskCreateStatic(
            &Application::LessonMessageTask, "lesson_worker", kLessonMessageWorkerStackDepth, this,
            tskIDLE_PRIORITY + 2, lesson_message_task_stack, &lesson_message_task_buffer);
    }
    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        ESP_LOGE(TAG,
                 "Failed to create persistent PSRAM lesson worker stack=%p storage=%p queue=%p task=%p",
                 lesson_message_task_stack, lesson_message_queue_storage,
                 lesson_message_queue_, lesson_message_task_handle_);
        if (lesson_message_task_handle_ != nullptr) {
            vTaskDelete(lesson_message_task_handle_);
            lesson_message_task_handle_ = nullptr;
        }
        lesson_message_queue_ = nullptr;
        heap_caps_free(lesson_message_queue_storage);
        lesson_message_queue_storage = nullptr;
        heap_caps_free(lesson_message_task_stack);
        lesson_message_task_stack = nullptr;
    }
#endif

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    CancelLessonRobotEntranceOnDisplay();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (claim_poll_timer_ != nullptr) {
        esp_timer_stop(claim_poll_timer_);
        esp_timer_delete(claim_poll_timer_);
    }
    if (claim_expiry_timer_ != nullptr) {
        esp_timer_stop(claim_expiry_timer_);
        esp_timer_delete(claim_expiry_timer_);
    }
    if (claim_assets_retry_timer_ != nullptr) {
        esp_timer_stop(claim_assets_retry_timer_);
        esp_timer_delete(claim_assets_retry_timer_);
    }
    if (heartbeat_timer_ != nullptr) {
        esp_timer_stop(heartbeat_timer_);
        esp_timer_delete(heartbeat_timer_);
    }
    if (speaking_timeout_timer_ != nullptr) {
        esp_timer_stop(speaking_timeout_timer_);
        esp_timer_delete(speaking_timeout_timer_);
    }
    if (lesson_asset_sync_wake_rearm_timer_ != nullptr) {
        esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
        esp_timer_delete(lesson_asset_sync_wake_rearm_timer_);
    }
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    StopLessonMessageTask();
    if (lesson_message_queue_ != nullptr) {
        LessonQueueItem item;
        while (xQueueReceive(lesson_message_queue_, &item, 0) == pdTRUE) {
            delete static_cast<ChatRequestContext*>(item.source_context);
            if (item.kind == LessonQueueItemKind::kFrame && item.payload != nullptr) {
                cJSON_free(item.payload);
            }
        }
        lesson_message_queue_ = nullptr;
    }
    heap_caps_free(lesson_message_queue_storage);
    lesson_message_queue_storage = nullptr;
    heap_caps_free(lesson_message_task_stack);
    lesson_message_task_stack = nullptr;
#endif
    vEventGroupDelete(event_group_);
}

void Application::StopLessonMessageTask() {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    if (!lesson_message_task_handle_) return;
    lesson_message_stop_.store(true);
    while (lesson_message_producers_.load()) vTaskDelay(pdMS_TO_TICKS(1));
    LessonQueueItem wake{LessonQueueItemKind::kAbandonTransport, nullptr, 0};
    // A full queue already wakes the worker; never wait for an extra slot.
    xQueueSendToFront(lesson_message_queue_, &wake, 0);
    // IDF eTaskGetState holds the kernel lock and reports either running core
    // before eSuspended. This task has no resume path once its locals unwind.
    while (!lesson_message_retired_.load() || eTaskGetState(lesson_message_task_handle_) != eSuspended)
        vTaskDelay(pdMS_TO_TICKS(1));
    vTaskDelete(lesson_message_task_handle_);
    lesson_message_task_handle_ = nullptr;
#endif
}

void Application::EnqueueLessonVisualCompletion(
    LessonQueueItemKind kind,
    std::uint64_t transport_epoch,
    std::uint64_t visual_generation,
    std::int64_t server_sequence,
    const char* assignment_id,
    const char* session_id,
    const char* step_id,
    LessonVisualCompletionResult result,
    const char* degraded_reason,
    std::uint64_t visual_nonce
) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer) return;
    if ((kind != LessonQueueItemKind::kVisualCompleted &&
         kind != LessonQueueItemKind::kVisualTimedOut) ||
        lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        return;
    }
    LessonQueueItem item = MakeLessonVisualQueueItem(
        kind, transport_epoch, visual_generation, server_sequence,
        assignment_id, session_id, step_id, result, degraded_reason, visual_nonce);
    if (!lesson_queue_data_admission_.TryAcquire()) {
        ESP_LOGW(TAG, "lesson visual completion dropped: data capacity full seq=%ld",
                 static_cast<long>(server_sequence));
        return;
    }
    if (xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE) {
        lesson_queue_data_admission_.Release();
        ESP_LOGW(TAG, "lesson visual completion dropped: worker queue full seq=%ld",
                 static_cast<long>(server_sequence));
    }
#else
    (void)kind;
    (void)transport_epoch;
    (void)visual_generation;
    (void)server_sequence;
    (void)assignment_id;
    (void)session_id;
    (void)step_id;
    (void)result;
    (void)degraded_reason;
    (void)visual_nonce;
#endif
}

void Application::EnqueueLessonEmbodiedCompletion(
    LessonQueueItemKind kind,
    std::uint64_t transport_epoch,
    const char* assignment_id,
    const char* session_id,
    const char* step_id,
    const char* action_id,
    std::uint64_t action_generation,
    std::uint64_t embodied_nonce
) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer) return;
    if ((kind != LessonQueueItemKind::kEmbodiedHoldCompleted &&
         kind != LessonQueueItemKind::kEmbodiedSettled) ||
        lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        return;
    }
    LessonQueueItem item{};
    item.kind = kind;
    item.transport_epoch = transport_epoch;
    item.action_generation = action_generation;
    item.embodied_nonce = embodied_nonce;
    std::snprintf(item.assignment_id, sizeof(item.assignment_id), "%s", assignment_id);
    std::snprintf(item.session_id, sizeof(item.session_id), "%s", session_id);
    std::snprintf(item.step_id, sizeof(item.step_id), "%s", step_id);
    std::snprintf(item.action_id, sizeof(item.action_id), "%s", action_id);
    if (!lesson_queue_data_admission_.TryAcquire()) return;
    if (xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE) {
        lesson_queue_data_admission_.Release();
    }
#else
    (void)kind;
    (void)transport_epoch;
    (void)assignment_id;
    (void)session_id;
    (void)step_id;
    (void)action_id;
    (void)action_generation;
    (void)embodied_nonce;
#endif
}

void Application::EnqueueLessonMessage(
    const cJSON* root,
    std::uint64_t transport_epoch, ChatRequestContext context
) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer) return;
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const cJSON* sequence = cJSON_GetObjectItem(root, "sequence");
    const char* type_value = cJSON_IsString(type) ? type->valuestring : "(missing)";
    const int sequence_value = cJSON_IsNumber(sequence) ? sequence->valueint : -1;

    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        FailChatRequest(context);
        ESP_LOGW(TAG, "lesson_* dropped: worker unavailable type=%s seq=%d",
                 type_value, sequence_value);
        return;
    }

    LogLessonHeapBoundary("enqueue.before_serialize", 0);
    char* payload = cJSON_PrintUnformatted(root);
    const size_t payload_bytes = payload != nullptr ? strlen(payload) : 0;
    LogLessonHeapBoundary("enqueue.after_serialize", payload_bytes);
    if (payload == nullptr) {
        FailChatRequest(context);
        ESP_LOGW(TAG, "lesson_* dropped: serialize failed type=%s seq=%d",
                 type_value, sequence_value);
        return;
    }

    LessonQueueItem item{
        LessonQueueItemKind::kFrame,
        payload,
        transport_epoch,
    };
    if (context) {
        item.source_context = new (std::nothrow) ChatRequestContext(context);
        if (!item.source_context) { cJSON_free(payload); FailChatRequest(context); return; }
    }
    const bool queue_full =
        uxQueueMessagesWaiting(lesson_message_queue_) >= kLessonMessageQueueDepth;
    const bool admitted = !queue_full && lesson_queue_data_admission_.TryAcquire();
    if (!admitted || xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE) {
        if (admitted) lesson_queue_data_admission_.Release();
        ESP_LOGW(TAG, "lesson_* dropped: worker queue full type=%s seq=%d",
                 type_value, sequence_value);
        cJSON_free(payload);
        delete static_cast<ChatRequestContext*>(item.source_context);
        FailChatRequest(context);
    } else {
        ESP_LOGI(TAG, "lesson_* enqueued type=%s seq=%d bytes=%u",
                 type_value, sequence_value, (unsigned)payload_bytes);
    }
#else
    (void)root;
    (void)transport_epoch;
    (void)context;
#endif
}

void Application::RequestLessonStorageAbandonment() {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer) return;
    Schedule([]() { CancelLessonRobotEntranceOnDisplay(); });
    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) return;
    const std::uint64_t terminal_epoch =
        lesson_transport_epoch_gate_.PublishTerminalEpoch();
    const auto terminal_request = lesson_terminal_control_.Publish(terminal_epoch);
    if (!terminal_request.enqueue_control) return;
    LessonQueueItem item{
        LessonQueueItemKind::kAbandonTransport,
        nullptr,
        terminal_epoch,
    };
    if (xQueueSendToFront(lesson_message_queue_, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "lesson abandonment wakeup enqueue failed; worker will drain published epoch");
    }
#endif
}

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
void Application::LessonMessageTask(void* arg) {
    auto* self = static_cast<Application*>(arg);
    LessonQueueItem item;
    struct LessonProtocolRead {
        std::atomic<uint32_t>& readers;
        explicit LessonProtocolRead(std::atomic<uint32_t>& value) : readers(value) { readers.fetch_add(1); }
        ~LessonProtocolRead() { readers.fetch_sub(1); }
    };
    const auto drain_terminal = [self]() {
        constexpr int kLessonStorageAbandonMaxAttempts = 4;
        constexpr uint32_t kLessonStorageAbandonRetryDelayMs = 10;
        if (!self->lesson_terminal_control_.WorkerShouldDrain()) {
            return false;
        }
        for (;;) {
            const std::uint64_t latest_epoch =
                self->lesson_transport_epoch_gate_.PublishedEpoch();
            if (self->lesson_transport_epoch_gate_.WorkerApplyTerminal(latest_epoch)) {
                InvalidateLessonVisualCompletionState(latest_epoch);
                self->Schedule([]() { CancelLessonRobotEntranceOnDisplay(); });
            }
            if (!self->lesson_terminal_control_.FinishWorkerDrain(
                    self->lesson_transport_epoch_gate_, latest_epoch)) {
                break;
            }
        }
        bool released = false;
        for (int attempt = 0; attempt < kLessonStorageAbandonMaxAttempts; ++attempt) {
            if (self->AbandonLessonStorageSession()) {
                released = true;
                break;
            }
            if (attempt + 1 < kLessonStorageAbandonMaxAttempts) {
                vTaskDelay(pdMS_TO_TICKS(kLessonStorageAbandonRetryDelayMs));
            }
        }
        if (!released) {
            LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
            self->AbandonLessonStorageSession();
        }
        return true;
    };
    const auto poll_cinematic_error = [self]() {
        const auto epoch = PendingLessonCinematicErrorEpoch();
        if (epoch == 0) return;
        LessonProtocolRead protocol_read(self->lesson_protocol_readers_);
        if (!self->chat_protocol_owned_.load() &&
            self->lesson_transport_epoch_gate_.WorkerAcceptFrame(epoch)) {
            try {
                DispatchPendingLessonCinematicError(self->protocol_.get());
            } catch (...) {
                // Allocation or transport exceptions leave the diagnostic pending
                // for the next bounded poll, without terminating this worker.
            }
        }
    };
    while (!self->lesson_message_stop_.load()) {
        drain_terminal();
        poll_cinematic_error();
        if (xQueueReceive(self->lesson_message_queue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        std::unique_ptr<ChatRequestContext> source_context(static_cast<ChatRequestContext*>(item.source_context));
        item.source_context = nullptr;
        const ChatRequestContext context = source_context ? *source_context : ChatRequestContext();
        if (item.kind == LessonQueueItemKind::kAbandonTransport) {
            drain_terminal();
            continue;
        }
        self->lesson_queue_data_admission_.Release();
        drain_terminal();
        LessonProtocolRead protocol_read(self->lesson_protocol_readers_);
        if (!self->lesson_transport_epoch_gate_.WorkerAcceptFrame(item.transport_epoch) ||
            self->chat_protocol_owned_.load() || !self->IsChatLessonRequestCurrent(context)) {
            if (item.kind == LessonQueueItemKind::kFrame && item.payload != nullptr) {
                cJSON_free(item.payload);
                item.payload = nullptr;
            }
            continue;
        }
        if (item.kind == LessonQueueItemKind::kVisualCompleted ||
            item.kind == LessonQueueItemKind::kVisualTimedOut) {
            DispatchLessonVisualCompletion(item, self->protocol_.get(), &self->robot_uart_);
            continue;
        }
        if (item.kind == LessonQueueItemKind::kEmbodiedHoldCompleted ||
            item.kind == LessonQueueItemKind::kEmbodiedSettled) {
            DispatchLessonEmbodiedCompletion(item, self->protocol_.get());
            continue;
        }
        if (item.kind != LessonQueueItemKind::kFrame || item.payload == nullptr) continue;
        const size_t payload_bytes = strlen(item.payload);
        LogLessonWorkerStackWatermark("before_parse");
        LogLessonHeapBoundary("worker.before_parse", payload_bytes);
        cJSON* root = cJSON_Parse(item.payload);
        LogLessonHeapBoundary("worker.after_parse", payload_bytes);
        if (root != nullptr) {
            const cJSON* type = cJSON_GetObjectItem(root, "type");
            const cJSON* sequence = cJSON_GetObjectItem(root, "sequence");
            ESP_LOGI(TAG, "lesson_worker handling type=%s seq=%d",
                     cJSON_IsString(type) ? type->valuestring : "(missing)",
                     cJSON_IsNumber(sequence) ? sequence->valueint : -1);
            SetLessonTransportEpoch(item.transport_epoch);
            try { self->HandleLessonMessage(root, context); }
            catch (...) { self->FailChatRequest(context); }
            LogLessonWorkerStackWatermark("after_handle");
            LogLessonHeapBoundary("worker.after_handle", payload_bytes);
            cJSON_Delete(root);
            LogLessonHeapBoundary("worker.after_delete", payload_bytes);
        } else {
            self->FailChatRequest(context);
            ESP_LOGW(TAG, "lesson_* dropped: worker parse failed");
        }
        cJSON_free(item.payload);
        item.payload = nullptr;
        LogLessonHeapBoundary("worker.after_payload_free", payload_bytes);
    }
    // Publish only after the current frame's context and protocol reader unwind.
    self->lesson_message_retired_.store(true);
    vTaskSuspend(nullptr);
}
#endif

bool Application::SetDeviceState(DeviceState state) {
    if (state != kDeviceStateIdle) lesson_asset_sync_wake_invalidated_.store(true);
    if (state != kDeviceStateSpeaking) speaking_arm_dispatch_.Cancel();
    return state_machine_.TransitionTo(state);
}

bool Application::PrepareWifiConfigEntry(WifiConfigEntryPreparation& preparation) {
    preparation = {};
    if (wifi_config_preparation_.valid) {
        PollChatAudioCleanup();
        PollChatProtocolCleanup();
        if (lesson_runtime_active_.load() || reset_pending_.load() ||
            chat_protocol_state_.load(std::memory_order_acquire) != 0 ||
            protocol_work_lifetime_.Pending() || protocol_work_lifetime_.Busy() ||
            chat_audio_state_.load(std::memory_order_acquire) != 0 ||
            chat_audio_completed_revoked_ != wifi_config_audio_revoked_ ||
            chat_audio_fault_) {
            return false;
        }
        preparation = wifi_config_preparation_;
        wifi_config_preparation_ = {};
        ESP_LOGI(TAG, "WiFi config audio and protocol cleanup complete");
        return true;
    }
    const DeviceState state = GetDeviceState();
    if (!WifiConfigEntryPolicy::CanPrepare(
            state, lesson_runtime_active_.load(), connect_in_flight_.load(),
            reset_pending_.load())) {
        ESP_LOGW(TAG, "WiFi config preparation rejected: state=%d connect=%d reset=%d",
                 static_cast<int>(state), connect_in_flight_.load() ? 1 : 0,
                 reset_pending_.load() ? 1 : 0);
        return false;
    }

    preparation.original_state = state;
    preparation.resume_mode = state == kDeviceStateConnecting ? reconnect_mode_ : listening_mode_;
    preparation.resume_realtime = state == kDeviceStateConnecting ||
                                  state == kDeviceStateListening ||
                                  state == kDeviceStateSpeaking;
    preparation.resume_listening = state != kDeviceStateConnecting ||
                                   reconnect_resume_listening_.load();
    preparation.valid = true;
    CancelChatRecovery();

    ++connect_generation_;
    CancelConnectWatchdog();
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    connect_attempt_active_.store(false);
    passive_ws_intent_.store(false);
    reconnect_passive_.store(false);

    if (chat_cleanup_enabled_) {
        if (chat_protocol_signals_) chat_protocol_signals_->Disable();
        tts_audio_accepting_.store(false);
        microphone_uplink_authorized_.store(false);
        speaking_arm_dispatch_.Cancel();
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        wifi_config_audio_revoked_ =
            RequestChatAudioCleanup(speaking_generation_.load(), true, false, false);
        CloseAudioChannelByIntent();
        if (state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring && state != kDeviceStateIdle &&
            !SetDeviceState(kDeviceStateIdle)) {
            preparation.valid = false;
            return false;
        }
        // The network worker still owns the transport until cleanup is collected.
        // Retain the original state for rollback and resume on an application tick.
        wifi_config_preparation_ = preparation;
        preparation.valid = false;
        return false;
    }

    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (GetDeviceState() == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
    }
    CloseAudioChannelByIntent();
    audio_service_.ResetDecoder();
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);

    const DeviceState settled_state = GetDeviceState();
    if (settled_state != kDeviceStateStarting &&
        settled_state != kDeviceStateWifiConfiguring &&
        settled_state != kDeviceStateIdle) {
        if (!SetDeviceState(kDeviceStateIdle)) {
            ESP_LOGE(TAG, "WiFi config preparation could not settle state=%d",
                     static_cast<int>(settled_state));
            if (!RollbackWifiConfigEntry(preparation)) {
                ESP_LOGE(TAG, "WiFi config preparation rollback failed");
            }
            preparation.valid = false;
            return false;
        }
    }
    return true;
}

bool Application::PublishWifiConfigEntry(
        const WifiConfigEntryPreparation& preparation) {
    if (!preparation.valid) {
        return false;
    }
    if (!SetDeviceState(kDeviceStateWifiConfiguring)) {
        ESP_LOGE(TAG, "WiFi config state publication rejected from state=%d",
                 static_cast<int>(GetDeviceState()));
        return false;
    }
    return true;
}

bool Application::RollbackWifiConfigEntry(
        const WifiConfigEntryPreparation& preparation) {
    if (!preparation.valid) {
        return false;
    }
    if (preparation.original_state == kDeviceStateStarting ||
        preparation.original_state == kDeviceStateWifiConfiguring) {
        if (GetDeviceState() != preparation.original_state) {
            return false;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return true;
    }
    if (preparation.original_state == kDeviceStateActivating) {
        return SetDeviceState(kDeviceStateActivating);
    }
    if (!preparation.resume_realtime) {
        if (!SetDeviceState(kDeviceStateIdle)) {
            return false;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return true;
    }
    if (!SetDeviceState(kDeviceStateIdle)) {
        return false;
    }
    if (protocol_ == nullptr) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return true;
    }
    reconnect_resume_listening_.store(preparation.resume_listening);
    if (!SetDeviceState(kDeviceStateConnecting)) {
        return false;
    }
    Schedule([this, mode = preparation.resume_mode]() {
        ContinueOpenAudioChannel(mode);
    });
    return true;
}

void Application::Initialize() {
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
    ESP_LOGW(TAG, "TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image");
#endif
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    std::string build_identity_error;
    if (!PreloadRunningEspBuildIdentity(&build_identity_error)) {
        ESP_LOGW(TAG, "Build identity preload failed reason=%s", build_identity_error.c_str());
    }

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    if (IsDeviceClaimed()) {
        if (!audio_service_.Start()) {
            ESP_LOGE(TAG, "Claimed boot audio startup failed; initialization remains stopped");
            return;
        }
    } else {
        // Provisioning and claim workers require contiguous internal SRAM.
        // Unclaimed robots do not use wake-word, voice, or lesson audio, so do
        // not reserve the three audio task stacks until claim confirmation.
        ESP_LOGI(TAG, "Unclaimed boot: deferring audio workers until claim confirmation");
    }
    robot_uart_.Initialize();
    if (xTaskCreate([](void* context) {
            auto* self = static_cast<Application*>(context);
            while (true) {
                if (self->GetDeviceState() == kDeviceStateSpeaking) self->speaking_arm_dispatch_.Poll(
                    static_cast<uint64_t>(esp_timer_get_time() / 1000),
                    self->GetDeviceState() == kDeviceStateSpeaking &&
                        !self->lesson_runtime_active_.load(),
                    [self](const SpeakingArmGesture::Target& target, auto owns) {
                        return self->robot_uart_.TrySendAutomaticArm(
                            target.left, target.percent, [self, owns]() {
                                return owns() && self->GetDeviceState() == kDeviceStateSpeaking &&
                                    !self->lesson_runtime_active_.load();
                            });
                    });
                vTaskDelay(pdMS_TO_TICKS(25));
            }
        }, "speaking_arms", 3072, this, 1, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Speaking arm worker unavailable");
    }

    // App manager (Menu/Game overlay) + nhan su kien nut TTP223 tu slave qua UART.
    // Su kien den trong task doc UART -> marshal sang main task truoc khi dung LVGL.
    AppManagerInit();
    AppManagerSetSlaveSender([this](const char* line) {
        robot_uart_.SendControlLine(line);
    });
    AppManagerSetSoundPlayer([this](const std::vector<int16_t>& pcm) {
        audio_service_.QueuePcmForPlayback(pcm);
    });
    robot_uart_.SetEventCallback([this](RobotInputEvent evt) {
        Schedule([evt]() {
            switch (evt) {
                case RobotInputEvent::LeftClick:  AppHandleInputLeft(); break;
                case RobotInputEvent::RightClick: AppHandleInputRight(); break;
                case RobotInputEvent::BothClick:  AppHandleInputBothClick(); break;
                case RobotInputEvent::MenuHold:   AppHandleMenuHold(); break;
                case RobotInputEvent::RightHold:  AppHandleRightHold(); break;
                case RobotInputEvent::SlaveReady: AppOnSlaveReady(); break;
            }
        });
    });

    AudioServiceCallbacks callbacks;
    callbacks.on_playback_failed = [this](uint32_t response) {
        lesson_audio_playout_.PublishFailure(response);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.on_output_completed = [this](uint32_t response, bool conversation, uint32_t now_ms) {
        speaking_arm_dispatch_.PublishOutput(response, conversation, now_ms);
        lesson_audio_playout_.PublishOutput(response, conversation,
            static_cast<uint64_t>(esp_timer_get_time() / 1000));
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        // Record monotonic timestamp when VAD detects speech start. This is
        // consumed by HandleWakeWordDetectedEvent() to reject wake-word
        // false-positives on noise (no recent VAD-speaking window).
        if (speaking) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            last_vad_speech_ms_ = now_ms;
            if (GetDeviceState() == kDeviceStateListening) {
                last_listening_activity_ms_.store(now_ms);
            }
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        if (new_state != kDeviceStateSpeaking) speaking_arm_dispatch_.Cancel();
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        const bool lesson_active = lesson_runtime_active_.load();
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                if (!lesson_active) {
                    display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (lesson_active) {
                    break;
                }
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                if (!lesson_active) {
                    std::string msg = Lang::Strings::CONNECTED_TO;
                    msg += data;
                    display->ShowNotification(msg.c_str(), 30000);
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                if (!lesson_active) {
                    display->SetStatus(Lang::Strings::DETECTING_MODULE);
                }
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                if (!lesson_active) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                }
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    application_task_ = xTaskGetCurrentTaskHandle();
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    // WDT-1: subscribe the main task to the Task Watchdog Timer. The loop below
    // feeds it on every (bounded) pass, so a genuine hang inside a handler stops
    // the feed and TWDT logs a backtrace. PANIC stays OFF (sdkconfig) -> detection
    // only, no reboot, to avoid false reboots before HIL tuning.
    if (esp_task_wdt_add(nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add(main) failed - TWDT not enabled?");
    }

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED |
        MAIN_EVENT_CHAT_OUTBOUND;

    while (true) {
        // req#1: bounded wait (was portMAX_DELAY) so the loop always makes a pass,
        // feeds the watchdog, and never blocks forever.
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(lesson_playout_pending_.load() ? 20 : 2000));
        PollLessonAudioPlayout();
        PollChatOutboundEvents(bits & MAIN_EVENT_CHAT_OUTBOUND);
        esp_task_wdt_reset();  // WDT-1: prove the main loop is iterating

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            // WSS-8: while a connect cycle is still in progress (or a passive
            // lesson preconnect is running), a failed attempt is a RECOVERABLE
            // transient — the wake open-loop (3x) and ScheduleReconnect backoff
            // retry and usually land in Listening within a second or two over the
            // slow cold TLS/tunnel handshake. Flashing "Server unavailable.
            // Retrying..." on each attempt shows a scary error that immediately
            // self-clears. Keep the calm idle/connecting view; the banner is
            // surfaced once for wake-open exhaustion (and any non-connect error,
            // where neither flag is set, still alerts immediately). Listen-mode
            // reconnect keeps slow-period retrying for recovered endpoints.
            if (lesson_runtime_active_.load()) {
                ESP_LOGI(TAG, "lesson error suppressed: %s", last_error_message_.c_str());
                RequestLessonStorageAbandonment();
                lesson_interactive_listen_generation_.fetch_add(1);
                lesson_interactive_listen_pending_.store(false);
                lesson_interactive_listening_active_.store(false);
                auto display = Board::GetInstance().GetDisplay();
                display->SetStatus(Lang::Strings::PLEASE_WAIT);
            } else if (connect_attempt_active_.load() || passive_ws_intent_.load()) {
                ESP_LOGI(TAG, "connect error suppressed (recoverable): attempt_active=%d passive=%d in_flight=%d reconnect_attempt=%d",
                         connect_attempt_active_.load() ? 1 : 0,
                         passive_ws_intent_.load() ? 1 : 0,
                         connect_in_flight_.load() ? 1 : 0,
                         reconnect_attempt_);
            } else {
                // Resolve the connect state through the FSM mapper (backend_offline_
                // is set on the ws/backend error) so this render is mapper-driven,
                // not hand-coded. OFFLINE_RETRY -> localized "Server unavailable.
                // Retrying..."; any other resolved state keeps the generic ERROR
                // banner. Detailed error stays in the chat body + log.
                const TbotConnectState cs = TbotConnectMapper::ResolveState(
                    GetDeviceState(), claim_substate_, GetBleSubstate(),
                    backend_offline_.load());
                const char* status = (cs == TbotConnectState::OFFLINE_RETRY)
                    ? Lang::Strings::SERVER_UNAVAILABLE_RETRYING
                    : Lang::Strings::ERROR;
                Alert(status, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            }
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            RunScheduledTasks();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            if (IsSelectedNormalChatRoute()) {
                NotifyChatOutbound();
            } else if (chat_lesson_capture_token_) {
                PollChatLessonCapture(static_cast<uint64_t>(esp_timer_get_time()));
            } else {
            static uint32_t send_event_count = 0;
            static uint32_t send_packet_count = 0;
            static uint32_t lesson_render_defer_count = 0;
            send_event_count++;
            if (!IsMicrophoneUplinkAuthorized()) {
                audio_service_.EnableVoiceProcessing(false);
                uint32_t dropped_packets = 0;
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {
                    ++dropped_packets;
                }
                if (dropped_packets > 0) {
                    ESP_LOGW(TAG,
                             "microphone_uplink_blocked state=%d passive=%d online=%d dropped=%lu",
                             static_cast<int>(GetDeviceState()),
                             passive_ws_intent_.load() ? 1 : 0,
                             online_intent_.load() ? 1 : 0,
                             static_cast<unsigned long>(dropped_packets));
                }
            } else if (IsLessonNetworkRenderQuiet()) {
                lesson_render_defer_count++;
                if (lesson_render_defer_count == 1 || lesson_render_defer_count % 25 == 0) {
                    ESP_LOGI(TAG, "MAIN_EVENT_SEND_AUDIO deferred_for_lesson_render count=%lu",
                             static_cast<unsigned long>(lesson_render_defer_count));
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
                vTaskDelay(pdMS_TO_TICKS(20));
            } else {
                uint32_t sent_packets = 0;
                while (sent_packets < kMaxAudioPacketsPerMainLoop) {
                    auto packet = audio_service_.PopPacketFromSendQueue();
                    if (!packet) {
                        break;
                    }
                    const uint32_t timestamp = packet->timestamp;
                    const size_t payload_size = packet->payload.size();
                    if (!protocol_) {
                        ESP_LOGW(TAG, "MAIN_EVENT_SEND_AUDIO protocol_unavailable event=%lu payload_bytes=%u timestamp=%lu",
                                 static_cast<unsigned long>(send_event_count),
                                 static_cast<unsigned>(payload_size),
                                 static_cast<unsigned long>(timestamp));
                        break;
                    }
                    bool sent = protocol_->SendAudio(std::move(packet));
                    if (!sent) {
                        ESP_LOGW(TAG, "MAIN_EVENT_SEND_AUDIO send_failed event=%lu payload_bytes=%u timestamp=%lu",
                                 static_cast<unsigned long>(send_event_count),
                                 static_cast<unsigned>(payload_size),
                                 static_cast<unsigned long>(timestamp));
                        break;
                    }
                    send_packet_count++;
                    if (send_packet_count == 1 || send_packet_count % 25 == 0) {
                        ESP_LOGI(TAG, "MAIN_EVENT_SEND_AUDIO packet count=%lu payload_bytes=%u timestamp=%lu",
                                 static_cast<unsigned long>(send_packet_count),
                                 static_cast<unsigned>(payload_size),
                                 static_cast<unsigned long>(timestamp));
                    }
                    ++sent_packets;
                    esp_task_wdt_reset();
                }
                if (sent_packets == kMaxAudioPacketsPerMainLoop) {
                    xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            RunScheduledTasks();
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            HasLessonAssetSyncWakeOpportunity();
            if (protocol_start_pending_generation_ != 0 &&
                protocol_start_pending_generation_ == protocol_generation_.load() &&
                !protocol_work_lifetime_.Pending()) {
                StartProtocolWorker();
            }
            PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            if (wifi_config_preparation_.valid) {
                static_cast<WifiBoard&>(Board::GetInstance()).ResumePendingWifiConfigMode();
            }
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            HandleListeningWatchdogTick();

            bool passive_liveness_failed = false;
            const DeviceState passive_state = GetDeviceState();
            const bool selected_chat_source = chat_protocol_signals_ && chat_protocol_signals_->SourceSelected();
            if (passive_ws_intent_.load() &&
                IsDeviceClaimed() &&
                protocol_ != nullptr &&
                !connect_in_flight_.load() &&
                !(selected_chat_source ? protocol_work_lifetime_.BusyExcept(chat_outbound_reservation_) : protocol_work_lifetime_.Busy()) &&
                !protocol_work_lifetime_.Pending() &&
                // Do NOT tear down the passive WS while a lesson SD asset sync is
                // in flight: hashing the ~116MB pack starves the WS receive task so
                // server pongs miss the 10s window, but the connection is fine and
                // the server keeps it open. Killing it here aborts the sync and
                // starts an endless reconnect/re-sync loop. The timer is reset in
                // EndLessonAssetSyncQuiet() so liveness resumes cleanly afterward.
                // (Short-circuits before MaintainPassiveLiveness so no ping/pong
                // state is mutated during the sync.)
                !IsLessonAssetSyncQuiet() &&
                passive_state != kDeviceStateWifiConfiguring &&
                passive_state != kDeviceStateAudioTesting &&
                (selected_chat_source || protocol_->IsAudioChannelOpened()) &&
                !(selected_chat_source ? MaintainChatPassiveLiveness() : protocol_->MaintainPassiveLiveness())) {
                ESP_LOGW(TAG, "passive_lesson_ws_liveness_failed -> passive backoff");
                backend_offline_.store(true);
                if (chat_cleanup_enabled_) {
                    protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
                    PollChatProtocolCleanup();
                } else {
                    protocol_->CloseAudioChannel();
                }
                SchedulePassiveLessonReconnect();
                passive_liveness_failed = true;
            }

            if (!passive_liveness_failed &&
                !selected_chat_source &&
                !reconnect_passive_.load() &&
                clock_ticks_ % 10 == 0 &&
                IsDeviceClaimed() &&
                !lesson_runtime_active_.load() &&
                GetDeviceState() == kDeviceStateIdle &&
                protocol_ != nullptr &&
                !connect_in_flight_.load() &&
                !protocol_->IsAudioChannelOpened()) {
                ESP_LOGW(TAG, "passive_lesson_idle_socket_missing -> passive reconnect");
                StartPassiveLessonWebsocket();
            }
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                if (chat_cleanup_enabled_) {
                    PlaybackDrainSnapshot snapshot;
                    const bool available = audio_service_.TryGetPlaybackDrainSnapshot(snapshot);
                    ESP_LOGI(TAG, "chat_metrics snapshot_available=%d source_selected=%d reconnects=%lu",
                        available ? 1 : 0, chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() ? 1 : 0,
                        static_cast<unsigned long>(reconnect_count_.load()));
                } else {
                // Audio realtime metrics snapshot: queue depths + drop/stale
                // counters. Cheap, on the app task (NOT the audio hot path), so
                // it never jitters capture/playback. Lets us measure backpressure
                // (Patch 3.1/3.2) and barge-in stale-frame drops (Patch 3.3).
                uint32_t decode_q = 0, send_q = 0, playback_q = 0;
                audio_service_.GetQueueDepths(decode_q, send_q, playback_q);
                auto audio_stats = audio_service_.GetDebugStatistics();
                auto wake_progress = audio_service_.GetWakeWordProgress();
                ESP_LOGI(TAG, "audio_metrics decode_q=%lu send_q=%lu playback_q=%lu input_count=%lu wake_running=%d wake_feed=%lu wake_fetch=%lu wake_gen=%lu vp_running=%d decode_drop=%lu encode_drop=%lu stale_frames=%lu interrupts=%lu reconnects=%lu "
                              "wake_chunks=%lu wake_rms_min=%lu wake_rms_max=%lu wake_peak_max=%lu wake_above_floor=%lu wake_above_total=%lu wake_last_above_us=%lld "
                              "wn_none=%lu wn_transition=%lu wn_detected=%lu wn_other=%lu wn_model=%ld wn_bad_model=%lu",
                         (unsigned long)decode_q, (unsigned long)send_q, (unsigned long)playback_q,
                         (unsigned long)audio_stats.input_count,
                         audio_service_.IsWakeWordRunning() ? 1 : 0,
                         (unsigned long)wake_progress.feed_count,
                         (unsigned long)wake_progress.fetch_count,
                         (unsigned long)wake_progress.run_generation,
                         audio_service_.IsAudioProcessorRunning() ? 1 : 0,
                         (unsigned long)audio_stats.decode_drop_count,
                         (unsigned long)audio_stats.encode_drop_count,
                         (unsigned long)audio_stats.stale_frame_count,
                         (unsigned long)interrupt_count_.load(),
                         (unsigned long)reconnect_count_.load(),
                         (unsigned long)wake_progress.telemetry.chunk_count,
                         (unsigned long)wake_progress.telemetry.rms_min,
                         (unsigned long)wake_progress.telemetry.rms_max,
                         (unsigned long)wake_progress.telemetry.peak_max,
                         (unsigned long)wake_progress.telemetry.above_floor_count,
                         (unsigned long)wake_progress.telemetry.above_floor_total,
                         (long long)wake_progress.telemetry.last_above_floor_us,
                         (unsigned long)wake_progress.telemetry.state_none,
                         (unsigned long)wake_progress.telemetry.state_transition,
                         (unsigned long)wake_progress.telemetry.state_detected,
                         (unsigned long)wake_progress.telemetry.state_other,
                         (long)wake_progress.telemetry.last_valid_model_index,
                         (unsigned long)wake_progress.telemetry.invalid_model_index_count);
                // Stack high-water snapshots are sampled off the audio hot path.
                auto stack_hwm = audio_service_.GetTaskStackHighWaterMarks();
                ESP_LOGI(TAG, "sys_metrics stack_main_min=%u stack_audio_input_min=%ld "
                              "stack_audio_output_min=%ld stack_opus_codec_min=%ld "
                              "stack_afe_detection_min=%ld psram_free_b=%u",
                         (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                         (long)stack_hwm.audio_input,
                         (long)stack_hwm.audio_output,
                         (long)stack_hwm.opus_codec,
                         (long)stack_hwm.afe_detection,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                }
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting) {
        // Explicit BOOT Wi-Fi setup owns the screen. A stale STA connected event
        // from the previous online session must not leave setup mode and render
        // ONLINE / "Connected" before the phone finishes provisioning. BluFi
        // success has its own path: it reports to the phone, stops BLE, then
        // schedules RefreshPendingTbotClaim().
        ESP_LOGI(TAG, "Network connected ignored because WiFi config mode is active");
        auto display = Board::GetInstance().GetDisplay();
        display->UpdateStatusBar(true);
        return;
    }

    if (state == kDeviceStateStarting) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        // Unclaimed + BLE advertising leaves ~7–8KB largest free internal block.
        // The normal activation worker needs 8KB stack and fails to create, so the
        // UI freezes on "Loading setup..." forever. Run only the protocol setup
        // needed for public lesson sync and leave claimed bootstrap to BLE.
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
        if (!IsDeviceClaimed()) {
            ESP_LOGW(TAG,
                     "Unclaimed device on Wi-Fi: run minimal activation transport "
                     "without claimed bootstrap worker");
            CompleteUnclaimedProtocolOnlyActivation();
            return;
        }
#endif
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        BaseType_t created = xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create activation task (heap exhausted?)");
            activation_task_handle_ = nullptr;
            if (!ota_) {
                ota_ = std::make_unique<Ota>();
                ota_->MarkCurrentVersionValid();
            }
            xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
        }
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    RequestLessonStorageAbandonment();
    backend_recovery_window_.Reset();
    // H2: network is gone -> stop the heartbeat (no live online session to report
    // and no point blocking the main task on an unreachable backend). It restarts
    // only from OnConnected.
    StopHeartbeat();

    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    auto display = Board::GetInstance().GetDisplay();
    if (chat_cleanup_enabled_ && chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !IsLessonVoiceRoute() && (online_intent_.load() || passive_ws_intent_.load())) {
        // Radio loss must retain recovery intent; an intentional close cancels it.
        ConnectionSource source;
        if (chat_protocol_signals_->TrySource(source)) {
            ESP_LOGW(TAG, "chat_source_fault reason=wifi_disconnected");
            chat_protocol_signals_->PublishConnectionFault(source, chat_source_connect_generation_.load(),
                ChatProtocolSignals::Error);
        }
        PollChatProtocolSignals();
        display->UpdateStatusBar(true);
        return;
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        backend_offline_.store(true);
        if (chat_cleanup_enabled_) {
            if (chat_protocol_signals_) chat_protocol_signals_->Disable();
            tts_audio_accepting_.store(false);
            microphone_uplink_authorized_.store(false);
            RequestChatAudioCleanup(speaking_generation_.load(), true, false, false);
        } else audio_service_.ResetDecoder();
        CloseAudioChannelByIntent();
        if (lesson_runtime_active_.load()) {
            lesson_interactive_listen_generation_.fetch_add(1);
            lesson_interactive_listen_pending_.store(false);
            lesson_interactive_listening_active_.store(false);
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
        } else {
            display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
            display->SetEmotion("thinking");
            if (chat_cleanup_enabled_) RequestChatCue(Lang::Sounds::OGG_EXCLAMATION);
            else audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    }

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::RearmClaimedIdleWakeWord() {
    if (IsWifiConfigEntryPending()) return;
    if (chat_cleanup_enabled_) {
        ConnectionSource source;
        if (IsDeviceClaimed() && !lesson_runtime_active_.load() && !lesson_asset_sync_quiet_.load() &&
            GetDeviceState() == kDeviceStateIdle && !connect_in_flight_.load() &&
            (!passive_ws_intent_.load() || (chat_protocol_signals_ && chat_protocol_signals_->TrySource(source))))
            RequestChatAudioCleanup(speaking_generation_.load(), false, false, true);
        return;
    }
    if (!IsDeviceClaimed() || lesson_runtime_active_.load() ||
        lesson_asset_sync_quiet_.load() || GetDeviceState() != kDeviceStateIdle ||
        connect_in_flight_.load() ||
        (passive_ws_intent_.load() &&
         (protocol_ == nullptr || !protocol_->IsAudioChannelOpened()))) {
        return;
    }
    audio_service_.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "claimed_idle_wake_word_rearmed running=%d",
             audio_service_.IsWakeWordRunning() ? 1 : 0);
}

void Application::HandleActivationDoneEvent() {
    auto state = GetDeviceState();
    if (state == kDeviceStateWifiConfiguring) {
        ESP_LOGI(TAG, "Activation done ignored because WiFi config mode is active");
        return;
    }
    if (state == kDeviceStateConnecting ||
        state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Activation done ignored because runtime audio is active");
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "Activation done ignored because lesson runtime is active");
        return;
    }

    ESP_LOGI(TAG, "Activation done");

    // Migrate robots that received a heartbeat revocation on older firmware.
    // That handler cleared only device_secret, leaving device_id plus the
    // factory-test WebSocket marker to impersonate a claimed device forever.
    if (HasStaleRevokedClaimIdentity()) {
        ESP_LOGW(TAG, "Detected stale revoked claim identity; reopening WiFi setup");
        HandleHeartbeatAuthFailure(401);
        return;
    }

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);
    if (ShouldKeepManagementHeartbeat()) {
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    }
    RearmClaimedIdleWakeWord();

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    ota_.reset();
#endif
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    RefreshPendingTbotClaim();

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::RefreshPendingTbotClaim() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#endif
    // BOOT re-pair (offline path): if an earlier re-pair could not reach the
    // cloud, backend.release_pending is still set and the robot rebooted into
    // Wi-Fi setup (SsidManager::Clear() -> kDeviceStateWifiConfiguring). The
    // deferred release MUST fire as soon as we are refreshing claim state on the
    // NEW network, even while still in kDeviceStateWifiConfiguring (the exact
    // post-re-pair-reboot state) -- so it runs BEFORE the WifiConfiguring
    // early-return below, which would otherwise strand it and leave the backend
    // devices row owned by the OLD parent (new parent's claim -> DEVICE_ALREADY_OWNED).
    // Off-task + single-flight; it self-clears release_pending on success and is a
    // no-op when release_pending is unset (every normal flow is untouched).
    MaybeDispatchDeferredCloudRelease();

    // Explicit setup mode owns the screen + BLE radio. A stale claim poll from
    // the previous online/standby state must not keep fetching /device/config
    // after Wi-Fi station has been stopped, or it overwrites the BluFi setup UI
    // with "Server unavailable. Retrying..." while the robot is intentionally
    // waiting for the mobile app.
    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        StopClaimPoll();
        claim_fetch_failures_ = 0;
        return;
    }

    // Claimed devices are no longer part of the claim/BluFi state machine. This
    // must use IsDeviceClaimed() directly, not online_intent_, because rebooted
    // devices can recover their claimed state from backend credentials before
    // the realtime online path has started.
    if (IsDeviceClaimed()) {
        StopClaimPoll();
        StopBleAdvertising();
        return;
    }
    CancelClaimExpiryTimer();

    Settings backend_settings("backend", false);
    std::string api_url = backend_settings.GetString("api_url");

    Settings websocket_settings("websocket", false);
    std::string token = websocket_settings.GetString("bootstrap_token");
    SecureStringScope token_scope(token);
    bool paused_ble_for_fetch = false;

    if (!token.empty() && websocket_settings.GetInt("claim_ambiguous", 0) != 0) {
        claim_confirmation_ambiguous_ = true;
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        StopClaimPoll();
        StopBleAdvertising();
        ESP_LOGE(TAG, "Claim confirmation remains ambiguous; automatic backend retry suppressed");
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return;
    }

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    {
        const auto ble_state = Blufi::GetInstance().GetBleState();
        if (ble_state == Blufi::BleState::kConnected) {
            // A bootstrap token can arrive before the phone sends SSID/password.
            // The live GATT session owns the handoff regardless of token state;
            // Wi-Fi completion will release BLE and schedule the claim refresh.
            ESP_LOGI(TAG, "BLE connected; waiting for provisioning handoff to finish");
            return;
        }
        if (!pending_tbot_claim_.active &&
            ble_state == Blufi::BleState::kAdvertising && token.empty()) {
            // Physical ESP32-S3 measurements show that Wi-Fi + BluFi advertising
            // can leave too little internal SRAM for DNS/TLS. Pause advertising
            // for the unauthenticated setup-liveness fetch; its result handler
            // reopens BLE whenever the robot remains unclaimed.
            ESP_LOGI(TAG, "Pausing BLE advertising before no-token claim config fetch");
            Blufi::GetInstance().CancelBleSetupTimeout();
            StopBleAdvertising();
            paused_ble_for_fetch = true;
        } else if (!pending_tbot_claim_.active && !token.empty() &&
                   ble_state == Blufi::BleState::kAdvertising) {
            // The phone is no longer connected, so no credential handoff can be
            // interrupted. Free advertising before the TLS claim fetch/confirm.
            ESP_LOGW(TAG, "Bootstrap token present but BLE still active; stopping BLE to proceed with claim fetch/confirm");
            Blufi::GetInstance().CancelBleSetupTimeout();
            StopBleAdvertising();
            paused_ble_for_fetch = true;
        }
    }
#endif

    if (api_url.empty()) {
        // H4(1): OTA CheckVersion is the primary api_url source, but if the OTA
        // host omitted it the whole claim/heartbeat feature is silently inert.
        // Fall back to GET /v1/device/bootstrap, which always emits api_url, and
        // persist it so subsequent reads (and the heartbeat) pick it up.
        api_url = FetchBackendApiUrlFromBootstrap(token);
    }

    if (api_url.empty()) {
        // H4(2): make the dead-feature state observable instead of silently
        // skipping. Warn in the log and surface a brief Alert so it is visible.
        ESP_LOGW(TAG, "No backend api_url (OTA and bootstrap fallback both empty); "
                      "claim/heartbeat feature is inert");
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_NOT_FOUND,
              "triangle_exclamation", "");
        StopClaimPoll();
        // No backend api_url -> the claim feature is inert; we cannot reach a
        // standby state, so do not leave BLE advertising.
        StopBleAdvertising();
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_substate_ = TbotClaimSubstate::None;
        return;
    }

    if (pending_tbot_claim_.active && !token.empty()) {
        // A phone may deliver the fresh BluFi bootstrap token after we already
        // cached the backend pending claim (for example after rejecting an old
        // consumed token). Do not fetch /device/config again while BLE is up: on
        // real ESP32-S3 that BLE+TLS overlap can fail AES allocation. Stop BLE
        // first and confirm the cached claim directly.
        pending_tbot_claim_api_url_ = api_url;
        SecureClearString(pending_tbot_claim_token_);
        pending_tbot_claim_token_ = token;
        StopBleAdvertising();
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        ESP_LOGI(TAG, "Cached pending claim has BLE bootstrap token -> auto-confirming");
        ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);
        return;
    }

    if (pending_tbot_claim_.active && token.empty()) {
        ESP_LOGW(TAG, "Pending claim cached but no BLE bootstrap token yet; keeping BLE advertising");
        pending_tbot_claim_api_url_ = api_url;
        SecureClearString(pending_tbot_claim_token_);
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StopClaimPoll();
        return;
    }

    // "Hi ESP needs many tries" fix (H1/H2): the /device/config fetch below is a
    // blocking ~3s HTTP/TLS round-trip. Running it inline here executes it on the
    // priority-10 Application task (Run(): vTaskPrioritySet(nullptr, 10)) on
    // core 0, which is exactly the CPU the wake-word AFE fetch task (prio 0, no
    // affinity) and the mic FEED task (prio 8, core 0) need to detect "Hi ESP".
    // Hold that high-priority task for ~3s every 10s and the wakenet never fires /
    // the FEED ringbuffer overflows, so the user has to repeat the wake word until
    // an utterance lands in a clear gap. Move ONLY the blocking fetch onto a
    // dedicated low-priority, non-core-0-pinned worker; all state mutation stays
    // on the Application task via the Schedule()d ApplyPendingTbotClaimFetchResult
    // continuation, preserving the single-threaded claim FSM invariant (OQ1).
    if (!token.empty() && passive_ws_intent_.load()) {
        ESP_LOGI(TAG, "Bootstrap claim preempting passive lesson WebSocket");
        CloseAudioChannelByIntent();
    }
    const bool claim_fetch_dispatched =
        DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);
    if (paused_ble_for_fetch && !claim_fetch_dispatched) {
        ESP_LOGW(TAG, "Claim fetch was not dispatched; restoring BLE claim standby");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
    }
}

// Off-task continuation: runs back on the Application task (Schedule()d from
// ClaimFetchTask) so every claim_substate_/pending_tbot_claim_*/BLE/SetDeviceState
// mutation below stays serialized on the one task that owns them (OQ1). This is
// the verbatim result-handling tail of the old RefreshPendingTbotClaim(); only
// the blocking fetch moved off-task.
void Application::ApplyPendingTbotClaimFetchResult(const std::string& api_url,
                                                   const std::string& token,
                                                   const PendingTbotClaim& pending_claim,
                                                   bool fetched, int device_config_status,
                                                   bool defer_confirmation,
                                                   uint32_t expected_setup_generation,
                                                   ClaimDeferredEffects* deferred_effects) {
    auto request_ble = [this, deferred_effects](ClaimBleLifecycleIntent intent) {
        if (deferred_effects != nullptr) {
            deferred_effects->ble_intent = intent;
        } else if (intent == ClaimBleLifecycleIntent::kEnsureAdvertising) {
            EnsureBleAdvertisingForStandby();
        } else if (intent == ClaimBleLifecycleIntent::kStopAdvertising) {
            StopBleAdvertising();
        }
    };
    if (!api_url.empty()) {
        Settings backend_settings("backend", true);
        if (backend_settings.GetString("api_url") != api_url) {
            backend_settings.SetString("api_url", api_url);
        }
    }
    if (claim_confirmation_ambiguous_) {
        ESP_LOGW(TAG, "Ignoring claim fetch result while confirmation outcome is ambiguous");
        StopClaimPoll();
        return;
    }
    if (!fetched && !token.empty() &&
        (device_config_status == 401 || device_config_status == 403)) {
        // The phone/backend claim bootstrap token is single-attempt auth. If the
        // backend rejects it, retrying the same bearer only loops through BLE
        // teardown -> HTTP 401 -> "Server unavailable". Treat this as a stale
        // local token, reopen claimable BLE, and wait for the phone to deliver a
        // fresh attempt token.
        ESP_LOGW(TAG, "Device config rejected bootstrap token (HTTP %d); clearing stale claim token",
                 device_config_status);
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        Blufi::GetInstance().ClearProvisioningSecrets();
#endif
        websocket_settings.EraseKey("claim_device_id");
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_fetch_failures_ = 0;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
        StopClaimPoll();
        return;
    }
    if (!fetched || !pending_claim.active) {
        // Unowned / no pending claim yet -> enter claimable standby ("Ready to
        // connect") and run a bounded poll so a phone tap is caught without a
        // tight loop. This is the C4 "if unowned -> claimable standby" trigger.
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_ = api_url;
        SecureClearString(pending_tbot_claim_token_);
        pending_tbot_claim_token_ = token;

        // L2: distinguish a failed fetch (backend unreachable) from a successful
        // fetch that simply reports no claim. On repeated fetch failure the copy
        // becomes "Server unavailable. Retrying..." instead of the misleading
        // "Ready to connect"; a single transient miss is tolerated silently.
        const bool had_claim_fetch_failures = claim_fetch_failures_ > 0;
        if (!fetched) {
            ++claim_fetch_failures_;
        } else {
            claim_fetch_failures_ = 0;
        }
        static constexpr int kClaimFetchFailureCopyThreshold = 2;

        if (!fetched && claim_fetch_failures_ == kClaimFetchFailureCopyThreshold) {
            // Render the retry copy exactly once when the failure streak crosses
            // the threshold (the count keeps growing while it stays failing, so
            // this never re-Alerts the same banner every few seconds).
            if (claim_substate_ != TbotClaimSubstate::WaitingConfirm) {
                claim_substate_ = TbotClaimSubstate::AvailableStandby;
                Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
                      "triangle_exclamation", "");
            }
        } else if (claim_substate_ != TbotClaimSubstate::WaitingConfirm &&
                   (claim_substate_ != TbotClaimSubstate::AvailableStandby ||
                    had_claim_fetch_failures)) {
            // Render "Ready to connect" on the transition into standby, and
            // again after a fetch failure streak clears so the visible retry
            // banner is not left stale while the robot is claimable again.
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            RenderClaimSubstate(claim_substate_);
        }
        // Once the liveness fetch completes without claim auth, BLE custom-data
        // becomes the wake-up signal for the next claim refresh.
        if (token.empty()) {
            // No phone handoff is pending, so the app still needs a stable BLE
            // advertisement to discover the robot and deliver a fresh token.
            request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
            // The no-token fetch refreshed setup liveness. Keep one stable BLE
            // advertising session now; the custom-data token callback schedules
            // the next refresh directly. Repeated Bluedroid deinit/init cycles
            // can assert in vQueueDelete on the ESP32-S3.
            StopClaimPoll();
            return;
        }
        // A token-backed retry follows a completed phone handoff. BLE was
        // stopped before the TLS fetch and must remain off while backend claim
        // visibility catches up; reinitializing Bluedroid on every poll leaks
        // internal heap on ESP32-S3. A 401/403 above reopens BLE for a new token.
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - claim_poll_started_ms_ >= kClaimVisibilityRetryWindowMs) {
            ESP_LOGW(TAG, "Claim visibility retry window elapsed; clearing stale token and restoring BLE standby");
            Settings websocket_settings("websocket", true);
            websocket_settings.SetString("bootstrap_token", "");
            websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            Blufi::GetInstance().ClearProvisioningSecrets();
#endif
            websocket_settings.EraseKey("claim_device_id");
            SecureClearString(pending_tbot_claim_token_);
            pending_tbot_claim_ = PendingTbotClaim{};
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            RenderClaimSubstate(claim_substate_);
            StopClaimPoll();
            request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
            return;
        }
        if (claim_fetch_failures_ >= 4 && IsDeviceClaimed()) {
            ESP_LOGW(TAG, "claim_poll_giveup failures=%d (claimed; stop hammering backend)", claim_fetch_failures_);
            StopClaimPoll();
        } else {
            StartClaimPoll();
        }
        return;
    }

    // Fetch succeeded with an active claim -> reset the failure streak.
    claim_fetch_failures_ = 0;

    // A claim is waiting for physical confirmation.
    pending_tbot_claim_ = pending_claim;
    pending_tbot_claim_api_url_ = api_url;
    SecureClearString(pending_tbot_claim_token_);
    pending_tbot_claim_token_ = token;

    if (token.empty()) {
        // The backend can expose an active pending claim before the phone's BluFi
        // custom-data frame arrives (or after an old bootstrap token was already
        // consumed). Do NOT fall back to websocket.token here: that is the
        // realtime/OTA token, not claim-confirm auth. Keep BLE discoverable so
        // the phone can reconnect and send a fresh bootstrap token.
        ESP_LOGW(TAG, "Pending claim detected but no BLE bootstrap token yet; keeping BLE advertising");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
        StartClaimPoll();
        return;
    }

    StopClaimPoll();
    // We have the claim bootstrap token in hand; BLE discovery/custom-data has
    // served its purpose for this attempt. Stop it before TLS confirm to reduce
    // BLE+Wi-Fi heap/radio contention.
    request_ble(ClaimBleLifecycleIntent::kStopAdvertising);
    claim_substate_ = TbotClaimSubstate::WaitingConfirm;

    // NOTE: we deliberately do NOT arm the local claim-expiry timer here. We
    // auto-confirm immediately below, so the local deadline is unnecessary; and
    // at boot the local clock may be pre-SNTP / skewed, so arming it could fire a
    // spurious timeout that races the confirm. The backend window is authoritative.

    // TBOT product decision: SKIP the press-to-allow step. The pending claim was
    // surfaced by our OWN authenticated /device/config poll, which already proves
    // this is the physical robot the app is claiming — so confirm it AUTOMATICALLY
    // instead of waiting for a BOOT-button press (which had too short / racy a
    // window in practice). ConfirmPendingTbotClaim() runs on this (App) task and
    // makes a blocking ~5s POST /claim/confirm inline. This is deliberately kept
    // ON-task (unlike the config fetch and heartbeat, which were moved to
    // low-priority workers): it is a ONE-SHOT call in the boot claim-confirm
    // window, not a periodic timer tick, and at this point the device is still
    // UNCLAIMED so the wake-word AFE mic is disabled (see the IsDeviceClaimed()-
    // gated Idle gate) — there is no wake-word pipeline to starve until this very
    // call succeeds and turns the mic on. It also owns the synchronous result
    // policy: retryable failures retain this claim/token and re-arm the bounded
    // poll, while terminal rejection clears the attempt and reopens BLE. Making
    // it async would split those state transitions across callers. The
    // BOOT-button path stays wired as a manual fallback if a future build wants
    // to re-enable explicit consent.
    ESP_LOGI(TAG, "Pending claim detected -> auto-confirming (press-to-allow skipped by product decision)");
    if (defer_confirmation) {
        if (deferred_effects != nullptr) {
            deferred_effects->dispatch_confirmation = true;
        } else if (!DispatchPendingTbotClaimConfirmation(
                       expected_setup_generation, true)) {
            StartClaimPoll();
        }
    } else {
        ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);
    }
}

bool Application::ConfirmPendingTbotClaim(bool trust_backend_expiry) {
    if (!pending_tbot_claim_.active) {
        return false;
    }

    if (claim_confirmation_ambiguous_) {
        ESP_LOGW(TAG, "Claim confirmation outcome ambiguous; automatic retry suppressed");
        StopClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    if (pending_tbot_claim_token_.empty()) {
        ESP_LOGW(TAG, "Claim confirm deferred: missing BLE bootstrap token");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
        return true;
    }

    // Reject a tap on an already-expired window rather than confirm blind.
    // trust_backend_expiry skips this when the caller (auto-confirm) just got the
    // claim from the backend poll, which only returns claims with expires_at >
    // NOW() on the SERVER clock — authoritative. The local clock can be pre-SNTP /
    // skewed at boot and previously tripped a false "window expired" here.
    if (!trust_backend_expiry && IsPendingTbotClaimExpired(pending_tbot_claim_, time(nullptr))) {
        ESP_LOGW(TAG, "Claim confirm ignored: window expired");
        HandleClaimConfirmTimeout();
        return true;
    }

    WakeWordLifecycleController::ProvisioningToken provisioning_token{};
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    // Capture the provisioning session before the confirm request so a
    // success-path teardown can only consume this exact originating token.
    provisioning_token = Blufi::GetInstance().CaptureProvisioningSession();
#endif
    const ClaimConfirmationResult confirmation_result = ClaimConfirmationReporter::Confirm(
        pending_tbot_claim_,
        pending_tbot_claim_api_url_, pending_tbot_claim_token_);
    return ApplyPendingTbotClaimConfirmationResult(confirmation_result, provisioning_token);
}

bool Application::ApplyPendingTbotClaimConfirmationResult(
    ClaimConfirmationResult confirmation_result,
    WakeWordLifecycleController::ProvisioningToken provisioning_token,
    bool defer_successful_teardown,
    ClaimDeferredEffects* deferred_effects) {
    if (confirmation_result == ClaimConfirmationResult::RetryableFailure) {
        ESP_LOGW(TAG, "Claim confirmation retryable; retaining claim token for bounded retry");
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        StartClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
              "triangle_exclamation", "");
        return true;
    }

    if (confirmation_result == ClaimConfirmationResult::AmbiguousSuccess) {
        ESP_LOGE(TAG, "Claim confirmation 2xx response unusable; freezing attempt for support/reset");
        Settings websocket_settings("websocket", true);
        websocket_settings.SetInt("claim_ambiguous", 1);
        claim_confirmation_ambiguous_ = true;
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        StopClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    if (confirmation_result == ClaimConfirmationResult::TerminalFailure) {
        CancelClaimExpiryTimer();
        StopClaimPoll();
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        // The Application owns the terminal NVS clear; BluFi owns zeroization of
        // the in-RAM token/code copies received from the phone.
        Blufi::GetInstance().ClearProvisioningSecrets();
#endif
        websocket_settings.EraseKey("claim_device_id");
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        if (deferred_effects != nullptr) {
            deferred_effects->ble_intent = ClaimBleLifecycleIntent::kEnsureAdvertising;
        } else {
            EnsureBleAdvertisingForStandby();
        }
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTION_CONFIRM_FAILED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    CancelClaimExpiryTimer();
    StopClaimPoll();
    // Claim confirmed -> the device is becoming claimed. Stop advertising for
    // pairing; an owned robot must not be BLE-discoverable for a new claim.
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    if (defer_successful_teardown && deferred_effects != nullptr) {
        deferred_effects->ble_intent = ClaimBleLifecycleIntent::kCompleteSuccessfulTeardown;
    } else if (!defer_successful_teardown) {
        Blufi::GetInstance().CompleteSuccessfulProvisioningTeardown(
            "claim_confirmed", provisioning_token);
    }
#else
    if (defer_successful_teardown && deferred_effects != nullptr) {
        deferred_effects->ble_intent = ClaimBleLifecycleIntent::kStopAdvertising;
    } else if (!defer_successful_teardown) {
        StopBleAdvertising();
    }
#endif
    if (protocol_) {
        CloseAudioChannelByIntent();
    }
    // The bootstrap token is single-attempt claim auth. Device credentials are
    // already persisted by the reporter; clear the consumed token so a reboot
    // never polls /device/config with stale Authorization before WS comes up.
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
    }
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    Blufi::GetInstance().ClearProvisioningSecrets();
#endif
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.EraseKey("claim_device_id");
    }
    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    SecureClearString(pending_tbot_claim_token_);
    claim_confirmation_ambiguous_ = false;
    claim_substate_ = TbotClaimSubstate::Confirmed;

    if (!FinishClaimActivationAfterLocalAssetsReady()) {
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        ScheduleClaimLocalAssetsRetry();
        return true;
    }

    return true;
}

namespace {
struct ClaimConfirmationContext {
    Application* app;
    PendingTbotClaim claim;
    std::string api_url;
    std::string token;
    WakeWordLifecycleController::ProvisioningToken provisioning_token;
    uint32_t expected_setup_generation;
    bool enforce_setup_generation;
};
}  // namespace

bool Application::DispatchPendingTbotClaimConfirmation(
    uint32_t expected_setup_generation, bool enforce_setup_generation) {
    if (!pending_tbot_claim_.active || pending_tbot_claim_token_.empty()) {
        return false;
    }
    bool expected = false;
    if (!claim_confirm_inflight_.compare_exchange_strong(expected, true)) {
        return false;
    }
    WakeWordLifecycleController::ProvisioningToken provisioning_token{};
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    provisioning_token = Blufi::GetInstance().CaptureProvisioningSession();
#endif
    auto* ctx = new (std::nothrow) ClaimConfirmationContext{
        this, pending_tbot_claim_, pending_tbot_claim_api_url_,
        pending_tbot_claim_token_, provisioning_token, expected_setup_generation,
        enforce_setup_generation};
    if (ctx == nullptr) {
        claim_confirm_inflight_.store(false);
        return false;
    }
    if (xTaskCreateWithCaps(&Application::ClaimConfirmationTask, "claim_confirm", 8192, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        SecureClearString(ctx->token);
        delete ctx;
        claim_confirm_inflight_.store(false);
        return false;
    }
    return true;
}

void Application::ClaimConfirmationTask(void* arg) {
    {
        auto* ctx = static_cast<ClaimConfirmationContext*>(arg);
        Application* self = ctx->app;
        PendingTbotClaim claim = ctx->claim;
        std::string api_url = std::move(ctx->api_url);
        std::string token = std::move(ctx->token);
        const auto provisioning_token = ctx->provisioning_token;
        const uint32_t expected_setup_generation = ctx->expected_setup_generation;
        const bool enforce_setup_generation = ctx->enforce_setup_generation;
        delete ctx;

        std::string success_response;
        const ClaimConfirmationResult result = ClaimConfirmationReporter::Confirm(
            claim, api_url, token, &success_response);
        self->Schedule([self, result, token = std::move(token), provisioning_token,
                        success_response = std::move(success_response),
                        expected_setup_generation, enforce_setup_generation]() mutable {
            self->claim_confirm_inflight_.store(false);
            ClaimDeferredEffects deferred_effects;
            auto apply_result = [&](bool defer_successful_teardown,
                                    ClaimDeferredEffects* effects) {
                ClaimConfirmationResult effective_result = result;
                if (effective_result == ClaimConfirmationResult::Confirmed &&
                    !PersistTbotClaimConfirmationResponse(success_response)) {
                    effective_result = ClaimConfirmationResult::AmbiguousSuccess;
                }
                self->ApplyPendingTbotClaimConfirmationResult(
                    effective_result, provisioning_token, defer_successful_teardown, effects);
                return effective_result == ClaimConfirmationResult::Confirmed;
            };
            if (enforce_setup_generation) {
                const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
                    expected_setup_generation, [&]() {
                        apply_result(true, &deferred_effects);
                    });
                if (applied) {
                    self->ExecuteClaimDeferredEffects(
                        deferred_effects, expected_setup_generation, provisioning_token);
                }
            } else {
                apply_result(false, nullptr);
            }
            SecureClearString(token);
            SecureClearString(success_response);
        });
        SecureClearString(token);
        SecureClearString(success_response);
    }
    vTaskDeleteWithCaps(nullptr);
}

void Application::SchedulePendingTbotClaimRefresh(uint32_t expected_setup_generation) {
    Schedule([this, expected_setup_generation]() {
        ClaimDeferredEffects deferred_effects;
        const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
            expected_setup_generation, [this, &deferred_effects]() {
            // Serialize setup promotion and snapshot/dispatch with BOOT re-entry.
            PromoteFromWifiConfigAfterProvisioning();
            if (GetDeviceState() != kDeviceStateWifiConfiguring) {
                deferred_effects.dispatch_refresh = true;
            }
            // If activation remains in progress, HandleActivationDoneEvent
            // performs the normal refresh after reaching Idle.
            });
        if (applied) {
            ExecuteClaimDeferredEffects(deferred_effects, expected_setup_generation);
        }
    });
}

void Application::CompleteCardputerWifiProvisioning(uint64_t ui_generation) {
    uint64_t completed =
        cardputer_wifi_completion_generation_.load(std::memory_order_acquire);
    if (ui_generation <= completed ||
        GetDeviceState() != kDeviceStateWifiConfiguring) {
        return;
    }
    PromoteFromWifiConfigAfterProvisioning();
    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        return;
    }
    while (ui_generation > completed &&
           !cardputer_wifi_completion_generation_.compare_exchange_weak(
               completed, ui_generation, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
    HandleNetworkConnectedEvent();
}

void Application::ExecuteClaimDeferredEffects(
        const ClaimDeferredEffects& effects, uint32_t expected_setup_generation,
        WakeWordLifecycleController::ProvisioningToken provisioning_token) {
    auto commit_dispatch = [this, &effects, expected_setup_generation]() {
        if (effects.dispatch_confirmation &&
            !DispatchPendingTbotClaimConfirmation(expected_setup_generation, true)) {
            StartClaimPoll();
        }
        if (effects.restore_standby_after_dispatch_failure) {
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            RenderClaimSubstate(claim_substate_);
            StartClaimPoll();
        }
    };
    bool lifecycle_ready = true;
    switch (effects.ble_intent) {
        case ClaimBleLifecycleIntent::kNone:
            lifecycle_ready = RunClaimDispatchForSetupGeneration(
                expected_setup_generation, commit_dispatch);
            break;
        case ClaimBleLifecycleIntent::kEnsureAdvertising:
            lifecycle_ready = EnsureBleAdvertisingForStandbyForSetupGeneration(
                expected_setup_generation, commit_dispatch);
            break;
        case ClaimBleLifecycleIntent::kStopAdvertising:
            lifecycle_ready = StopBleAdvertisingForSetupGeneration(
                expected_setup_generation, commit_dispatch);
            break;
        case ClaimBleLifecycleIntent::kCompleteSuccessfulTeardown:
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            lifecycle_ready = Blufi::GetInstance()
                .CompleteSuccessfulProvisioningTeardownForGeneration(
                    "claim_confirmed", provisioning_token, expected_setup_generation,
                    commit_dispatch);
#else
            lifecycle_ready = StopBleAdvertisingForSetupGeneration(
                expected_setup_generation, commit_dispatch);
#endif
            break;
    }

    if (!lifecycle_ready) {
        return;
    }
    if (effects.dispatch_refresh) {
        DispatchPendingTbotClaimRefreshForSetupGeneration(expected_setup_generation);
    }
}

bool Application::RunClaimDispatchForSetupGeneration(
        uint32_t expected_setup_generation, const std::function<void()>& action) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    return Blufi::GetInstance().RunWithSetupGenerationCurrent(
        expected_setup_generation, action);
#else
    (void)expected_setup_generation;
    action();
    return true;
#endif
}

void Application::DispatchPendingTbotClaimRefreshForSetupGeneration(
    uint32_t expected_setup_generation) {
    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    Settings websocket_settings("websocket", false);
    std::string token = websocket_settings.GetString("bootstrap_token");
    SecureStringScope token_scope(token);

    if (pending_tbot_claim_.active && !token.empty()) {
        ClaimDeferredEffects effects;
        const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
            expected_setup_generation, [&]() {
                if (!api_url.empty()) {
                    pending_tbot_claim_api_url_ = api_url;
                }
                SecureClearString(pending_tbot_claim_token_);
                pending_tbot_claim_token_ = token;
                claim_substate_ = TbotClaimSubstate::WaitingConfirm;
                effects.ble_intent = ClaimBleLifecycleIntent::kStopAdvertising;
                effects.dispatch_confirmation = true;
            });
        if (applied) {
            ExecuteClaimDeferredEffects(effects, expected_setup_generation);
        }
        return;
    }

    bool dispatched = false;
    const bool current = RunClaimDispatchForSetupGeneration(
        expected_setup_generation, [&]() {
            dispatched = DispatchPendingTbotClaimFetch(
                api_url, token, true, expected_setup_generation, true);
        });
    if (!current) {
        return;
    }
    if (dispatched) {
        if (!token.empty() && passive_ws_intent_.load()) {
            ESP_LOGI(TAG, "Provisioning claim preempting passive lesson WebSocket");
            CloseAudioChannelByIntent();
        }
    }
    if (!dispatched) {
        ClaimDeferredEffects effects;
        effects.ble_intent = ClaimBleLifecycleIntent::kEnsureAdvertising;
        effects.restore_standby_after_dispatch_failure = true;
        ExecuteClaimDeferredEffects(effects, expected_setup_generation);
    }
}

void Application::PromoteFromWifiConfigAfterProvisioning() {
    // BluFi reported STA-connected success. Unlike a stale STA event, this is a
    // real provisioning completion, so leave WiFi-config mode and run the normal
    // activation->Idle path. RefreshPendingTbotClaim (which auto-confirms the
    // pending claim) only runs once we are OUT of kDeviceStateWifiConfiguring.
    if (GetDeviceState() != kDeviceStateWifiConfiguring) {
        return;  // Already promoted / not in setup -> let the normal path run.
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        return;  // Success report was stale; stay in setup.
    }
    if (!SetDeviceState(kDeviceStateActivating)) {
        return;  // FSM rejected the transition; nothing more to do here.
    }
    if (!IsDeviceClaimed()) {
        // Same heap constraint as HandleNetworkConnectedEvent: with BLE still
        // advertising for claim standby, the 8KB activation task often cannot be
        // created. Run only the protocol setup needed for public lesson sync.
        ESP_LOGW(TAG,
                 "Unclaimed after BluFi Wi-Fi success: run minimal activation "
                 "transport inline");
        CompleteUnclaimedProtocolOnlyActivation();
        return;
    }

    ESP_LOGI(TAG, "Claimed after BluFi Wi-Fi success: run lightweight activation");
    CompleteClaimedWifiReprovisionActivation();
}

void Application::PromoteCourseModeFromWifiConfigAfterProvisioning() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    PromoteFromWifiConfigAfterProvisioning();
#else
    return;
#endif
}

void Application::CompleteUnclaimedProtocolOnlyActivation() {
    if (!ota_) {
        ota_ = std::make_unique<Ota>();
    }
    ota_->MarkCurrentVersionValid();

    RequestInitializeProtocol(ProtocolActivation::kNormal);
}

void Application::CompleteClaimedWifiReprovisionActivation() {
    if (!ota_) {
        ota_ = std::make_unique<Ota>();
    }
    ota_->MarkCurrentVersionValid();

    // Normal Wi-Fi changes reuse the realtime token from the previous online
    // session. Recovery pairing can legitimately arrive here without one (for
    // example after an operator releases stale cloud ownership), so refresh the
    // signed runtime config before opening the WebSocket.
    Settings websocket_settings("websocket", false);
    if (websocket_settings.GetString("token").empty()) {
        const esp_err_t refresh_result = ota_->CheckVersion();
        if (refresh_result != ESP_OK) {
            ESP_LOGW(TAG, "WebSocket config refresh after WiFi provisioning failed: 0x%x",
                     refresh_result);
        }
    }

    RequestInitializeProtocol(ProtocolActivation::kWifiReprovision);
}

bool Application::EnsureLocalAssetsAppliedForClaim() {
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return true;
    }

    return assets.Apply(false);
}

bool Application::FinishClaimActivationAfterLocalAssetsReady() {
    if (!IsDeviceClaimed()) {
        return false;
    }
    if (!EnsureLocalAssetsAppliedForClaim()) {
        ESP_LOGE(TAG, "Claim confirmed but local assets/models are not ready; retrying locally");
        return false;
    }
    if (claim_assets_retry_timer_ != nullptr) {
        esp_timer_stop(claim_assets_retry_timer_);
    }

    claim_protocol_completion_pending_ = true;
    ReloadProtocolAfterClaimCredentials();
    return true;
}

void Application::CompleteClaimProtocolActivation() {
    // TBOT claim complete -> refresh the protocol with claimed credentials, then
    // return to explicit wake standby. InitializeProtocol opens only the passive
    // lesson/nudge WebSocket for claimed idle devices.
    if (!audio_service_.Start()) {
        ESP_LOGE(TAG, "Claim activation audio startup failed; retry remains pending");
        ScheduleClaimLocalAssetsRetry();
        return;
    }
    SetDeviceState(kDeviceStateIdle);
    if (!lesson_asset_sync_quiet_.load()) {
        audio_service_.EnableWakeWordDetection(true);
    }
    StartHeartbeat();
    DispatchDeviceHeartbeat();
    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTED, "link", Lang::Sounds::OGG_SUCCESS);
}

void Application::ScheduleClaimLocalAssetsRetry() {
    if (claim_assets_retry_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->HandleClaimLocalAssetsRetry(); });
        };
        args.arg = this;
        args.name = "claim_assets_retry";
        if (esp_timer_create(&args, &claim_assets_retry_timer_) != ESP_OK) {
            claim_assets_retry_timer_ = nullptr;
            return;
        }
    }
    esp_timer_stop(claim_assets_retry_timer_);
    esp_timer_start_once(claim_assets_retry_timer_, 2000ULL * 1000ULL);
}

void Application::HandleClaimLocalAssetsRetry() {
    if (!IsDeviceClaimed()) {
        return;
    }
    if (FinishClaimActivationAfterLocalAssetsReady()) {
        return;
    }
    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
          "triangle_exclamation", "");
    ScheduleClaimLocalAssetsRetry();
}

void Application::ReloadProtocolAfterClaimCredentials() {
    CloseAudioChannelByIntent();
    RequestInitializeProtocol();
}

void Application::RenderClaimSubstate(TbotClaimSubstate substate) {
    // The connect-state contract row is the source of truth for which state we
    // are in; the on-screen copy is the localized Lang::Strings equivalent so
    // the VI build shows translated text (English literals live in the table).
    const TbotConnectState state = TbotConnectMapper::ResolveState(
        GetDeviceState(), substate, GetBleSubstate());
    const char* copy = TbotConnectMapper::ScreenTextFor(state);
    switch (state) {
        case TbotConnectState::CLAIM_AVAILABLE:
            copy = Lang::Strings::READY_TO_CONNECT;
            break;
        case TbotConnectState::CLAIM_WAITING_CONFIRM:
            copy = Lang::Strings::PRESS_BUTTON_TO_CONFIRM;
            break;
        case TbotConnectState::CLAIM_CONFIRM_TIMEOUT:
            copy = Lang::Strings::SETUP_EXPIRED;
            break;
        default:
            break;  // fall back to the contract text
    }
    Alert(Lang::Strings::TBOT_CONNECT, copy, "link", "");
}

// ---------------------------------------------------------------------------
// Off-task claim-config fetch ("Hi ESP needs many tries" fix)
// ---------------------------------------------------------------------------

namespace {
// Heap-owned hand-off for the off-task claim fetch worker (mirrors ConnectContext).
struct ClaimFetchContext {
    Application* app;
    std::string api_url;
    std::string token;
    bool apply_when_poll_inactive;
    uint32_t expected_setup_generation;
    bool enforce_setup_generation;
};
}  // namespace

bool Application::DispatchPendingTbotClaimFetch(const std::string& api_url,
                                                const std::string& token,
                                                bool apply_when_poll_inactive,
                                                uint32_t expected_setup_generation,
                                                bool enforce_setup_generation) {
    // Runs on the Application task. Belt-and-suspenders gating (fix 2): never
    // kick off a blocking TLS handshake while live realtime audio is in flight —
    // a wake/connect/listen/speak must always win the radio + CPU. On skip we do
    // nothing and let the next periodic tick retry; we do NOT StopClaimPoll or
    // reset the window, so an unclaimed device keeps discovering a phone claim and
    // the 5-minute confirm cap (PollPendingTbotClaimTick) stays intact.
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateConnecting ||
        state == kDeviceStateListening ||
        state == kDeviceStateSpeaking ||
        connect_in_flight_.load()) {
        ESP_LOGD(TAG, "Skipping claim fetch this tick (runtime audio active)");
        return false;
    }

    // Single-flight: a slow backend must never let the 10s timer stack workers.
    bool expected = false;
    if (!claim_poll_inflight_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Claim fetch already in flight; skipping this tick");
        return false;
    }

    auto* ctx = new (std::nothrow) ClaimFetchContext{
        this, api_url, token, apply_when_poll_inactive,
        expected_setup_generation, enforce_setup_generation};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "claim_fetch context allocation failed; restoring standby");
        claim_poll_inflight_.store(false);
        return false;
    }
    // Low priority (tskIDLE_PRIORITY+1) and NOT pinned to core 0 so the worker
    // simply WAITS on the network at low priority while the wake-word AFE
    // fetch/feed pipeline keeps the CPU. The old design queued this blocking call
    // onto the priority-10 Application task, which is the starvation root cause.
    //
    // Stack MUST be internal DRAM — not SPIRAM. The worker opens NVS + does
    // TLS/HTTP; both disable the flash cache. A SPIRAM task stack is invalid
    // while the cache is off and panics with:
    //   esp_task_stack_is_sane_cache_disabled (spi_flash cache_utils).
    // Live crash after BluFi Wi-Fi success was exactly this path.
    if (xTaskCreateWithCaps(&Application::ClaimFetchTask, "claim_fetch", 6144, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "claim_fetch task create failed; retrying next tick");
        SecureClearString(ctx->token);
        delete ctx;
        claim_poll_inflight_.store(false);
        return false;
    }
    return true;
}

void Application::ClaimFetchTask(void* arg) {
    {
        auto* ctx = static_cast<ClaimFetchContext*>(arg);
        Application* self = ctx->app;
        std::string api_url = ctx->api_url;
        std::string token = ctx->token;
        const bool apply_when_poll_inactive = ctx->apply_when_poll_inactive;
        const uint32_t expected_setup_generation = ctx->expected_setup_generation;
        const bool enforce_setup_generation = ctx->enforce_setup_generation;
        SecureClearString(ctx->token);
        delete ctx;

        // The ONLY work on this worker: the blocking ~3s HTTP/TLS fetch. No shared
        // state is touched here.
        if (api_url.empty()) {
            api_url = FetchBackendApiUrlFromBootstrap(token, false);
        }
        PendingTbotClaim pending_claim;
        int device_config_status = 0;
        const bool fetched = !api_url.empty() &&
            FetchPendingTbotClaimFromDeviceConfig(api_url, token, pending_claim,
                                                  &device_config_status);

        // Marshal result-application back onto the Application task (OQ1): all
        // claim_substate_/pending_tbot_claim_*/BLE/SetDeviceState mutation stays on
        // the one task that owns them. Clear the single-flight guard there so the
        // next tick can dispatch again.
        self->Schedule([self, api_url, token, pending_claim, fetched, device_config_status,
                        apply_when_poll_inactive, expected_setup_generation,
                        enforce_setup_generation]() mutable {
            self->claim_poll_inflight_.store(false);
            // The periodic poll may have been stopped (claimed+online / WiFiConfiguring)
            // while this fetch was outstanding; honor that and drop pure poll ticks
            // that finished after stop.
            //
            // One-shot fetches (post-BluFi / unclaimed boot with bootstrap token)
            // dispatch BEFORE StartClaimPoll() is active. Those must ALWAYS apply:
            //  - active claim + token -> auto-confirm
            //  - claim_present=0 + token -> Apply... reopens BLE standby
            // Without the claim_present=0 branch, BLE stays down after the
            // "Bootstrap token present; stopping BLE" path and phone scan times out
            // (live E2E 2026-07-11: BLE_SCAN_TIMEOUT after claim_fetch http=200
            // claim_present=0 with no EnsureBleAdvertisingForStandby).
            if (!self->claim_poll_active_ && token.empty() && !apply_when_poll_inactive) {
                SecureClearString(token);
                return;
            }
            ClaimDeferredEffects deferred_effects;
            auto apply_result = [&]() {
                self->ApplyPendingTbotClaimFetchResult(
                    api_url, token, pending_claim, fetched, device_config_status,
                    enforce_setup_generation, expected_setup_generation, &deferred_effects);
            };
            if (enforce_setup_generation) {
                const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
                    expected_setup_generation, apply_result);
                if (applied) {
                    self->ExecuteClaimDeferredEffects(
                        deferred_effects, expected_setup_generation);
                }
            } else {
                self->ApplyPendingTbotClaimFetchResult(
                    api_url, token, pending_claim, fetched, device_config_status,
                    false, expected_setup_generation, nullptr);
            }
            SecureClearString(token);
        });
        SecureClearString(token);
    }
    vTaskDeleteWithCaps(nullptr);
}

// ---------------------------------------------------------------------------
// Bounded claim poll (C4)
// ---------------------------------------------------------------------------

void Application::StartClaimPoll() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    if (online_intent_.load() && IsDeviceClaimed()) {
        return;  // Claimed + online; never re-arm the blocking claim backend poll.
    }
    // Fix 3: once the realtime WS is up (online_intent_) the device is fully
    // functional, so the claim poll is pure background — back it off to 60s so it
    // can never materially starve audio. Offline / mid-confirm keeps the 10s
    // cadence so a phone claim is discovered promptly. We never fully kill the
    // poll for an unclaimed device (must keep discovering it got claimed).
    const uint64_t desired_interval_us =
        online_intent_.load() ? kClaimPollIntervalIdleUs : kClaimPollIntervalUs;
    if (claim_poll_active_) {
        if (desired_interval_us == claim_poll_interval_us_ || claim_poll_timer_ == nullptr) {
            return;  // Already polling this window at the right cadence.
        }
        // Cadence changed (e.g. WS just came up) -> re-arm at the new interval
        // without resetting the 5-minute confirm-window start.
        esp_timer_stop(claim_poll_timer_);
        claim_poll_interval_us_ = desired_interval_us;
        esp_timer_start_periodic(claim_poll_timer_, claim_poll_interval_us_);
        ESP_LOGI(TAG, "Claim poll re-armed (every %lus)",
                 static_cast<unsigned long>(claim_poll_interval_us_ / 1000000ULL));
        return;
    }
    if (claim_poll_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                // Never do network I/O in the timer task — post to Application.
                app->Schedule([app]() { app->PollPendingTbotClaimTick(); });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "claim_poll",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &claim_poll_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create claim poll timer");
            claim_poll_timer_ = nullptr;
            return;
        }
    }
    claim_poll_started_ms_ = esp_timer_get_time() / 1000;
    claim_poll_active_ = true;
    claim_poll_interval_us_ = desired_interval_us;
    esp_timer_start_periodic(claim_poll_timer_, claim_poll_interval_us_);
    ESP_LOGI(TAG, "Claim poll started (every %lus, %lds cap)",
             static_cast<unsigned long>(claim_poll_interval_us_ / 1000000ULL),
             static_cast<long>(kClaimPollWindowMs / 1000));
#endif
}

void Application::StopClaimPoll() {
    if (claim_poll_timer_ != nullptr && claim_poll_active_) {
        esp_timer_stop(claim_poll_timer_);
    }
    claim_poll_active_ = false;
}

void Application::PollPendingTbotClaimTick() {
    if (!claim_poll_active_) {
        return;
    }

    // Respect the 5-minute window cap only for an active backend claim-confirm
    // window. Mere unclaimed standby is intentionally long-lived: the phone may
    // scan after the robot has been sitting ready for more than five minutes.
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - claim_poll_started_ms_ >= kClaimPollWindowMs) {
        if (!pending_tbot_claim_.active) {
            ESP_LOGI(TAG, "Claim standby poll window elapsed; continuing BLE advertising");
            claim_poll_started_ms_ = now_ms;
            RefreshPendingTbotClaim();
            return;
        }

        ESP_LOGW(TAG, "Claim confirm window elapsed -> CLAIM_CONFIRM_TIMEOUT");
        StopClaimPoll();
        HandleClaimConfirmTimeout();
        return;
    }

    // Re-fetch /device/config; RefreshPendingTbotClaim() handles the result
    // (promote to WaitingConfirm, stay in standby, or re-arm the poll).
    RefreshPendingTbotClaim();
}

// ---------------------------------------------------------------------------
// Local claim-expiry deadline (C4)
// ---------------------------------------------------------------------------

void Application::ArmClaimExpiryTimer() {
    CancelClaimExpiryTimer();

    time_t expires_epoch = 0;
    if (!ParseIso8601UtcToEpoch(pending_tbot_claim_.expires_at, expires_epoch)) {
        ESP_LOGW(TAG, "Pending claim has no parseable expires_at; relying on poll cap");
        return;
    }

    const time_t now = time(nullptr);
    // M3: arming a wall-clock deadline is only meaningful once the clock is real.
    // Without server_time (or before a 2024-01-01 sanity floor) time() can read
    // ~1970, which would arm the one-shot decades out. Skip arming and lean on
    // the bounded poll's 5-minute window cap, which is monotonic and correct.
    static constexpr time_t kClockSanityFloor = 1704067200;  // 2024-01-01T00:00:00Z
    if (!has_server_time_ && now < kClockSanityFloor) {
        ESP_LOGI(TAG, "Clock unsynced; not arming wall-clock claim expiry, relying on poll-window cap");
        return;
    }
    int64_t remaining_s = static_cast<int64_t>(expires_epoch) - static_cast<int64_t>(now);
    if (remaining_s <= 0) {
        // Already expired -> surface immediately on the Application task.
        Schedule([this]() { HandleClaimConfirmTimeout(); });
        return;
    }

    esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->Schedule([app]() { app->HandleClaimConfirmTimeout(); });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "claim_expiry",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &claim_expiry_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create claim expiry timer");
        claim_expiry_timer_ = nullptr;
        return;
    }
    esp_timer_start_once(claim_expiry_timer_, static_cast<uint64_t>(remaining_s) * 1000000ULL);
    ESP_LOGI(TAG, "Claim expiry armed in %lds", static_cast<long>(remaining_s));
}

void Application::CancelClaimExpiryTimer() {
    if (claim_expiry_timer_ != nullptr) {
        esp_timer_stop(claim_expiry_timer_);
        esp_timer_delete(claim_expiry_timer_);
        claim_expiry_timer_ = nullptr;
    }
}

void Application::HandleClaimConfirmTimeout() {
    CancelClaimExpiryTimer();
    StopClaimPoll();
    // Leaving claimable standby (window elapsed) -> stop advertising for pairing.
    StopBleAdvertising();
    Settings websocket_settings("websocket", true);
    websocket_settings.SetString("bootstrap_token", "");
    websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    Blufi::GetInstance().ClearProvisioningSecrets();
#endif
    websocket_settings.EraseKey("claim_device_id");
    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    SecureClearString(pending_tbot_claim_token_);
    claim_confirmation_ambiguous_ = false;
    claim_substate_ = TbotClaimSubstate::ConfirmTimeout;
    // "Setup expired" (CLAIM_CONFIRM_TIMEOUT) — no silent failure, no spinner.
    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SETUP_EXPIRED,
          "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
}

// ---------------------------------------------------------------------------
// BLE discoverability for unclaimed standby / explicit setup
//
// The mobile app discovers robots by BLE scan, matching the BluFi advertisement
// name "TBOT-<MAC>". Keep that advertisement available for unclaimed claimable
// standby. A claimed online robot uses the BOOT Wi-Fi-config path to reopen
// BluFi; keeping BLE always on while AFE wake-word audio runs can destabilize
// the realtime websocket path on the ESP32-S3.
//
// init() coexists with the connected Wi-Fi station: it does NOT stop the STA
// (it only kicks a dedicated, non-disruptive scan), and BT/Wi-Fi software
// coexistence is enabled (CONFIG_SW_COEXIST_ENABLE / CONFIG_ESP_COEX_SW_COEXIST
// _ENABLE + CONFIG_BT_BLUEDROID_ESP_COEX_VSC), so advertising while online is
// safe for setup windows. Normal claimed ONLINE suppresses BLE in
// RefreshPendingTbotClaim() and uses EnterWifiConfigMode() to reopen it.
// ---------------------------------------------------------------------------

bool Application::HasStaleRevokedClaimIdentity() const {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return false;
#else
    Settings claim_state("tbot_claim", false);
    Settings backend_settings("backend", false);
    const bool claim_confirmed = claim_state.GetInt("confirmed", 0) != 0;
    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
    return !claim_confirmed && !device_id.empty() && device_secret.empty();
#endif
}

bool Application::IsDeviceClaimed() const {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return true;
#else
    // Primary claimed-signal: a DEDICATED flag written ONLY by a successful
    // physical-claim confirm (PersistTbotClaimConfirmationResponse). Recovery
    // signal: backend device_id + device_secret are also written only by that
    // successful confirm, so after a reboot/marker loss they are enough to keep
    // the robot on the claimed online path. We must NOT key off websocket
    // "token": OTA CheckVersion can write that realtime-WS token on every boot.
    Settings backend_settings("backend", false);
    if (backend_settings.GetInt("release_pending", 0) != 0) {
        // Credentials are retained only so the deferred ownership release can
        // authenticate. They must not start claimed runtime/audio while the
        // robot is rebooting into Wi-Fi provisioning.
        return false;
    }

    Settings claim_state("tbot_claim", false);
    const bool claim_confirmed = claim_state.GetInt("confirmed", 0) != 0;

    Settings websocket_settings("websocket", false);
    const std::string websocket_token = websocket_settings.GetString("token");
    const bool factory_test_claimed = claim_state.GetInt("factory_test", 0) != 0;
    if (factory_test_claimed && !websocket_token.empty()) {
        return true;
    }

    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (claim_confirmed && (device_id.empty() || device_secret.empty())) {
        ESP_LOGW(TAG, "Ignoring stale claim marker without complete backend credentials");
    }
    return !device_id.empty() && !device_secret.empty();
#endif
}

void Application::EnsureBleAdvertisingForStandby() {
    EnsureBleAdvertisingForStandbyImpl(std::nullopt);
}

bool Application::EnsureBleAdvertisingForStandbyForSetupGeneration(
        uint32_t expected_generation, const std::function<void()>& on_current) {
    return EnsureBleAdvertisingForStandbyImpl(expected_generation, on_current);
}

bool Application::EnsureBleAdvertisingForStandbyImpl(
        std::optional<uint32_t> expected_generation,
        const std::function<void()>& on_current) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();

    if (IsDeviceClaimed()) {
        return expected_generation.has_value()
            ? StopBleAdvertisingForSetupGeneration(
                  expected_generation.value(), on_current)
            : StopBleAdvertisingImpl(std::nullopt);
    }

    auto provisioning_token = blufi.CaptureProvisioningSession();
    auto prepare = [&]() -> esp_err_t {
        if (provisioning_token.valid()) {
            return ESP_OK;
        }
        auto provisioning_reservation = blufi.TryReserveProvisioningSession();
        if (!provisioning_reservation) {
            ESP_LOGW(TAG, "Claim standby BLE start deferred: provisioning completion active");
            return ESP_ERR_INVALID_STATE;
        }
        const auto begin_result = audio_service_.BeginWifiProvisioning();
        if (!begin_result) {
            ESP_LOGE(TAG, "Claim standby BLE start failed: audio lifecycle did not quiesce");
            return ESP_FAIL;
        }
        provisioning_token = begin_result.token;
        if (!provisioning_reservation.Commit(provisioning_token)) {
            ESP_LOGE(TAG, "Claim standby BLE start failed: could not bind provisioning token");
            audio_service_.EndWifiProvisioningAndRearm(provisioning_token);
            provisioning_token = {};
            return ESP_FAIL;
        }
        return ESP_OK;
    };
    if (expected_generation.has_value()) {
        const bool ensured = blufi.EnsureAdvertisingForSetupGeneration(
            expected_generation.value(), CONFIG_BLE_SETUP_TIMEOUT_SEC,
            &provisioning_token, prepare, on_current);
        if (!ensured) {
            ESP_LOGE(TAG, "Claim standby BLE ensure failed or became stale");
        }
        return ensured;
    }

    if (blufi.GetBleState() == Blufi::BleState::kOff) {
        // Not advertising yet -> bring BLE up. Guarded by kOff so we never call
        // init() twice without a deinit() in between (double-init would leak the
        // BT controller/host). init() resets the re-advertise cap for this fresh
        // discoverable window. init() does NOT disturb the connected station.
        ESP_LOGI(TAG, "Claim standby: starting BLE advertising (TBOT-<MAC>)");
        const esp_err_t init_error = prepare() == ESP_OK ? blufi.init() : ESP_FAIL;
        if (init_error != ESP_OK) {
            ESP_LOGE(TAG, "Claim standby BLE start failed: BLUFI init failed");
            blufi.AbortProvisioningSetup(provisioning_token);
            return false;
        }
    }

    // Re-arm the BLE hard-timeout on every standby poll. The poll cadence
    // (kClaimPollIntervalUs, ~10s) is far shorter than CONFIG_BLE_SETUP_TIMEOUT
    // _SEC (300s), so the one-shot timer is continually pushed forward and never
    // tears BLE down WHILE we remain unclaimed in standby — the robot stays
    // discoverable. The hard-timeout still fires (and tears BLE down) if we ever
    // stop re-arming, i.e. the moment we leave standby. The §9 re-advertise cap
    // (kMaxBleReadvertiseAttempts) is untouched and still bounds a flapping peer.
    blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);
#else
    if (expected_generation.has_value()) {
        (void)expected_generation;
        if (on_current) {
            on_current();
        }
    }
#endif
    return true;
}

void Application::EnsureBleAdvertisingForUnclaimedSavedWifi() {
    if (IsDeviceClaimed()) {
        return;
    }

    ESP_LOGI(TAG, "Stored WiFi exists but device is unclaimed; keeping BLE advertising open for setup");
    EnsureBleAdvertisingForStandby();
}

void Application::StopBleAdvertising() {
    StopBleAdvertisingImpl(std::nullopt);
}

bool Application::StopBleAdvertisingForSetupGeneration(
        uint32_t expected_generation, const std::function<void()>& on_current) {
    return StopBleAdvertisingImpl(expected_generation, on_current);
}

bool Application::StopBleAdvertisingImpl(
        std::optional<uint32_t> expected_generation,
        const std::function<void()>& on_current) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();
    if (expected_generation.has_value()) {
        const bool was_active = blufi.GetBleState() != Blufi::BleState::kOff;
        const esp_err_t result = blufi.DeinitForSetupGeneration(
            expected_generation.value(), on_current);
        if (result != ESP_OK) {
            return false;
        }
        if (was_active) {
            ESP_LOGI(TAG, "Leaving claimable standby: stopping BLE advertising");
        }
        return true;
    }
    // Cancel the hard-timeout first so a stale timer callback cannot post a
    // redundant teardown after deinit() (mirrors WifiBoard::OnNetworkEvent).
    blufi.CancelBleSetupTimeout();
    if (blufi.GetBleState() != Blufi::BleState::kOff) {
        ESP_LOGI(TAG, "Leaving claimable standby: stopping BLE advertising");
        return blufi.deinit() == ESP_OK;
    }
#else
    if (expected_generation.has_value()) {
        (void)expected_generation;
        if (on_current) {
            on_current();
        }
    }
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Heartbeat (C5)
// ---------------------------------------------------------------------------

static std::string FirmwareVersionForHeartbeat() {
    const std::string user_agent = SystemInfo::GetUserAgent();
    const std::size_t slash = user_agent.rfind('/');
    if (slash == std::string::npos || slash + 1 >= user_agent.size()) {
        return user_agent;
    }
    return user_agent.substr(slash + 1);
}

static std::string CopyStringField(cJSON* object, const char* key, const char* fallback) {
    if (object == nullptr) {
        return fallback;
    }
    cJSON* value = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0') {
        return fallback;
    }
    return value->valuestring;
}

static int ClampInt(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int ExtractWifiRssi(cJSON* status_root) {
    cJSON* network = status_root == nullptr ? nullptr : cJSON_GetObjectItem(status_root, "network");
    cJSON* rssi = network == nullptr ? nullptr : cJSON_GetObjectItem(network, "rssi");
    if (!cJSON_IsNumber(rssi)) {
        return -127;
    }
    return ClampInt(rssi->valueint, -127, 0);
}

static std::string ExtractWifiSsid(cJSON* status_root) {
    cJSON* network = status_root == nullptr ? nullptr : cJSON_GetObjectItem(status_root, "network");
    cJSON* ssid = network == nullptr ? nullptr : cJSON_GetObjectItem(network, "ssid");
    if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr) {
        return "";
    }
    const std::size_t length = std::strlen(ssid->valuestring);
    if (length == 0 || length > 32) {
        return "";
    }
    return ssid->valuestring;
}

bool Application::ShouldKeepManagementHeartbeat() const {
    return IsDeviceClaimed() &&
           !lesson_runtime_active_.load() &&
           GetDeviceState() == kDeviceStateIdle &&
           !IsConnectSuccessPublicationSuppressed();
}

static std::string BuildTbotHeartbeatBody(const std::string& status_json,
                                          const std::string& device_id) {
    cJSON* status_root = cJSON_Parse(status_json.c_str());
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    const std::string firmware_version = FirmwareVersionForHeartbeat();
    cJSON_AddStringToObject(root, "firmware_version", firmware_version.c_str());

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
        battery_level = 0;
    }
    battery_level = ClampInt(battery_level, 0, 100);
    cJSON_AddNumberToObject(root, "battery_level", battery_level);

    const int wifi_rssi = ExtractWifiRssi(status_root);
    cJSON* connectivity = cJSON_CreateObject();
    cJSON_AddStringToObject(connectivity, "connectivity_state", "online");
    cJSON_AddNumberToObject(connectivity, "wifi_rssi", wifi_rssi);
    const std::string wifi_ssid = ExtractWifiSsid(status_root);
    if (!wifi_ssid.empty()) {
        cJSON_AddStringToObject(connectivity, "wifi_ssid", wifi_ssid.c_str());
    }
    cJSON_AddItemToObject(root, "connectivity_metrics", connectivity);

    const std::string ble_state = CopyStringField(status_root, "ble_state", "off");
    const std::string ap_state = CopyStringField(status_root, "ap_state", "off");
    cJSON_AddStringToObject(root, "ble_state", ble_state.c_str());
    cJSON_AddStringToObject(root, "ap_state", ap_state.c_str());

    float temp = 0.0f;
    if (Board::GetInstance().GetTemperature(temp)) {
        cJSON_AddNumberToObject(root, "temp", temp);
    }

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw == nullptr ? "{}" : raw;
    if (raw != nullptr) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    if (status_root != nullptr) {
        cJSON_Delete(status_root);
    }
    return body;
}

void Application::StartHeartbeat() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    if (heartbeat_active_) {
        return;
    }
    if (open_channel_queue == nullptr || open_channel_task == nullptr) {
        ESP_LOGE(TAG, "Persistent network worker unavailable for heartbeat");
        return;
    }
    if (heartbeat_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                // Post to the Application task, which gates + spawns the off-task
                // HTTP worker. The blocking POST must never run on the prio-10
                // main task (it starves the wake-word AFE pipeline -> "Hi ESP"
                // needs several tries), so do NOT call SendDeviceHeartbeat() here.
                app->Schedule([app]() { app->DispatchDeviceHeartbeat(); });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "heartbeat",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &heartbeat_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create heartbeat timer");
            heartbeat_timer_ = nullptr;
            return;
        }
    }
    heartbeat_active_ = true;
    esp_timer_start_periodic(heartbeat_timer_, kHeartbeatIntervalUs);
    ESP_LOGI(TAG, "Heartbeat started (every %lus)",
             static_cast<unsigned long>(kHeartbeatIntervalUs / 1000000ULL));
#endif
}

void Application::StopHeartbeat() {
    if (heartbeat_timer_ != nullptr && heartbeat_active_) {
        esp_timer_stop(heartbeat_timer_);
    }
    heartbeat_active_ = false;
}

void Application::HandleHeartbeatAuthFailure(int status_code) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGW(TAG, "Heartbeat auth failed (HTTP %d) during lesson; deferring claim recovery", status_code);
        StopHeartbeat();
        deferred_heartbeat_auth_failure_status_.store(status_code);
        return;
    }
    ESP_LOGW(TAG, "Heartbeat auth failed (HTTP %d); entering remote-unpair WiFi setup", status_code);
    StopHeartbeat();
    StopClaimPoll();
    CloseAudioChannelByIntent();

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetStatus(Lang::Strings::INITIALIZING);
        display->SetChatMessage("system", "");
    }

    {
        Settings backend_settings("backend", true);
        backend_settings.SetString("device_id", "");
        backend_settings.SetString("device_secret", "");
        backend_settings.SetInt("release_pending", 0);
    }
    {
        Settings claim_state("tbot_claim", true);
        claim_state.SetInt("confirmed", 0);
        claim_state.SetInt("factory_test", 0);
    }
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetString("token", "");
        websocket_settings.SetString("url", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
        websocket_settings.EraseKey("claim_device_id");
    }

    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    SecureClearString(pending_tbot_claim_token_);
    claim_confirmation_ambiguous_ = false;
    claim_substate_ = TbotClaimSubstate::AvailableStandby;
    backend_offline_.store(false);

    // A revoked heartbeat is the durable fallback when the backend invalidates
    // ownership before its WebSocket unpair command reaches the robot. Forget
    // the old network so the normal boot path opens BLUFI without a BOOT press.
    const auto wifi_clear_result =
        SsidManager::GetInstance().ForceClearAndCancelTransaction();
    if (wifi_clear_result != SsidMutationResult::kApplied) {
        ESP_LOGE(TAG, "Heartbeat auth recovery could not clear saved WiFi");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void Application::EnterRepairPairingMode(ChatRequestContext context) {
    if (!IsChatRequestCurrent(context)) return;
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    ESP_LOGW(TAG, "Course-mode local endpoint blocks repair/reset networking");
    return;
#else
    // Callable from the BOOT button task; marshal ALL claim-FSM + NVS mutation onto
    // the Application task (OQ1: the claim state machine is single-threaded).
    Schedule([this, context]() {
        if (!IsChatRequestCurrent(context)) return;
        if (lesson_runtime_active_.load()) {
            ESP_LOGW(TAG, "lesson re-pair ignored during lesson");
            return;
        }
        ESP_LOGW(TAG, "BOOT re-pair: forgetting current claim so a new parent phone can connect");
        StopHeartbeat();
        CloseAudioChannelByIntent();
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::INITIALIZING);
        display->SetChatMessage("system", "");

        // Release backend ownership NOW (synchronous) if we have credentials and are
        // online. This is a deliberate, user-initiated reset, so a one-time blocking
        // ~5s POST on the app task is acceptable -- and it is REQUIRED for the feature
        // to work: a deferred/async release races the parent re-pairing on the new
        // phone and loses (the phone's claim hits the backend first and gets
        // DEVICE_ALREADY_OWNED -> "robot already paired"). Freeing the `devices` row
        // here, before we re-advertise, guarantees the next claim succeeds.
        // If we're offline the POST fails fast (5s cap) and we fall back to
        // release_pending so RefreshPendingTbotClaim retries once we're back online
        // (honors "the robot may have no Wi-Fi at reset time").
        bool had_cloud_secret = false;
        {
            Settings backend_settings("backend", false);
            had_cloud_secret = !backend_settings.GetString("device_secret").empty() &&
                               !backend_settings.GetString("device_id").empty() &&
                               !backend_settings.GetString("api_url").empty();
        }
        bool released = !had_cloud_secret;
        if (had_cloud_secret) {
            released = SystemReset::ReleaseCloudOwnership();
            ESP_LOGW(TAG, "BOOT re-pair cloud ownership release: %s",
                     released ? "OK (backend freed for re-claim)"
                              : "FAILED (offline/auth?) -> deferred retry when online");
        } else {
            ESP_LOGW(TAG, "BOOT re-pair: no cloud credentials to release (treating as already free)");
        }

        // Local unclaim: drop the claimed flag + the live WS/claim tokens so the
        // robot stops acting as an owned device and re-advertises for pairing.
        {
            Settings backend_settings("backend", true);
            if (released) {
                // Backend row is freed (or already absent). Drop the complete
                // identity before reboot so startup cannot briefly run claimed
                // audio/runtime and fragment the heap before provisioning.
                backend_settings.SetString("device_id", "");
                backend_settings.SetString("device_secret", "");
                backend_settings.SetInt("release_pending", 0);
            } else if (had_cloud_secret) {
                // KEEP the credentials so the deferred POST can authenticate later.
                backend_settings.SetInt("release_pending", 1);
            }
        }
        {
            Settings claim_state("tbot_claim", true);
            claim_state.SetInt("confirmed", 0);
            claim_state.SetInt("factory_test", 0);
        }
        {
            Settings websocket_settings("websocket", true);
            websocket_settings.SetString("bootstrap_token", "");
            websocket_settings.SetString("token", "");
            websocket_settings.SetInt("claim_ambiguous", 0);
            websocket_settings.SetString("url", "");
            websocket_settings.EraseKey("claim_device_id");
        }

        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        backend_offline_.store(false);
        RenderClaimSubstate(claim_substate_);

        // The parent wants to re-pair AND choose a (possibly different) Wi-Fi network.
        // Forget the saved Wi-Fi and reboot: on the next boot the robot has no SSID, so
        // TryWifiConnect() falls into StartWifiConfigMode() and re-opens BLE Wi-Fi
        // provisioning, so the phone is prompted to pick a network. If we instead stayed
        // on the old Wi-Fi and only re-advertised claimable standby, the app sees the
        // robot already-online and SKIPS the Wi-Fi step -> the parent can never change
        // networks (the reported "can't set a different Wi-Fi"). The cloud row was freed
        // synchronously above (while still online); the offline case keeps
        // release_pending so the deferred release fires once the NEW network connects.
        const auto wifi_clear_result =
            SsidManager::GetInstance().ForceClearAndCancelTransaction();
        if (wifi_clear_result != SsidMutationResult::kApplied) {
            ESP_LOGE(TAG, "BOOT re-pair could not clear saved WiFi");
            return;
        }
        ESP_LOGW(TAG, "BOOT re-pair: Wi-Fi forgotten; rebooting into Wi-Fi setup for a new network");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    });
#endif
}

namespace {
struct CloudReleaseContext {
    Application* app;
    std::string api_url;
    std::string device_id;
    std::string device_secret;
};
}  // namespace

void Application::MaybeDispatchDeferredCloudRelease() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    Settings backend_settings("backend", false);
    if (backend_settings.GetInt("release_pending", 0) == 0) {
        return;  // No pending BOOT re-pair release.
    }
    const std::string api_url = backend_settings.GetString("api_url");
    const std::string device_id = backend_settings.GetString("device_id");
    std::string device_secret = backend_settings.GetString("device_secret");
    SecureStringScope device_secret_scope(device_secret);
    if (api_url.empty() || device_id.empty() || device_secret.empty()) {
        // No (remaining) cloud credentials to release -> nothing the backend can
        // act on. Clear the marker so we stop re-checking on every refresh.
        Settings writable("backend", true);
        writable.SetInt("release_pending", 0);
        return;
    }

    // Same belt-and-suspenders gating as the claim fetch: never start a blocking
    // TLS POST while live audio is in flight, and single-flight so a stuck network
    // can't stack workers across refreshes.
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateConnecting ||
        state == kDeviceStateListening ||
        state == kDeviceStateSpeaking ||
        connect_in_flight_.load()) {
        return;
    }
    bool expected = false;
    if (!cloud_release_inflight_.compare_exchange_strong(expected, true)) {
        return;  // Release already in flight.
    }

    // Low priority + not pinned to core 0 (same as ClaimFetchTask) so the blocking
    // POST waits on the network without starving the core-0 wake-word AFE pipeline.
    // Internal DRAM stack required: worker + result path touch NVS/flash (cache-off).
    auto* ctx = new (std::nothrow) CloudReleaseContext{
        this, api_url, device_id, device_secret};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "cloud_release context allocation failed; retrying next refresh");
        cloud_release_inflight_.store(false);
        return;
    }
    if (xTaskCreateWithCaps(&Application::CloudReleaseTask, "cloud_release", 6144, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        SecureClearString(ctx->device_secret);
        delete ctx;
        ESP_LOGE(TAG, "cloud_release task create failed; retrying next refresh");
        cloud_release_inflight_.store(false);
    }
#endif
}

void Application::CloudReleaseTask(void* arg) {
    {
        auto* ctx = static_cast<CloudReleaseContext*>(arg);
        Application* self = ctx->app;

        // Use the credentials captured before task creation. Reading NVS here could
        // release a newer claim that arrived while this worker was waiting to run.
        const bool released = SystemReset::ReleaseCloudOwnership(ctx->api_url, ctx->device_id, ctx->device_secret);
        std::string api_url = std::move(ctx->api_url);
        std::string device_id = std::move(ctx->device_id);
        std::string device_secret = std::move(ctx->device_secret);
        delete ctx;

        // Marshal the result back onto the Application task (OQ1) and clear the
        // single-flight guard there.
        self->Schedule([self, released, api_url = std::move(api_url),
                        device_id = std::move(device_id),
                        device_secret = std::move(device_secret)]() mutable {
            self->cloud_release_inflight_.store(false);
            Settings current_settings("backend", false);
            const bool credentials_unchanged =
                current_settings.GetString("api_url") == api_url &&
                current_settings.GetString("device_id") == device_id &&
                current_settings.GetString("device_secret") == device_secret;
            if (!credentials_unchanged) {
                Settings backend_settings("backend", true);
                backend_settings.SetInt("release_pending", 0);
                ESP_LOGI(TAG, "Deferred cloud release completed against superseded credentials");
                std::fill(device_secret.begin(), device_secret.end(), '\0');
                return;
            }
            if (!released) {
                ESP_LOGW(TAG, "Deferred cloud ownership release failed; will retry on next refresh");
                std::fill(device_secret.begin(), device_secret.end(), '\0');
                return;
            }
            Settings backend_settings("backend", true);
            backend_settings.SetInt("release_pending", 0);
            backend_settings.SetString("device_id", "");
            backend_settings.SetString("device_secret", "");
            std::fill(device_secret.begin(), device_secret.end(), '\0');
            ESP_LOGI(TAG, "Deferred cloud ownership released; robot is free for a new parent to claim");
            self->RefreshPendingTbotClaim();
        });
    }
    vTaskDeleteWithCaps(nullptr);
}

namespace {
struct HeartbeatContext {
    Application* app;
    std::string url;
    std::string device_secret;
    std::string body;
};
}  // namespace

void Application::DispatchDeviceHeartbeat() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    // Runs on the Application task. Do ALL the gating + URL/body build here (it
    // reads the FSM DeviceState and NVS settings, which stay serialized on the one
    // task that owns them per OQ1), then hand ONLY the blocking ~5s HTTP/TLS POST
    // to an off-task worker. The old design ran the POST inline on this priority-10
    // task, freezing the core-0 wake-word AFE feed/fetch pipeline for up to 5s
    // every 20s -> "Hi ESP" had to be repeated until an utterance landed in a gap.

    // H2: gate on a LIVE online DeviceState (Idle/Listening/Speaking), not merely
    // on token presence. A claimed device sitting in WifiConfiguring/Activating/
    // Connecting/Upgrading/Error has no healthy session to report and must not
    // fire heartbeats. The timer can still be stopped late, so this is the
    // authoritative runtime gate.
    const DeviceState device_state = GetDeviceState();
    if (device_state != kDeviceStateIdle &&
        device_state != kDeviceStateListening &&
        device_state != kDeviceStateSpeaking) {
        return;
    }

    // Single-flight: a slow backend must never let the 20s timer stack workers.
    bool expected = false;
    if (!heartbeat_inflight_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Heartbeat already in flight; skipping this tick");
        return;
    }

    // Gate: only claimed/online. Backend API auth uses the backend device
    // secret from claim/confirm; websocket.token is reserved for realtime WS
    // HMAC auth and must not be reused here.
    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    if (api_url.empty()) {
        heartbeat_inflight_.store(false);
        return;
    }
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (device_secret.empty()) {
        heartbeat_inflight_.store(false);
        return;  // Not claimed yet -> do not heartbeat.
    }
    const std::string backend_device_id = backend_settings.GetString("device_id");
    if (backend_device_id.empty()) {
        ESP_LOGW(TAG, "Heartbeat skipped: missing backend device id");
        heartbeat_inflight_.store(false);
        return;
    }

    // Build POST {api_url}/device/heartbeat. The body matches the backend
    // heartbeat DTO and copies radio/temp telemetry from the board status JSON.
    std::string base = api_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    // Ensure the /v1 API prefix (see claim_confirmation_reporter BuildTbot* URLs).
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    const std::string status_json = Board::GetInstance().GetDeviceStatusJson();

    auto* ctx = new HeartbeatContext{this, base + "/device/heartbeat", device_secret,
                                     BuildTbotHeartbeatBody(status_json, backend_device_id)};
    // The persistent worker is allocated once at claim time, before repeated TLS
    // calls fragment internal SRAM. Queueing avoids requiring a new contiguous
    // task stack on every 20-second tick.
    const NetworkWorkItem work{NetworkWorkKind::kHeartbeat, ctx};
    if (open_channel_queue == nullptr || xQueueSend(open_channel_queue, &work, 0) != pdTRUE) {
        ESP_LOGE(TAG, "heartbeat worker queue unavailable; retrying next tick");
        delete ctx;
        heartbeat_inflight_.store(false);
    } else {
        ESP_LOGI(TAG, "Heartbeat queued");
    }
#endif
}

void Application::HeartbeatTask(void* arg) {
    auto* ctx = static_cast<HeartbeatContext*>(arg);
    if (ctx == nullptr) return;
    auto* self = ctx->app;
    ESP_LOGI(TAG, "Heartbeat worker received request");
    const std::string url = ctx->url;
    const std::string device_secret = ctx->device_secret;
    std::string body = std::move(ctx->body);
    delete ctx;

    const int status_code = self->SendDeviceHeartbeat(url, device_secret, std::move(body));
    self->Schedule([self, status_code]() {
        self->heartbeat_inflight_.store(false);
        if (status_code == 401 || status_code == 403) {
            self->HandleHeartbeatAuthFailure(status_code);
        }
    });
}

int Application::SendDeviceHeartbeat(const std::string& url, const std::string& device_secret,
                                     std::string body) {
    // Runs on the off-task HeartbeatTask worker (see DispatchDeviceHeartbeat).
    // Returns the HTTP status code (or 0 on transport failure); the caller
    // marshals auth-failure handling back onto the Application task.
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for heartbeat");
        return 0;
    }
    // B1: still cap the blocking Open() at 5s (was the prio-10-task safety bound;
    // keep it bounded on the worker too so a hung TLS handshake can't leak tasks).
    http->SetTimeout(5000);
    http->SetHeader("X-Device-Token", device_secret);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "Heartbeat HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return 0;
    }
    const int status_code = http->GetStatusCode();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Heartbeat failed (HTTP %d)", status_code);
        return status_code;
    }
    ESP_LOGI(TAG, "Heartbeat accepted (HTTP %d)", status_code);
    return status_code;
}

TbotBleSubstate Application::GetBleSubstate() const {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    switch (Blufi::GetInstance().GetBleState()) {
        case Blufi::BleState::kAdvertising:
        case Blufi::BleState::kConnected:
            return TbotBleSubstate::Advertising;
        case Blufi::BleState::kTimeout:
            return TbotBleSubstate::Timeout;
        case Blufi::BleState::kOff:
        default:
            return TbotBleSubstate::Off;
    }
#else
    // SoftAP/other builds have no BLE radio in this path.
    return TbotBleSubstate::Off;
#endif
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();
    SystemInfo::PrintHeapCheckpoint("activation.start");

    // Rollback is useful when a new image cannot boot, but waiting until the
    // network OTA check completes makes a healthy image vulnerable to rollback
    // during tunnel/backend outages. At this point the app, display, audio, and
    // activation task have started, so mark the running image valid before doing
    // any network-bound version or assets work.
    ota_->MarkCurrentVersionValid();

#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    if (!IsDeviceClaimed()) {
        // Unclaimed + saved Wi-Fi keeps BLE advertising open for phone setup.
        // Running OTA HTTPS/config fetch at the same time exhausts internal heap
        // (TLS + BluFi), so keep claimed-only bootstrap off this path while still
        // creating the raw WebSocket session used by public lesson asset fanout.
        ESP_LOGW(TAG,
                 "Unclaimed device: skip OTA/bootstrap HTTPS while BLE stays up "
                 "(claim path remains via BLE; public lesson sync uses raw WS)");
        CheckAssetsVersion();
    } else {
        // Check for new assets version
        CheckAssetsVersion();

        // Check for new firmware version
        SystemInfo::StartHeapPhaseMonitor();
        CheckNewVersion();
        SystemInfo::PrintHeapCheckpoint("ota_check.complete");
        SystemInfo::StopHeapPhaseMonitor();

        // Claimed devices can override the OTA-provided websocket.url from the
        // backend's authenticated runtime config. If this fails, keep the existing
        // OTA/NVS value and compile-time placeholder fallback chain.
        SystemInfo::StartHeapPhaseMonitor();
        RefreshWebsocketUrlFromConfigFetch();
        SystemInfo::PrintHeapCheckpoint("config_fetch.complete");
        SystemInfo::StopHeapPhaseMonitor();

        // Keep the AFE released until the passive WebSocket TLS handshake has
        // completed. Both need contiguous internal SRAM; materializing wake-word
        // here can leave the socket retry without even a 4 KB allocation.
    }
#else
    SystemInfo::StartHeapPhaseMonitor();
    CheckNewVersion();
    SystemInfo::PrintHeapCheckpoint("course_mode_local_config.complete");
    SystemInfo::StopHeapPhaseMonitor();
#endif

    // Publication and its completion event belong to the application task.
    RequestInitializeProtocol(ProtocolActivation::kNormal);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();

    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    while (true) {
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < kOtaCheckMaxAttempts; ++attempt) {
            auto current_state = GetDeviceState();
            if (current_state == kDeviceStateWifiConfiguring ||
                current_state == kDeviceStateAudioTesting ||
                current_state == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Skipping OTA version check because activation ended");
                return;
            }
            display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

            err = ota_->CheckVersion();
            if (err == ESP_OK) {
                break;
            }
            if (attempt + 1 >= kOtaCheckMaxAttempts) {
                char error_message[32];
                snprintf(error_message, sizeof(error_message), "code=%d", err);
                Alert(Lang::Strings::ERROR, error_message, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);
                ESP_LOGE(TAG, "OTA version check exhausted its bounded retry budget, code=%d", err);
                return;
            }

            const int retry_delay = kOtaRetryDelaysSeconds[attempt];
            ESP_LOGW(TAG, "OTA version check failed; retry in %d seconds (%d/%d), code=%d",
                     retry_delay, attempt + 1, kOtaCheckMaxAttempts, err);
            for (int elapsed_seconds = 0; elapsed_seconds < retry_delay; ++elapsed_seconds) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                auto delayed_state = GetDeviceState();
                if (delayed_state == kDeviceStateWifiConfiguring ||
                    delayed_state == kDeviceStateAudioTesting) {
                    ESP_LOGI(TAG, "Aborting OTA retry because WiFi config mode is active");
                    return;
                }
                if (delayed_state == kDeviceStateIdle) {
                    ESP_LOGI(TAG, "Aborting OTA retry because activation ended");
                    return;
                }
            }
        }

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::RequestInitializeProtocol(ProtocolActivation activation) {
    auto request = [this, activation]() {
        if (activation != ProtocolActivation::kNone) {
            protocol_activation_pending_ = activation;
        }
        ++connect_generation_;
        reset_pending_.store(true);
        protocol_reinit_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReinitialize);
        CompletePendingProtocolWork();
    };
    if (xTaskGetCurrentTaskHandle() == application_task_) request();
    else Schedule(std::move(request));
}

void Application::CompleteProtocolActivation() {
    if (protocol_heap_monitor_pending_) {
        SystemInfo::PrintHeapCheckpoint("protocol_init.complete");
        SystemInfo::StopHeapPhaseMonitor();
        protocol_heap_monitor_pending_ = false;
    }
    const auto activation = protocol_activation_pending_;
    protocol_activation_pending_ = ProtocolActivation::kNone;
    if (claim_protocol_completion_pending_) {
        claim_protocol_completion_pending_ = false;
        CompleteClaimProtocolActivation();
    }
    if (activation == ProtocolActivation::kNone) return;
    SystemInfo::PrintHeapCheckpoint(activation == ProtocolActivation::kNormal
        ? "activation.complete" : "wifi_reprovision_activation.complete");
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::InitializeProtocol() {
    if (protocol_activation_pending_ == ProtocolActivation::kNormal) {
        SystemInfo::StartHeapPhaseMonitor();
        protocol_heap_monitor_pending_ = true;
    }
    backend_recovery_window_.Reset();
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Track whether we built the realtime WebSocket protocol. For WS,
    // Protocol::Start() is purely the audio-channel preconnect
    // (WebsocketProtocol::Start() -> OpenAudioChannel()), so it is the call we
    // skip while UNCLAIMED (see the BLE+audio contention note at the Start()
    // call below). For MQTT, Start() brings up the control channel (not just an
    // audio preconnect), so we never gate it here.
    bool is_websocket_protocol = false;
    std::string transient_evidence_journey_id =
        ota_->TakeTransientEvidenceJourneyId();
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    Settings websocket_settings("websocket", false);
    const bool has_configured_websocket_url =
        !websocket_settings.GetString("url", "").empty();
    const bool prefer_claimed_websocket =
        IsDeviceClaimed() && has_configured_websocket_url;
    const bool has_available_websocket_url =
        !websocket_settings.GetString("url", CONFIG_WEBSOCKET_URL).empty();
    if (ota_->HasMqttConfig() && !prefer_claimed_websocket) {
        SecureClearString(transient_evidence_journey_id);
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig() || has_available_websocket_url) {
        auto websocket_protocol = std::make_unique<WebsocketProtocol>();
        websocket_protocol->SetTransientConfig(
            ota_->GetTransientWebsocketUrl(), ota_->GetTransientWebsocketToken(),
            std::move(transient_evidence_journey_id));
        websocket_protocol->SetUnclaimedPublicLessonOnly(!IsDeviceClaimed());
        protocol_ = std::move(websocket_protocol);
        is_websocket_protocol = true;
    } else {
        SecureClearString(transient_evidence_journey_id);
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#else
    auto websocket_protocol = std::make_unique<WebsocketProtocol>();
    websocket_protocol->SetTransientConfig(
        ota_->GetTransientWebsocketUrl(), ota_->GetTransientWebsocketToken(),
        std::move(transient_evidence_journey_id));
    websocket_protocol->SetUnclaimedPublicLessonOnly(false);
    protocol_ = std::move(websocket_protocol);
    is_websocket_protocol = true;
#endif
    SecureClearString(transient_evidence_journey_id);
    protocol_generation_.fetch_add(1, std::memory_order_acq_rel);

    Protocol* callback_protocol = protocol_.get();
    const uint64_t callback_protocol_generation =
        protocol_generation_.load(std::memory_order_acquire);
    try {
        std::atomic_store(&chat_protocol_signals_, std::make_shared<ChatProtocolSignals>());
        chat_source_open_handled_ = 0;
        chat_source_failure_handled_ = 0;
        chat_passive_ping_id_ = 0;
    } catch (...) {
        std::atomic_store(&chat_protocol_signals_, std::shared_ptr<ChatProtocolSignals>());
        chat_protocol_fault_ = true;
        chat_protocol_infrastructure_fault_ = true;
    }
    const auto callback_signals = chat_protocol_signals_;
    protocol_->OnConnected([this, callback_protocol_generation, callback_signals]() {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        Schedule([this, callback_protocol_generation, callback_signals, callback_era]() {
        if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era)) return;
        if (callback_protocol_generation != protocol_generation_.load() ||
            chat_protocol_owned_.load(std::memory_order_acquire)) return;
        backend_recovery_window_.Reset();
        if (IsConnectSuccessPublicationSuppressed()) {
            ESP_LOGI(TAG, "connect success publication suppressed");
            online_intent_.store(false);
            StopHeartbeat();
            return;
        }
        backend_offline_.store(false);  // healthy session -> ONLINE, not retry
        DismissAlert();
        const bool lesson_answer_turn =
            lesson_interactive_listen_pending_.load() ||
            lesson_interactive_listening_active_.load();
        if (lesson_runtime_active_.load() && !lesson_answer_turn) {
            ESP_LOGI(TAG, "lesson protocol connected without heartbeat");
            StopHeartbeat();
            return;
        }
        // Device session is up -> begin periodic heartbeat (C5). The sender is
        // self-gated: it only POSTs once claim backend credentials are in NVS.
        StartHeartbeat();
        DispatchDeviceHeartbeat();
        });
    });

    protocol_->OnNetworkError([this, callback_protocol_generation, callback_signals](const std::string& message) {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        if (callback_signals && callback_signals->Deferred()) {
            if (callback_signals->Publish(callback_era, ChatProtocolSignals::Error)) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            }
            return;
        }
        try {
        Schedule([this, callback_protocol_generation, callback_signals, callback_era, message]() {
        if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era)) return;
        if (callback_protocol_generation != protocol_generation_.load() ||
            chat_protocol_owned_.load(std::memory_order_acquire)) return;
        backend_offline_.store(true);   // -> OFFLINE_RETRY copy via the mapper
        // The lesson WebSocket and management HTTP endpoint have independent
        // availability. Keep claimed-idle presence alive while the passive
        // lesson socket backs off; real network loss has its own stop path.
        if (ShouldKeepManagementHeartbeat()) {
            StartHeartbeat();
            DispatchDeviceHeartbeat();
        } else {
            StopHeartbeat();
        }
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        });
        } catch (...) {
            if (callback_signals) callback_signals->Publish(callback_era, ChatProtocolSignals::Error);
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        }
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (!lesson_audio_playout_.AllowsAudio(speaking_generation_.load())) return;
        if (GetDeviceState() == kDeviceStateSpeaking || tts_audio_accepting_.load()) {
            last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
            // Stamp the active response generation so a frame that slips in just
            // as the response is cancelled is gen-gated out at dequeue.
            packet->generation = speaking_generation_.load();
            packet->conversation_audio = true;
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board, callback_protocol, callback_protocol_generation, callback_signals]() {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        // Both protocols invoke this synchronously inside OpenAudioChannel.
        // No subsequent Open can reserve until this worker's completion releases.
        const auto callback_connect_generation = protocol_callback_connect_generation_.load();
        const int callback_sample_rate = callback_protocol->server_sample_rate();
        Schedule([this, codec, &board, callback_protocol_generation, callback_connect_generation,
                  callback_signals, callback_era, callback_sample_rate]() {
        if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era)) return;
        if (callback_protocol_generation != protocol_generation_.load() ||
            callback_connect_generation != connect_generation_.load() ||
            chat_protocol_owned_.load(std::memory_order_acquire)) return;
        backend_recovery_window_.Reset();
        if (IsConnectSuccessPublicationSuppressed()) {
            ESP_LOGI(TAG, "audio channel success publication suppressed");
            online_intent_.store(false);
            StopHeartbeat();
            return;
        }
        // User-driven listen/wake sessions own reconnect intent. Passive lesson
        // preconnect only makes the device reachable for server lesson pull/nudge;
        // it must not later reconnect into Listening without a wake/button action.
        if (passive_ws_intent_.load()) {
            if (IsDeviceClaimed() && !lesson_runtime_active_.load()) {
                ESP_LOGI(TAG, "claimed passive lesson websocket opened with management heartbeat");
                StartHeartbeat();
                DispatchDeviceHeartbeat();
            }
            if (!IsDeviceClaimed() || lesson_runtime_active_.load()) {
                ESP_LOGI(TAG, "unclaimed passive lesson websocket opened without heartbeat");
                StopHeartbeat();
            }
        } else {
            const bool lesson_answer_turn =
                lesson_interactive_listen_pending_.load() ||
                lesson_interactive_listening_active_.load();
            if (lesson_runtime_active_.load() && !lesson_answer_turn) {
                ESP_LOGI(TAG, "lesson audio channel opened ignored");
                online_intent_.store(false);
                StopHeartbeat();
                return;
            }
            online_intent_.store(true);
            StartHeartbeat();
            DispatchDeviceHeartbeat();
        }
        backend_offline_.store(false);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        // Once a claimed realtime WS is up, STOP the blocking claim-config
        // HTTP/TLS poll. Unclaimed public lesson sync must not change claim
        // provisioning semantics.
        if (IsDeviceClaimed()) {
            Schedule([this, callback_protocol_generation, callback_connect_generation,
                      callback_signals, callback_era]() {
                if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era)) return;
                if (callback_protocol_generation != protocol_generation_.load() ||
                    callback_connect_generation != connect_generation_.load() ||
                    chat_protocol_owned_.load(std::memory_order_acquire)) return;
                StopClaimPoll();
            });
        }
        if (callback_sample_rate != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                callback_sample_rate, codec->output_sample_rate());
        }
        });
    });
    
    protocol_->OnAudioChannelClosed([this, callback_protocol, callback_protocol_generation, callback_signals]() {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        if (callback_signals && callback_signals->Deferred()) {
            if (callback_signals->Publish(callback_era, ChatProtocolSignals::Closed)) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            }
            return;
        }
        Schedule([this, callback_protocol, callback_protocol_generation, callback_signals, callback_era]() {
            if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era)) return;
            if (chat_protocol_owned_.load(std::memory_order_acquire)) return;
            if (!ProtocolLifetimeMatches(
                    protocol_.get(), callback_protocol,
                    protocol_generation_.load(std::memory_order_acquire),
                    callback_protocol_generation)) {
                return;
            }
            speaking_arm_dispatch_.Cancel();
            tts_audio_accepting_.store(false);
            // WebSocket close callbacks can run on a PSRAM-backed transport task.
            // NVS-backed claim checks and Wi-Fi power changes must run on the
            // Application task, whose stack remains available while flash cache
            // operations are in progress.
            if (ShouldKeepManagementHeartbeat()) {
                StartHeartbeat();
                DispatchDeviceHeartbeat();
            } else {
                StopHeartbeat();
            }
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            RequestLessonStorageAbandonment();
            auto display = Board::GetInstance().GetDisplay();
            if (!lesson_runtime_active_.load()) {
                display->SetChatMessage("system", "");
            }
            if (GetDeviceState() == kDeviceStateWifiConfiguring ||
                GetDeviceState() == kDeviceStateAudioTesting) {
                // The close was caused by explicit setup entry. Keep the setup
                // state/screen; do not fall back to ONLINE/Idle or schedule a
                // websocket reconnect while Wi-Fi provisioning owns the radio.
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
                return;
            }
            if (lesson_runtime_active_.load() && passive_ws_intent_.load()) {
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
                RequestLessonStorageAbandonment();
                if (PassiveReconnectHasOwner(reconnect_passive_.load(),
                                             connect_in_flight_.load())) {
                    ESP_LOGI(TAG, "lesson passive_liveness_reconnect_pending");
                    return;
                }
                ESP_LOGW(TAG, "lesson passive ws dropped -> passive reconnect");
                SchedulePassiveLessonReconnect();
                return;
            }
            if (connect_in_flight_.load()) {
                ESP_LOGW(TAG, "ws_close_ignored_during_connect");
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            // WSS-7: drop stale mic backlog so a future session does not replay
            // seconds-old uplink audio after the channel reopens.
            while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
            // Claimed idle robots keep a passive lesson socket so admin/backend
            // nudges can reach the LCD without putting the device into Listening.
            // Idle WebSocket timeout is an unexpected drop for that passive path;
            // reopen the same passive channel instead of using voice reconnect.
            if (passive_ws_intent_.load()) {
                if (reconnect_passive_.load()) {
                    ESP_LOGI(TAG, "passive_liveness_reconnect_pending");
                    return;
                }
                ESP_LOGW(TAG, "passive_lesson_ws_dropped_unexpected -> passive reconnect");
                SchedulePassiveLessonReconnect();
                return;
            }
            // Sustained operation: an UNEXPECTED drop (server/tunnel closed the WS,
            // NOT a user/system close) leaves online_intent_ true -> auto-reconnect
            // with backoff so a 20-60 min conversation is not permanently cut off.
            if (online_intent_.load()) {
                if (lesson_runtime_active_.load()) {
                    ESP_LOGW(TAG, "lesson ws dropped unexpected -> suppress generic reconnect");
                    RequestLessonStorageAbandonment();
                    online_intent_.store(false);
                    lesson_interactive_listen_generation_.fetch_add(1);
                    lesson_interactive_listen_pending_.store(false);
                    lesson_interactive_listening_active_.store(false);
                    backend_offline_.store(true);
                    audio_service_.ResetDecoder();
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                    return;
                }
                ESP_LOGW(TAG, "ws_dropped_unexpected -> auto-reconnect (online_intent)");
                backend_offline_.store(true);
                audio_service_.ResetDecoder();
                display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
                display->SetEmotion("thinking");
                audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
                ScheduleReconnect(GetDefaultListeningMode(), false);
                return;
            }
            if (lesson_runtime_active_.load()) {
                RequestLessonStorageAbandonment();
            }
        });
    });
    
    protocol_->OnIncomingJson([this, is_websocket_protocol](const cJSON* root, uint64_t epoch) {
        DispatchIncomingJson(root, epoch, is_websocket_protocol);
    });

    if (!InitializeChatSourceRoute(is_websocket_protocol)) {
        backend_offline_.store(true);
        online_intent_.store(false);
        microphone_uplink_authorized_.store(false);
        display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
        return;
    }

    // WebSocket Start() opens the realtime audio channel. Unclaimed devices keep
    // it closed until wake/button so BLE claim and local wake-word setup own the
    // radio. Claimed devices open a PASSIVE channel so ESP-server connect-time
    // lesson pull and backend lesson nudges have a route without entering
    // Listening. MQTT Start() is a control-channel connect, so it still runs here.
    if (is_websocket_protocol) {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
        ESP_LOGI(TAG, "Course-mode local endpoint: opening private-LAN WebSocket");
#endif
        if (IsDeviceClaimed()) {
            ESP_LOGI(TAG, "Claimed device: opening passive WebSocket for lesson/nudge");
            StartPassiveLessonWebsocket();
        } else {
            ESP_LOGI(TAG, "Unclaimed device: opening passive WebSocket for public lesson sync");
            StartPassiveLessonWebsocket();
        }
    } else {
        StartProtocolWorker();
    }
    if (is_websocket_protocol) CompleteProtocolActivation();
}

bool Application::HandleRobotActionMessage(const cJSON* root, ChatRequestContext context) {
    if (!IsChatRequestCurrent(context)) return false;
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot action ignored");
        return false;
    }

    auto action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) {
        return false;
    }

    if (strcmp(action->valuestring, "head_set_angle") == 0) {
        auto angle = cJSON_GetObjectItem(root, "angle");
        int target_angle = cJSON_IsNumber(angle) ? angle->valueint : 90;
        Schedule([this, target_angle, context]() {
            if (!IsChatRequestCurrent(context)) return;
            SendHeadSetAngle(target_angle);
        });
        return true;
    }

    auto schedule_percent_action = [this, root, context](bool (Application::*method)(int), int default_percent) {
        auto percent = cJSON_GetObjectItem(root, "percent");
        int target_percent = cJSON_IsNumber(percent) ? percent->valueint : default_percent;
        Schedule([this, method, target_percent, context]() {
            if (!IsChatRequestCurrent(context)) return;
            (this->*method)(target_percent);
        });
    };
    if (strcmp(action->valuestring, "left_arm_set_percent") == 0) {
        schedule_percent_action(&Application::SendLeftArmSetPercent, 100);
        return true;
    }
    if (strcmp(action->valuestring, "right_arm_set_percent") == 0) {
        schedule_percent_action(&Application::SendRightArmSetPercent, 100);
        return true;
    }
    if (strcmp(action->valuestring, "both_arms_set_percent") == 0) {
        schedule_percent_action(&Application::SendBothArmsSetPercent, 100);
        return true;
    }
    if (strcmp(action->valuestring, "head_set_percent") == 0) {
        schedule_percent_action(&Application::SendHeadSetPercent, 50);
        return true;
    }

    using RobotActionHandler = bool (Application::*)();
    static const struct {
        const char* action;
        RobotActionHandler handler;
    } handlers[] = {
        {"left_arm_raise", &Application::SendLeftArmRaise},
        {"right_arm_raise", &Application::SendRightArmRaise},
        {"left_arm_lower", &Application::SendLeftArmLower},
        {"right_arm_lower", &Application::SendRightArmLower},
        {"both_arms_raise", &Application::SendBothArmsRaise},
        {"both_arms_lower", &Application::SendBothArmsLower},
        {"head_turn_left", &Application::SendHeadTurnLeft},
        {"head_turn_right", &Application::SendHeadTurnRight},
        {"head_center", &Application::SendHeadCenter},
    };

    for (const auto& handler : handlers) {
        if (strcmp(action->valuestring, handler.action) == 0) {
            Schedule([this, method = handler.handler, context]() {
                if (!IsChatRequestCurrent(context)) return;
                (this->*method)();
            });
            return true;
        }
    }

    return false;
}

void Application::HandleEmotionGesture(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Emotion gesture ignored for arm control: %s", emotion);
}

bool Application::SendLeftArmRaise() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmRaise();
}

bool Application::SendRightArmRaise() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmRaise();
}

bool Application::SendLeftArmLower() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmLower();
}

bool Application::SendRightArmLower() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmLower();
}

bool Application::SendBothArmsRaise() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsRaise();
}

bool Application::SendBothArmsLower() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsLower();
}

bool Application::SendLeftArmSetPercent(int percent) {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmSetPercent(percent);
}

bool Application::SendRightArmSetPercent(int percent) {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmSetPercent(percent);
}

bool Application::SendBothArmsSetPercent(int percent) {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsSetPercent(percent);
}

bool Application::SendHeadTurnLeft() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadTurnLeft();
}

bool Application::SendHeadTurnRight() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadTurnRight();
}

bool Application::SendHeadCenter() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadCenter();
}

bool Application::SendHeadSetAngle(int angle) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadSetAngle(angle);
}

bool Application::SendHeadSetPercent(int percent) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadSetPercent(percent);
}

LessonRuntimeToken Application::GetLessonRuntimeToken() const {
    if (!lesson_runtime_active_.load()) return {};
    return {lesson_runtime_generation_.load()};
}

LessonEmbodiedMotionResult Application::ApplyLessonEmbodiedPreset(
    const LessonRuntimeToken& token,
    const LessonEmbodiedPreset& preset) {
    if (!IsLessonRuntimeTokenAuthorized(
            lesson_runtime_active_.load(), lesson_runtime_generation_.load(), token)) {
        ESP_LOGI(TAG, "stale lesson embodied action token rejected");
        return LessonEmbodiedMotionResult::kRejected;
    }
    return ApplyLessonEmbodiedPresetCommands(
        preset,
        [this](int percent) { return robot_uart_.SendHeadSetPercent(percent); },
        [this](int percent) { return robot_uart_.SendLeftArmSetPercent(percent); },
        [this](int percent) { return robot_uart_.SendRightArmSetPercent(percent); });
}

LessonEmbodiedMotionResult Application::CancelLessonEmbodiedAction(
    const LessonRuntimeToken& token) {
    return RestoreLessonRestPose(token);
}

LessonEmbodiedMotionResult Application::RestoreLessonRestPose(
    const LessonRuntimeToken& token) {
    return ApplyLessonEmbodiedPreset(
        token, ResolveLessonEmbodiedPreset(LessonEmbodiedIntent::kRestWarm));
}

#if CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS
bool Application::RunCourseModeHilTftPattern() {
    return ScheduleAndWait([] {
        Display* display = Board::GetInstance().GetDisplay();
        if (display == nullptr) return false;
        lv_obj_t* screen = lv_screen_active();
        if (screen == nullptr) return false;
        lv_obj_t* pattern = lv_obj_create(screen);
        if (pattern == nullptr) return false;
        lv_obj_set_size(pattern, lv_pct(100), lv_pct(100));
        lv_obj_center(pattern);
        lv_obj_set_flex_flow(pattern, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(pattern, 0, 0);
        lv_obj_set_style_pad_gap(pattern, 0, 0);
        lv_obj_set_style_border_width(pattern, 0, 0);
        lv_obj_set_style_radius(pattern, 0, 0);
        constexpr std::array<std::uint32_t, 3> colors = {
            0xFF0000, 0x00FF00, 0x0000FF};
        for (std::uint32_t color : colors) {
            lv_obj_t* bar = lv_obj_create(pattern);
            if (bar == nullptr) return false;
            lv_obj_set_size(bar, lv_pct(34), lv_pct(100));
            lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 0, 0);
        }
        lv_timer_t* timer = lv_timer_create([](lv_timer_t* value) {
            lv_obj_t* object = static_cast<lv_obj_t*>(lv_timer_get_user_data(value));
            if (object != nullptr) lv_obj_delete(object);
            lv_timer_delete(value);
        }, 1000, pattern);
        if (timer == nullptr) {
            lv_obj_delete(pattern);
            return false;
        }
        lv_timer_set_repeat_count(timer, 1);
        return true;
    }, 1000);
}

CourseModeHilSdEvidence Application::RunCourseModeHilSdRead(
    const std::string& relative_path, const std::string& expected_sha256) {
    constexpr char kRoot[] = "/sdcard/tbot/lesson-assets";
    constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;
    constexpr std::int64_t kMaxReadUs = 5 * 1000 * 1000;
    if (expected_sha256.size() != 64) return {};
    for (char ch : expected_sha256) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && (ch < 'a' || ch > 'f')) return {};
    }
    CourseModeHilAssetFile asset;
    if (!OpenCourseModeHilAssetFile(kRoot, relative_path, kMaxBytes, &asset)) return {};

    auto hash_file = [&](std::uint32_t* bytes, std::string* sha256) {
        std::rewind(asset.file);
        std::clearerr(asset.file);
        const int descriptor = fileno(asset.file);
        mbedtls_sha256_context context;
        mbedtls_sha256_init(&context);
#if MBEDTLS_VERSION_MAJOR >= 3
        int result = mbedtls_sha256_starts(&context, 0);
#else
        int result = mbedtls_sha256_starts_ret(&context, 0);
#endif
        std::array<unsigned char, 4096> buffer {};
        std::size_t total = 0;
        const std::int64_t deadline = esp_timer_get_time() + kMaxReadUs;
        while (result == 0) {
            const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), asset.file);
            if (read > 0) {
                total += read;
                if (total > kMaxBytes || esp_timer_get_time() > deadline) {
                    result = -1;
                    break;
                }
#if MBEDTLS_VERSION_MAJOR >= 3
                result = mbedtls_sha256_update(&context, buffer.data(), read);
#else
                result = mbedtls_sha256_update_ret(&context, buffer.data(), read);
#endif
            }
            if (read < buffer.size()) {
                if (std::ferror(asset.file)) result = -1;
                break;
            }
            esp_task_wdt_reset();
            taskYIELD();
        }
        std::array<unsigned char, 32> digest {};
        if (result == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
            result = mbedtls_sha256_finish(&context, digest.data());
#else
            result = mbedtls_sha256_finish_ret(&context, digest.data());
#endif
        }
        struct stat final_metadata {};
        if (result == 0 && (descriptor < 0 || fstat(descriptor, &final_metadata) != 0 ||
                           !S_ISREG(final_metadata.st_mode) || total != asset.bytes ||
                           static_cast<std::uint64_t>(final_metadata.st_size) != asset.bytes)) {
            result = -1;
        }
        mbedtls_sha256_free(&context);
        if (result != 0 || total == 0 || total > UINT32_MAX) return false;
        constexpr char kHex[] = "0123456789abcdef";
        sha256->resize(64);
        for (std::size_t index = 0; index < digest.size(); ++index) {
            (*sha256)[index * 2] = kHex[digest[index] >> 4U];
            (*sha256)[index * 2 + 1] = kHex[digest[index] & 0x0FU];
        }
        *bytes = static_cast<std::uint32_t>(total);
        return true;
    };

    std::uint32_t first_bytes = 0;
    std::uint32_t second_bytes = 0;
    std::string first_sha;
    std::string second_sha;
    const bool verified = hash_file(&first_bytes, &first_sha) &&
        first_sha == expected_sha256 && hash_file(&second_bytes, &second_sha) &&
        second_bytes == first_bytes && second_sha == first_sha;
    CloseCourseModeHilAssetFile(&asset);
    if (!verified) return {};
    return {true, second_bytes, second_sha, "verifiedReadback"};
}

bool Application::RunCourseModeHilAudioDrain() {
    return audio_service_.WaitForPlaybackQueueEmpty(1000) && audio_service_.IsIdle();
}

bool Application::RunCourseModeHilStopAndRest() {
    speaking_arm_dispatch_.Cancel();
    bool sent = robot_uart_.SendBothArmsLower();
    return robot_uart_.SendHeadCenter() && sent;
}

bool Application::RunCourseModeHilSafeMotion(int duration_ms) {
    speaking_arm_dispatch_.Cancel();
    if (duration_ms <= 0 || duration_ms > 750) return false;
    bool sent = robot_uart_.SendRightArmRaise();
    sent = robot_uart_.SendHeadCenter() && sent;
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return RunCourseModeHilStopAndRest() && sent;
}
#endif

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1},
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson alert suppressed: %s", status);
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle && !lesson_runtime_active_.load()) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

uint32_t Application::BeginLessonInteractiveListeningRequest() {
    return lesson_interactive_listen_generation_.fetch_add(1) + 1;
}

void Application::PrepareLessonInteractiveListening() {
    PrepareLessonInteractiveListening(lesson_interactive_listen_generation_.load());
}

void Application::PrepareLessonInteractiveListening(uint32_t generation) {
    if (generation != lesson_interactive_listen_generation_.load()) {
        ESP_LOGI(TAG, "stale lesson interactive listen prepare ignored");
        return;
    }
    if (!lesson_runtime_active_.load()) {
        lesson_interactive_listen_pending_.store(false);
        return;
    }
    lesson_interactive_listen_pending_.store(true);
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ClearChatMessages();
        display->SetStatus("Sắp đến lượt con...");
    }
    StartListening();
}

void Application::CancelLessonInteractiveListening() {
    lesson_interactive_listen_generation_.fetch_add(1);
    const bool had_pending = lesson_interactive_listen_pending_.exchange(false);
    const bool had_active = lesson_interactive_listening_active_.exchange(false);
    const bool had_lesson_listen = had_pending || had_active;
    const bool lesson_runtime_cancel = lesson_runtime_active_.load();
    if (!lesson_runtime_cancel && !had_lesson_listen) {
        return;
    }
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateConnecting) {
        lesson_idle_repaint_suppressed_.store(true);
        ++connect_generation_;
        connect_attempt_active_.store(false);
        passive_ws_intent_.store(false);
        online_intent_.store(false);
        CancelConnectWatchdog();
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (state != kDeviceStateListening) {
        return;
    }
    lesson_idle_repaint_suppressed_.store(true);
    if (protocol_) {
        protocol_->SendStopListening();
    }
    listening_started_ms_.store(0);
    last_listening_activity_ms_.store(0);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    SetDeviceState(kDeviceStateIdle);
}

void Application::SetLessonRuntimeActive(bool active) {
    if (active) speaking_arm_dispatch_.Cancel();
    const bool was_active = lesson_runtime_active_.load();
    if (was_active != active) {
        if (active) {
            lesson_runtime_generation_.store(NextLessonRuntimeGeneration(
                lesson_runtime_generation_.load(), was_active, active));
            lesson_runtime_active_.store(true);
        } else {
            lesson_runtime_active_.store(false);
            lesson_runtime_generation_.store(NextLessonRuntimeGeneration(
                lesson_runtime_generation_.load(), was_active, active));
        }
    }
    if (active) {
        lesson_terminal_audio_generation_.store(0);
    }
    if (!active) {
        lesson_interactive_listen_generation_.fetch_add(1);
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        const int status_code = deferred_heartbeat_auth_failure_status_.exchange(0);
        if (status_code != 0) {
            Schedule([this, status_code]() {
                HandleHeartbeatAuthFailure(status_code);
            });
        }
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        static_cast<WifiBoard&>(Board::GetInstance())
            .ResumePendingWifiConfigMode();
#endif
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
}

void Application::BeginLessonTerminalAudioQuiet() {
    std::lock_guard<std::mutex> lock(lesson_playout_mutex_);
    if (auto token = std::atomic_load(&lesson_playout_authorization_)) token->store(false);
    lesson_audio_playout_.Cancel();
    lesson_terminal_audio_generation_.store(
        static_cast<std::uint64_t>(speaking_generation_.load()) + 1);
    tts_audio_accepting_.store(false);
    speaking_arm_dispatch_.Cancel();
    // Terminal errors may have no subsequent TTS STOP. Fence in-flight decode
    // before clearing queued output, without rearming a child listening turn.
    audio_service_.SetPlaybackGeneration(++speaking_generation_);
    audio_service_.ResetDecoder();
    last_speaking_activity_ms_.store(0);
    CancelLessonInteractiveListening();
    if (GetDeviceState() == kDeviceStateSpeaking) {
        lesson_idle_repaint_suppressed_.store(true);
        SetDeviceState(kDeviceStateIdle);
    }
}

bool Application::IsLessonRuntimeActive() const {
    return lesson_runtime_active_.load();
}

void Application::BeginLessonNetworkRenderQuiet() {
    int depth = lesson_network_render_quiet_.fetch_add(1) + 1;
    if (depth == 1) {
        ESP_LOGI(TAG, "lesson_network_render_quiet begin");
    }
}

void Application::EndLessonNetworkRenderQuiet() {
    int previous = lesson_network_render_quiet_.fetch_sub(1);
    if (previous <= 1) {
        lesson_network_render_quiet_.store(0);
        ESP_LOGI(TAG, "lesson_network_render_quiet end");
    }
}

bool Application::IsLessonNetworkRenderQuiet() const {
    return lesson_network_render_quiet_.load() > 0;
}

bool Application::HasLessonAssetSyncWakeOpportunity() {
    // State changes may originate in board callbacks; only App owns the deadline.
    if (lesson_asset_sync_wake_invalidated_.exchange(false))
        lesson_asset_sync_wake_deadline_us_ = 0;
    if (!IsDeviceClaimed()) {
        lesson_asset_sync_wake_pending_ = false;
        lesson_asset_sync_wake_deadline_us_ = 0;
        return true;
    }
    if (!lesson_asset_sync_wake_pending_) return true;
    // A deferred stop/restart may complete between clock observations.
    if (lesson_asset_sync_wake_revoked_ != chat_audio_desired_.revoked) {
        lesson_asset_sync_wake_revoked_ = chat_audio_desired_.revoked;
        lesson_asset_sync_wake_deadline_us_ = 0;
    }
    if (GetDeviceState() != kDeviceStateIdle || !audio_service_.IsWakeWordRunning()) {
        lesson_asset_sync_wake_deadline_us_ = 0;
        return false;
    }
    const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
    if (!lesson_asset_sync_wake_deadline_us_)
        lesson_asset_sync_wake_deadline_us_ = now_us + 3000000ULL;
    if (lesson_asset_sync_wake_invalidated_.exchange(false)) {
        lesson_asset_sync_wake_deadline_us_ = 0;
        return false;
    }
    return now_us >= lesson_asset_sync_wake_deadline_us_;
}

bool Application::BeginLessonAssetSyncQuiet() {
#if CONFIG_TBOT_VOICE_DEMO
    return false;
#endif
    // Busy retries must not stop the settling timer or revoke the wake window.
    if (!HasLessonAssetSyncWakeOpportunity()) return false;
    const DeviceState state = GetDeviceState();
    const bool passive_listening = state == kDeviceStateListening &&
        !chat_cleanup_enabled_.load() && passive_ws_intent_.load() &&
        !online_intent_.load() && !microphone_uplink_authorized_.load() && !IsVoiceDetected();
    if ((state != kDeviceStateIdle && !passive_listening) ||
        lesson_runtime_active_.load() || connect_in_flight_.load() || reset_pending_.load()) return false;
    if (lesson_asset_sync_wake_pending_ && lesson_asset_sync_wake_invalidated_.exchange(false)) {
        lesson_asset_sync_wake_deadline_us_ = 0;
        return false;
    }
    bool expected = false;
    if (!lesson_asset_sync_quiet_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "lesson asset sync quiet already active");
        return false;
    }

    lesson_asset_sync_wake_pending_ = false;
    lesson_asset_sync_wake_deadline_us_ = 0;
    if (lesson_asset_sync_wake_rearm_timer_ != nullptr) {
        esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
    }

    if (passive_listening) {
        lesson_idle_repaint_suppressed_.store(true);
        if (protocol_) {
            protocol_->SendStopListening();
        }
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        SetDeviceState(kDeviceStateIdle);
    }
    tts_audio_accepting_.store(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();
    while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
    ESP_LOGI(TAG, "lesson asset sync quiet begin");
    return true;
}

void Application::EndLessonAssetSyncQuiet() {
    if (!lesson_asset_sync_quiet_.load()) {
        return;
    }

    tts_audio_accepting_.store(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.ResetDecoder();
    while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
    if (!lesson_asset_sync_quiet_.exchange(false)) {
        return;
    }
    lesson_asset_sync_wake_pending_ = IsDeviceClaimed();
    lesson_asset_sync_wake_deadline_us_ = 0;

    // Passive-liveness polling was suspended during the sync (see the
    // IsLessonAssetSyncQuiet() gate in the CLOCK_TICK handler). Reset the ping
    // timer so it does not immediately fire a stale pong-timeout now that the WS
    // receive path is free again.
    if (protocol_ != nullptr) {
        protocol_->ResetPassiveLiveness();
    }

    if (IsDeviceClaimed() && audio_service_.IsRunning() &&
        GetDeviceState() == kDeviceStateIdle &&
        !lesson_runtime_active_.load() &&
        !connect_in_flight_.load() &&
        !reset_pending_.load()) {
        ScheduleLessonAssetSyncWakeRearm();
    }
    ESP_LOGI(TAG, "lesson asset sync quiet end");
}

void Application::ScheduleLessonAssetSyncWakeRearm() {
    ScheduleLessonAssetSyncWakeRearm(1500ULL * 1000ULL);
}

void Application::ScheduleLessonAssetSyncWakeRearm(uint64_t delay_us) {
    if (lesson_asset_sync_wake_rearm_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() {
                self->RearmClaimedIdleWakeWord();
                self->HasLessonAssetSyncWakeOpportunity();
            });
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "asset_wake";
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &lesson_asset_sync_wake_rearm_timer_) != ESP_OK) {
            lesson_asset_sync_wake_rearm_timer_ = nullptr;
            ESP_LOGE(TAG, "Failed to create lesson asset wake rearm timer");
            return;
        }
    }

    esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
    esp_timer_start_once(lesson_asset_sync_wake_rearm_timer_, delay_us);
}

void Application::StopListening() {
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() ||
        lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson stop listening ignored state=%d", static_cast<int>(GetDeviceState()));
        return;
    }
    if (!(GetDeviceState() == kDeviceStateSpeaking && lesson_interactive_listen_pending_.load())) {
        CancelLessonInteractiveListening();
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    if (IsWifiConfigEntryPending()) return;
    auto state = GetDeviceState();

    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson toggle ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        if (!audio_service_.IsRunning()) {
            ESP_LOGI(TAG, "Audio test unavailable while provisioning workers are deferred");
            return;
        }
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (pending_tbot_claim_.active) {
        ConfirmPendingTbotClaim();
        return;
    }

    // H1: "Setup expired" (CLAIM_CONFIRM_TIMEOUT) is not a dead-end. A button tap
    // while in ConfirmTimeout re-enters the bounded standby poll (the documented
    // ConfirmTimeout -> AvailableStandby recovery), so the screen returns to
    // "Ready to connect" instead of stranding. Route the tap to claim-retry here
    // rather than letting it fall through to the talk path below.
    if (claim_substate_ == TbotClaimSubstate::ConfirmTimeout) {
        ESP_LOGI(TAG, "Claim confirm timeout -> retry: re-entering claim standby poll");
        RefreshPendingTbotClaim();
        return;
    }

    if (!IsDeviceClaimed() && backend_offline_.load() &&
        (state == kDeviceStateIdle || state == kDeviceStateConnecting ||
         state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
        ESP_LOGI(TAG, "Unclaimed BOOT tap from offline retry -> reopening phone scan standby");
        backend_offline_.store(false);
        if (state != kDeviceStateIdle) {
            SetDeviceState(kDeviceStateIdle);
        }
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        RefreshPendingTbotClaim();
        return;
    }

    if (!IsDeviceClaimed() && state == kDeviceStateIdle &&
        (claim_substate_ == TbotClaimSubstate::AvailableStandby ||
         claim_substate_ == TbotClaimSubstate::None)) {
        ESP_LOGI(TAG, "Unclaimed BOOT tap -> refreshing claimable standby for phone scan");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        RefreshPendingTbotClaim();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    if (IsSelectedNormalChatRoute()) {
        if (state == kDeviceStateIdle) BeginChatListen(GetDefaultListeningMode(), ChatListenOrigin::User);
        else if (state == kDeviceStateSpeaking) HandleChatAbort(kAbortReasonNone, listening_mode_ != kListeningModeManualStop);
        else if (state == kDeviceStateListening) CloseAudioChannelByIntent();
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        CloseAudioChannelByIntent();
    }
}

namespace {
struct ConnectContext {
    Application* app;
    ListeningMode mode;
    uint32_t generation;
    std::string wake_word;
    bool wake_word_invoke = false;
    bool passive_preconnect = false;
    Protocol* protocol = nullptr;
    uint64_t protocol_generation = 0;
    uint64_t reservation = 0;
    bool start_protocol = false;
};
}  // namespace

void Application::StartPassiveLessonWebsocket() {
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    if (protocol_->IsAudioChannelOpened() || connect_in_flight_.load()) {
        return;
    }
    passive_ws_intent_.store(true);
    online_intent_.store(false);
    uint32_t gen = ++connect_generation_;
    connect_in_flight_.store(true);
    ArmConnectWatchdog();
    auto* ctx = new ConnectContext{this, GetDefaultListeningMode(), gen, std::string(), false, true};
    if (!StartOpenChannelWorker(ctx)) {
        delete ctx;
        connect_in_flight_.store(false);
        passive_ws_intent_.store(false);
        CancelConnectWatchdog();
        ESP_LOGE(TAG, "lesson_ws worker unavailable");
        SchedulePassiveLessonReconnect();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized");
        SetDeviceState(kDeviceStateIdle);   // SM-3: don't wedge CONNECTING
        return;
    }
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() ||
        lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson open channel ignored state=%d", static_cast<int>(GetDeviceState()));
        online_intent_.store(false);
        connect_attempt_active_.store(false);
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        SetListeningMode(mode);
        return;
    }
    if (connect_in_flight_.load()) {
        ESP_LOGW(TAG, "connect already in flight, ignoring duplicate request");
        return;
    }
    // SM-1/WSS-1: OpenAudioChannel() blocks (TCP+TLS handshake + server hello,
    // up to ~20s). Queue it on the persistent worker so reconnects do not depend
    // on finding a fresh contiguous 8KB internal heap block. connect_generation_
    // invalidates a stale result; the
    // connect watchdog (SM-3) recovers a wedged/black-hole connect.
    reconnect_mode_ = mode;
    online_intent_.store(true);  // we want an open channel -> reconnect on unexpected drop
    uint32_t gen = ++connect_generation_;
    connect_in_flight_.store(true);
    connect_attempt_active_.store(true);  // WSS-8: suppress per-attempt error banner until terminal
    ArmConnectWatchdog();
    passive_ws_intent_.store(false);
    auto* ctx = new ConnectContext{this, mode, gen, std::string(), false, false};
    if (!StartOpenChannelWorker(ctx)) {
        delete ctx;
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);  // WSS-8: no worker -> cycle ended
        CancelConnectWatchdog();
        ESP_LOGE(TAG, "ws_open worker unavailable -> idle");
        SetDeviceState(kDeviceStateIdle);
    }
}

bool Application::StartOpenChannelWorker(void* context) {
    RetireChatOutbound();
    if (chat_protocol_infrastructure_fault_) return false;
    if (open_channel_queue == nullptr || open_channel_task == nullptr) {
        return false;
    }
    auto* ctx = static_cast<ConnectContext*>(context);
    if (!protocol_ || protocol_work_lifetime_.Pending() || protocol_work_lifetime_.Busy()) return false;
    ctx->reservation = protocol_work_lifetime_.Reserve();
    if (!ctx->reservation) return false;
    ctx->protocol = protocol_.get();
    ctx->protocol_generation = protocol_generation_.load();
    const NetworkWorkItem work{NetworkWorkKind::kOpenChannel, context};
    if (xQueueSend(open_channel_queue, &work, 0) == pdTRUE) return true;
    protocol_work_lifetime_.Release(ctx->reservation);
    ctx->reservation = 0;
    return false;
}

bool Application::InitializeChatOutboundWorker() {
    if (chat_outbound_task_) return true;
    chat_outbound_task_ = xTaskCreateStatic(
        &Application::ChatOutboundTask, "chat_outbound", kChatOutboundWorkerStackDepth, this,
        tskIDLE_PRIORITY + 3, chat_outbound_task_stack, &chat_outbound_task_buffer);
    return chat_outbound_task_ != nullptr;
}

bool Application::InitializeChatAudioCleanupWorker() {
    if (chat_audio_task_) return true;
    chat_audio_task_ = xTaskCreateStatic(
        &Application::ChatAudioCleanupTask, "chat_audio_cleanup", kChatAudioCleanupWorkerStackDepth,
        this, tskIDLE_PRIORITY + 3, chat_audio_cleanup_task_stack, &chat_audio_cleanup_task_buffer);
    return chat_audio_task_ != nullptr;
}

bool Application::InitializeChatSourceRoute(bool is_websocket_protocol) {
    if (!is_websocket_protocol) return true;
    chat_cleanup_enabled_.store(false);
    const auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->SupportsChatOutputDrain()) return true;
    try {
        if (protocol_ && chat_protocol_signals_ && chat_outbound_task_ && open_channel_queue &&
            open_channel_task && InitializeChatAudioCleanupWorker()) {
            auto callbacks = MakeChatSourceCallbacks(protocol_generation_.load(), chat_protocol_signals_);
            if (callbacks.audio && callbacks.json && callbacks.closed && callbacks.opened && callbacks.adopted && callbacks.error) {
                protocol_->SetSourceCallbacks(std::move(callbacks));
                chat_protocol_signals_->SelectDeferred();
                chat_cleanup_enabled_.store(true);
                static_cast<WebsocketProtocol*>(protocol_.get())->SetConversationAudioDrainAck(true);
                chat_protocol_fault_ = chat_protocol_infrastructure_fault_ = false;
                return true;
            }
        }
    } catch (...) {}
    chat_protocol_fault_ = chat_protocol_infrastructure_fault_ = true;
    return false;
}

uint32_t Application::RequestChatPlaybackCleanup(uint32_t playback_generation) {
    if (chat_reboot_audio_requested_) return 0;
    chat_audio_reset_serial_ = audio_service_.RequestChatPlaybackReset();
    if (chat_audio_reset_serial_ == UINT32_MAX) chat_audio_exhausted_ = true;
    audio_service_.SetPlaybackGeneration(playback_generation);
    chat_playback_desired_ = chat_audio_reset_serial_;
    chat_playback_fault_ = false;
    // Preserve an outstanding capture preparation, but let it reset the latest
    // stream instead of endlessly retrying a superseded decoder token.
    if (chat_audio_desired_.revoked != chat_audio_completed_revoked_)
        chat_audio_desired_.reset_serial = chat_audio_reset_serial_;
    PollChatAudioCleanup();
    return chat_audio_reset_serial_;
}

bool Application::RequestChatCue(std::string_view sound) {
    std::unique_lock<std::mutex> lock(chat_cue_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || chat_cue_pending_ || chat_cue_serial_ == UINT32_MAX || sound.empty() || sound.size() > 65536) return false;
    try { chat_cue_owner_ = std::make_shared<const std::string>(sound); }
    catch (...) { return false; }
    ++chat_cue_serial_;
    chat_cue_sound_ = *chat_cue_owner_;
    chat_cue_generation_ = speaking_generation_.load();
    chat_cue_reset_ = audio_service_.ChatPlaybackResetToken();
    chat_cue_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
    chat_cue_pending_ = true;
    chat_cue_retry_ = false;
    lock.unlock();
    if (xTaskGetCurrentTaskHandle() == application_task_) PollChatAudioCleanup();
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    return true;
}

uint32_t Application::RequestChatAudioCleanup(uint32_t playback_generation, bool reset,
                                             bool processing, bool wake, bool chat_scope,
                                             bool stop_service, ChatWakePolicy wake_policy) {
    if (chat_reboot_audio_requested_ && !stop_service) return chat_audio_desired_.revoked;
    const uint32_t revoked = audio_service_.RevokeChatUplink();
    chat_audio_prepared_ = 0;
    if (reset) {
        chat_playback_desired_ = 0;
        chat_audio_reset_serial_ = audio_service_.RequestChatPlaybackReset();
        if (chat_audio_reset_serial_ == UINT32_MAX) chat_audio_exhausted_ = true;
    }
    audio_service_.SetPlaybackGeneration(playback_generation);
    chat_audio_desired_ = {};
    chat_audio_desired_.revoked = revoked;
    chat_audio_desired_.reset_serial = chat_audio_reset_serial_;
    chat_audio_desired_.processing = processing;
    chat_audio_desired_.wake = wake;
    chat_audio_desired_.chat_scope = chat_scope;
    chat_audio_desired_.stop_service = stop_service;
    chat_audio_desired_.wake_policy = wake_policy;
    PollChatAudioCleanup();
    return revoked;
}

void Application::PollChatAudioCleanup() {
    std::unique_lock<std::mutex> cue_lock(chat_cue_mutex_, std::try_to_lock);
    if (!cue_lock.owns_lock()) return;
    if (chat_audio_state_.load(std::memory_order_acquire) == 2) {
        if (chat_audio_work_.cue) {
            if (chat_audio_work_.cue_serial == chat_cue_serial_) {
                chat_cue_result_ = chat_audio_work_.cue_result;
                chat_cue_pending_ = chat_cue_result_ == 1;
                chat_cue_retry_ = chat_cue_pending_;
            }
            chat_audio_state_.store(0, std::memory_order_release);
        }
    }
    if (chat_audio_state_.load(std::memory_order_acquire) == 2) {
        if (chat_audio_work_.read_wake) {
            if (chat_audio_work_.wake_serial == chat_wake_read_serial_) {
                chat_wake_read_result_ = std::move(chat_audio_work_.wake_text);
                chat_wake_read_pending_ = false;
            }
            chat_audio_state_.store(0, std::memory_order_release);
        }
    }
    if (chat_audio_state_.load(std::memory_order_acquire) == 2) {
        if (chat_audio_work_.reset_done) chat_audio_reset_completed_ = chat_audio_work_.reset_serial;
        if (chat_audio_work_.playback_only) {
            chat_playback_attempted_ = chat_audio_work_.reset_serial;
            if (chat_audio_work_.reset_serial == chat_playback_desired_)
                chat_playback_fault_ = !chat_audio_work_.reset_done;
        } else {
            chat_audio_completed_revoked_ = chat_audio_work_.revoked;
            if (chat_audio_work_.revoked == chat_audio_desired_.revoked) {
                // Preparation completed before this newer reset was collected.
                // Its readiness was never published, so retain the obligation
                // until the same revoked capture token has the latest reset.
                if (chat_audio_work_.reset_serial != chat_audio_desired_.reset_serial)
                    chat_audio_completed_revoked_ = 0;
                chat_audio_fault_ = !chat_audio_work_.prepared;
                if (chat_audio_work_.prepared && chat_audio_work_.processing &&
                    chat_audio_work_.reset_serial == chat_audio_reset_serial_) {
                    chat_audio_prepared_ = chat_audio_work_.revoked;
                }
                if (chat_audio_work_.prepared && chat_audio_work_.stop_service) {
                    chat_reboot_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + 1000000ULL;
                }
            }
        }
        chat_audio_state_.store(0, std::memory_order_release);
    }
    if (chat_audio_state_.load(std::memory_order_acquire) != 0) return;
    if (chat_cue_pending_ && static_cast<uint64_t>(esp_timer_get_time()) >= chat_cue_deadline_us_) {
        chat_cue_pending_ = chat_cue_retry_ = false;
        chat_cue_result_ = 3;
    }
    const bool prepare = chat_audio_desired_.revoked &&
        chat_audio_completed_revoked_ != chat_audio_desired_.revoked;
    const bool playback = chat_playback_desired_ &&
        chat_playback_desired_ != chat_audio_reset_completed_ &&
        chat_playback_desired_ != chat_playback_attempted_;
    const bool cue = chat_cue_pending_ && !chat_cue_retry_;
    if (!prepare && !playback && !chat_wake_read_pending_ && !cue) return;
    if (!chat_audio_task_ || chat_audio_exhausted_) {
        if (prepare) chat_audio_fault_ = true;
        if (playback) chat_playback_fault_ = true;
        return;
    }
    chat_audio_work_ = prepare ? chat_audio_desired_ : ChatAudioCleanup{};
    if (!prepare && playback) {
        chat_audio_work_.playback_only = true;
        chat_audio_work_.reset_serial = chat_playback_desired_;
    }
    if (!prepare && !playback && chat_wake_read_pending_) {
        chat_audio_work_.read_wake = true;
        chat_audio_work_.wake_serial = chat_wake_read_serial_;
    }
    if (!prepare && !playback && !chat_wake_read_pending_ && cue) {
        chat_audio_work_.cue = true;
        chat_audio_work_.cue_serial = chat_cue_serial_;
        chat_audio_work_.cue_generation = chat_cue_generation_;
        chat_audio_work_.cue_deadline_us = chat_cue_deadline_us_;
        chat_audio_work_.cue_sound = chat_cue_sound_;
        chat_audio_work_.cue_owner = chat_cue_owner_;
        chat_audio_work_.reset_serial = chat_cue_reset_;
    }
    chat_audio_work_.reset = chat_audio_reset_completed_ != chat_audio_work_.reset_serial;
    chat_audio_state_.store(1, std::memory_order_release);
    xTaskNotifyGive(chat_audio_task_);
}

void Application::RunChatAudioCleanup() {
    if (chat_audio_state_.load(std::memory_order_acquire) != 1) return;
    try {
        if (chat_audio_work_.cue) {
            chat_audio_work_.cue_result = static_cast<int>(audio_service_.TryPlayChatCue(
                chat_audio_work_.cue_sound, chat_audio_work_.cue_generation, chat_audio_work_.reset_serial,
                chat_audio_work_.cue_deadline_us));
        } else if (chat_audio_work_.read_wake) {
            chat_audio_work_.wake_text = audio_service_.GetLastWakeWord();
        } else if (chat_audio_work_.stop_service) {
            audio_service_.Stop();
            chat_audio_work_.prepared = true;
        } else {
            if (chat_audio_work_.wake_policy == ChatWakePolicy::Listening) {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
                chat_audio_work_.wake = audio_service_.IsAfeWakeWord();
#else
                chat_audio_work_.wake = false;
#endif
            }
            if (chat_audio_work_.reset) {
                chat_audio_work_.reset_done = audio_service_.ResetChatDecoder(chat_audio_work_.reset_serial);
            }
            chat_audio_work_.prepared = (!chat_audio_work_.reset || chat_audio_work_.reset_done) &&
                (chat_audio_work_.playback_only || audio_service_.PrepareChatAudioTransition(
                    chat_audio_work_.revoked, chat_audio_work_.processing,
                    chat_audio_work_.wake, chat_audio_work_.chat_scope));
        }
    } catch (...) {
        chat_audio_work_.prepared = false;
        if (chat_audio_work_.cue) chat_audio_work_.cue_result = 3;
    }
    chat_audio_state_.store(2, std::memory_order_release);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::RetryChatAudioCleanup() {
    std::unique_lock<std::mutex> cue_lock(chat_cue_mutex_, std::try_to_lock);
    if (!cue_lock.owns_lock()) return;
    // Called once per application clock tick, not on failure notification.
    const bool retry_cue = chat_cue_retry_;
    if (chat_cue_retry_) {
        if (static_cast<uint64_t>(esp_timer_get_time()) >= chat_cue_deadline_us_) {
            chat_cue_pending_ = false;
            chat_cue_result_ = 3;
        }
        chat_cue_retry_ = false;
    }
    cue_lock.unlock();
    if (retry_cue) PollChatAudioCleanup();
    if (chat_playback_fault_ && !chat_audio_exhausted_ &&
        chat_audio_state_.load(std::memory_order_acquire) == 0) {
        chat_playback_attempted_ = 0;
        PollChatAudioCleanup();
    }
    if (chat_audio_fault_ && !chat_audio_exhausted_ &&
        chat_audio_state_.load(std::memory_order_acquire) == 0 &&
        (chat_audio_reset_completed_ != chat_audio_desired_.reset_serial ||
         chat_audio_desired_.stop_service)) {
        chat_audio_completed_revoked_ = 0;
        PollChatAudioCleanup();
    }
}

void Application::BeginChatRebootAudioCleanup() {
    if (chat_reboot_audio_requested_) return;
    chat_reboot_audio_requested_ = true;
    RequestChatAudioCleanup(speaking_generation_.load(), false, false, false, true, true);
}

void Application::PollChatReboot() {
    if (chat_reboot_deadline_us_ &&
        static_cast<uint64_t>(esp_timer_get_time()) >= chat_reboot_deadline_us_) {
        chat_reboot_deadline_us_ = 0;
        esp_restart();
    }
}

void Application::ChatAudioCleanupTask(void* context) {
    auto* app = static_cast<Application*>(context);
    for (;;) {
        app->RunChatAudioCleanup();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

bool Application::PollChatProtocolCleanup() {
    using Action = ProtocolWorkLifetime::Action;
    if (!chat_cleanup_enabled_) return false;
    if (chat_reboot_audio_requested_) return true;
    auto state = chat_protocol_state_.load(std::memory_order_acquire);
    if (state == 2) return true;
    if (state == 3) {
        const auto pending = protocol_work_lifetime_.TakeReady();
        if (!chat_protocol_work_.success) {
            chat_protocol_fault_ = true;
            chat_protocol_infrastructure_fault_ = true;
            protocol_work_lifetime_.Request(pending);
            if (pending <= chat_protocol_work_.action) return true;
        }
        if (pending > chat_protocol_work_.action) {
            chat_protocol_work_.action = pending;
            chat_protocol_work_.success = false;
            protocol_work_lifetime_.Request(pending);
            chat_protocol_state_.store(1, std::memory_order_release);
            state = 1;
        } else {
            const auto action = chat_protocol_work_.action;
            protocol_ = std::move(chat_protocol_work_.protocol);
            if (action != Action::kClose) {
                protocol_generation_.fetch_add(1, std::memory_order_acq_rel);
            }
            chat_protocol_state_.store(0, std::memory_order_release);
            chat_protocol_owned_.store(false, std::memory_order_release);
            deferred_close_generation_ = 0;
            reset_pending_ = false;
            protocol_reinit_pending_ = false;
            if (action == Action::kReboot) {
                protocol_work_lifetime_.Request(Action::kReboot);
                BeginChatRebootAudioCleanup();
            } else if (action == Action::kReinitialize) {
                InitializeProtocol();
            } else if (action == Action::kReset) {
                protocol_activation_pending_ = ProtocolActivation::kNone;
                claim_protocol_completion_pending_ = false;
            } else if (!protocol_start_pending_generation_) {
                CompleteProtocolActivation();
            }
            return true;
        }
    }
    if (state == 0) {
        if (!protocol_work_lifetime_.Pending()) return false;
        chat_protocol_owned_.store(true);
        if (lesson_protocol_readers_.load() != 0) return true;
        RetireChatOutbound();
        const auto action = protocol_work_lifetime_.TakeReady();
        if (action == Action::kNone) return true;
        // TakeReady proved that every real user retired. Restore the barrier
        // before zero-wait admission so a full network queue cannot reopen it.
        protocol_work_lifetime_.Request(action);
        if (chat_protocol_signals_) chat_protocol_signals_->Disable();
        chat_protocol_owned_.store(true, std::memory_order_release);
        chat_protocol_work_.protocol = std::move(protocol_);
        chat_protocol_work_.action = action;
        chat_protocol_work_.epoch = deferred_close_epoch_;
        chat_protocol_work_.intentional = connect_close_deferral_.TakeAfterWorker() ||
            deferred_close_generation_ != protocol_generation_.load();
        chat_protocol_work_.success = false;
        chat_protocol_work_.destructive_prepared = false;
        connect_in_flight_ = false;
        CancelConnectWatchdog();
        chat_protocol_state_.store(1, std::memory_order_release);
    }
    // Escalation while waiting for capacity updates the owned action in place.
    const auto pending = protocol_work_lifetime_.TakeReady();
    if (pending > chat_protocol_work_.action) chat_protocol_work_.action = pending;
    protocol_work_lifetime_.Request(chat_protocol_work_.action);
    if (chat_protocol_work_.action != Action::kClose && !chat_protocol_work_.destructive_prepared) {
        RequestLessonStorageAbandonment();
        CancelLessonRobotEntranceOnDisplay();
        protocol_start_pending_generation_ = 0;
        if (protocol_heap_monitor_pending_) {
            SystemInfo::StopHeapPhaseMonitor();
            protocol_heap_monitor_pending_ = false;
        }
        chat_protocol_work_.destructive_prepared = true;
    }
    if (!open_channel_queue || !open_channel_task) {
        chat_protocol_fault_ = true;
        chat_protocol_infrastructure_fault_ = true;
        return true;
    }
    const NetworkWorkItem work{NetworkWorkKind::kProtocolCleanup, this};
    chat_protocol_state_.store(2, std::memory_order_release);
    if (xQueueSend(open_channel_queue, &work, 0) != pdTRUE) {
        chat_protocol_state_.store(1, std::memory_order_release);
    }
    return true;
}

void Application::RunChatProtocolCleanup() {
    if (chat_protocol_state_.load(std::memory_order_acquire) != 2) return;
    try {
        if (chat_protocol_work_.protocol) {
            if (chat_protocol_work_.action == ProtocolWorkLifetime::Action::kClose) {
                if (chat_protocol_work_.intentional) chat_protocol_work_.protocol->CloseAudioChannel();
                else chat_protocol_work_.protocol->CompleteDeferredClose(chat_protocol_work_.epoch);
            } else {
                chat_protocol_work_.protocol.reset();
            }
        }
        chat_protocol_work_.success = true;
    } catch (...) {
        chat_protocol_work_.success = false;
    }
    chat_protocol_state_.store(3, std::memory_order_release);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

Protocol::SourceCallbacks Application::MakeChatSourceCallbacks(
    uint64_t protocol_generation, std::shared_ptr<ChatProtocolSignals> signals) {
    // Install the complete immutable callback set before any Start/Open.
    Protocol::SourceCallbacks callbacks;
    callbacks.opened = [this, protocol_generation, signals, callback_protocol = protocol_.get()](ConnectionSource source, uint64_t deadline_us) {
        const auto connect_generation = protocol_callback_connect_generation_.load();
        if (!signals || protocol_generation != protocol_generation_.load() ||
            chat_protocol_owned_.load() || connect_generation != connect_generation_.load()) return;
        signals->PublishOpened(source, connect_generation, callback_protocol->server_sample_rate(), deadline_us);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.adopted = [this, protocol_generation](ConnectionSource source) {
        return IsChatConnectionCurrent(source, protocol_generation, chat_source_connect_generation_.load()) &&
            (IsLessonVoiceRoute() || passive_ws_intent_.load() ||
                (!connect_in_flight_.load() && GetDeviceState() != kDeviceStateConnecting));
    };
    auto publish = [this, protocol_generation, signals](ConnectionSource source, uint32_t flag) {
        if (!signals || protocol_generation != protocol_generation_.load() ||
            chat_protocol_owned_.load(std::memory_order_acquire) ||
            protocol_callback_connect_generation_.load() != connect_generation_.load()) return;
        ESP_LOGW(TAG, "chat_source_fault reason=transport flags=%lu", static_cast<unsigned long>(flag));
        if (signals->PublishConnectionFault(source, protocol_callback_connect_generation_.load(), flag))
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.error = [publish](ConnectionSource source, const std::string&) {
        publish(source, ChatProtocolSignals::Error);
    };
    callbacks.closed = [publish](ConnectionSource source) {
        publish(source, ChatProtocolSignals::Closed);
    };
    callbacks.json = [this, protocol_generation, signals, callback_protocol = protocol_.get()](ConnectionSource source, const cJSON* root, uint64_t lesson_epoch, ConnectionReceipt receipt) {
        const ChatConnectionMessages::Owner owner{source, protocol_generation, chat_source_connect_generation_.load()};
        if (!IsChatConnectionCurrent(owner.source, owner.protocol_generation, owner.connect_generation)) return;
        bool queued_json = false;
        try {
        const auto* message_type = cJSON_GetObjectItem(root, "type");
        if (lesson_asset_sync_quiet_.load() && cJSON_IsString(message_type) &&
            (strcmp(message_type->valuestring, "tts") == 0 || strcmp(message_type->valuestring, "stt") == 0)) return;
        const auto* message_state = cJSON_GetObjectItem(root, "state");
#if CONFIG_TBOT_VOICE_DEMO
        // Presentation bursts must not occupy the bounded control queue.
        if (!IsLessonVoiceRoute() && cJSON_IsString(message_type) &&
            (strcmp(message_type->valuestring, "stt") == 0 ||
             strcmp(message_type->valuestring, "llm") == 0 ||
             (strcmp(message_type->valuestring, "tts") == 0 && cJSON_IsString(message_state) &&
              strcmp(message_state->valuestring, "sentence_start") == 0))) {
            const auto* text = cJSON_GetObjectItem(root, "text");
            if (strcmp(message_type->valuestring, "llm") != 0 && cJSON_IsString(text)) {
                signals->captions.Publish({source, protocol_generation, owner.connect_generation,
                    speaking_generation_.load(), 0}, strcmp(message_type->valuestring, "tts") == 0,
                    text->valuestring);
            }
            return;
        }
#endif
        if (IsLessonVoiceRoute() && cJSON_IsString(message_type) && strcmp(message_type->valuestring, "tts") == 0 &&
            cJSON_IsString(message_state) && (strcmp(message_state->valuestring, "start") == 0 ||
                strcmp(message_state->valuestring, "stop") == 0)) {
            auto context = chat_inbound_messages_.Own(root,
                {source, protocol_generation, chat_source_connect_generation_.load()},
                receipt.received_us, lesson_epoch, callback_protocol->session_id());
            if (!context) {
                const auto snapshot = chat_inbound_messages_.TrySnapshot();
                ESP_LOGW(TAG, "chat_json_queue available=%u queued=%u outstanding=%u",
                    static_cast<unsigned>(snapshot.available), static_cast<unsigned>(snapshot.queued), static_cast<unsigned>(snapshot.outstanding));
                ESP_LOGW(TAG, "chat_source_fault reason=lesson_json_admission");
                signals->PublishConnectionFault(source, chat_source_connect_generation_.load(), ChatProtocolSignals::Error);
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
                return;
            }
            if (!IsChatLessonRequestCurrent(context)) return;
            signals->lesson_audio_epoch = lesson_epoch;
            try { DispatchIncomingJson(context->root.get(), lesson_epoch, true, context); }
            catch (...) { FailChatRequest(context); }
            return;
        }
        HandleChatStart(signals, protocol_generation, source, root, receipt);
        HandleChatTerminalStop(signals, protocol_generation, source, root, receipt.received_us);
        const auto* type = cJSON_GetObjectItem(root, "type");
        const auto* state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "tts") == 0 && cJSON_IsString(state) &&
            (strcmp(state->valuestring, "start") == 0 || strcmp(state->valuestring, "stop") == 0)) return;
        queued_json = true;
        using Admission = ChatInboundMessages::Admission;
        const auto session_id = callback_protocol->session_id();
        Admission outcome = Admission::Invalid;
        uint32_t retries = 0;
        const auto deadline_us = receipt.received_us <= UINT64_MAX - 250000ULL ?
            receipt.received_us + 250000ULL : UINT64_MAX;
        const auto admission_deadline_us = receipt.admission_deadline_us && receipt.admission_deadline_us < deadline_us ?
            receipt.admission_deadline_us : deadline_us;
        const auto current = [this, owner]() {
            return IsChatConnectionCurrent(owner.source, owner.protocol_generation, owner.connect_generation);
        };
        const std::function<bool()> can_publish = [&]() {
            const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
            return current() && now_us >= receipt.received_us && now_us < admission_deadline_us;
        };
        for (;;) {
            if (!current()) return;
            if (!can_publish()) break;
            outcome = chat_inbound_messages_.TryAdmit(root, owner, receipt.received_us, lesson_epoch, session_id, can_publish);
            if (outcome == Admission::Accepted) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
                if (retries) {
                    const auto waited_us = static_cast<uint64_t>(esp_timer_get_time()) - receipt.received_us;
                    ESP_LOGW(TAG, "chat_json_admission reason=json_admission outcome=%u retries=%lu wait_us_hi=%lu wait_us_lo=%lu",
                        static_cast<unsigned>(outcome), static_cast<unsigned long>(retries),
                        static_cast<unsigned long>(waited_us >> 32), static_cast<unsigned long>(static_cast<uint32_t>(waited_us)));
                }
                return;
            }
            if (outcome != Admission::Busy && outcome != Admission::Full) break;
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            ++retries;
            vTaskDelay(1);
        }
        if (!current()) return;
        const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
        const auto waited_us = now_us >= receipt.received_us ? now_us - receipt.received_us : 0;
        const auto wait_hi = static_cast<unsigned long>(waited_us >> 32);
        const auto wait_lo = static_cast<unsigned long>(static_cast<uint32_t>(waited_us));
        const auto snapshot = chat_inbound_messages_.TrySnapshot();
        ESP_LOGW(TAG, "chat_json_queue available=%u queued=%u outstanding=%u",
            static_cast<unsigned>(snapshot.available), static_cast<unsigned>(snapshot.queued), static_cast<unsigned>(snapshot.outstanding));
        ESP_LOGW(TAG, "chat_source_fault reason=json_admission outcome=%u retries=%lu wait_us_hi=%lu wait_us_lo=%lu",
            static_cast<unsigned>(outcome), static_cast<unsigned long>(retries), wait_hi, wait_lo);
        signals->PublishConnectionFault(owner.source, owner.connect_generation, ChatProtocolSignals::Error);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        } catch (...) {
            if (!IsChatConnectionCurrent(owner.source, owner.protocol_generation, owner.connect_generation)) return;
            if (queued_json) {
                const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
                const auto waited_us = now_us >= receipt.received_us ? now_us - receipt.received_us : 0;
                const auto snapshot = chat_inbound_messages_.TrySnapshot();
                ESP_LOGW(TAG, "chat_json_queue available=%u queued=%u outstanding=%u",
                    static_cast<unsigned>(snapshot.available), static_cast<unsigned>(snapshot.queued), static_cast<unsigned>(snapshot.outstanding));
                ESP_LOGW(TAG, "chat_source_fault reason=json_admission outcome=%u retries=0 wait_us_hi=%lu wait_us_lo=%lu",
                    static_cast<unsigned>(ChatInboundMessages::Admission::NoMemory),
                    static_cast<unsigned long>(waited_us >> 32), static_cast<unsigned long>(static_cast<uint32_t>(waited_us)));
            } else ESP_LOGW(TAG, "chat_source_fault reason=json_exception");
            signals->PublishConnectionFault(owner.source, owner.connect_generation, ChatProtocolSignals::Error);
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        }
    };
    callbacks.audio = [this, protocol_generation, signals](ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet) {
        if (IsLessonVoiceRoute()) {
            HandleChatLessonAudio(signals, protocol_generation, source, std::move(packet));
            return;
        }
        HandleChatAudio(signals, protocol_generation, source, std::move(packet));
    };
    return callbacks;
}

bool Application::IsLessonVoiceRoute() const {
    return lesson_runtime_active_.load() || lesson_interactive_listen_pending_.load() ||
        lesson_interactive_listening_active_.load() || lesson_terminal_audio_generation_.load() != 0;
}

bool Application::IsChatLessonRequestCurrent(const ChatRequestContext& context) const {
    return !context || (IsChatRequestCurrent(context) &&
        context->lesson_epoch == lesson_transport_epoch_gate_.PublishedEpoch());
}

void Application::ScheduleChatLesson(ChatRequestContext context, std::function<void()> callback) {
    if (!IsChatLessonRequestCurrent(context)) return;
    try {
        Schedule([this, context, callback = std::move(callback)]() {
            if (!IsChatLessonRequestCurrent(context)) return;
            try { callback(); }
            catch (...) { if (!context) throw; FailChatRequest(context); }
        });
    } catch (...) { if (!context) throw; FailChatRequest(context); }
}

void Application::HandleChatLessonAudio(const std::shared_ptr<ChatProtocolSignals>& signals,
    uint64_t protocol_generation, ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet || !signals || !IsChatConnectionCurrent(source, protocol_generation, chat_source_connect_generation_.load()) ||
        signals->lesson_audio_epoch != lesson_transport_epoch_gate_.PublishedEpoch() || lesson_asset_sync_quiet_.load()) return;
    if (!lesson_audio_playout_.AllowsAudio(speaking_generation_.load())) return;
    if (GetDeviceState() == kDeviceStateSpeaking || tts_audio_accepting_.load()) {
        last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
        packet->generation = speaking_generation_.load();
        packet->conversation_audio = true;
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    }
}

void Application::HandleChatStart(const std::shared_ptr<ChatProtocolSignals>& signals,
    uint64_t protocol_generation, ConnectionSource source, const cJSON* root, ConnectionReceipt receipt) {
    const auto* type = cJSON_GetObjectItem(root, "type");
    const auto* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "tts") != 0 ||
        !cJSON_IsString(state) || strcmp(state->valuestring, "start") != 0 ||
        !signals || !signals->MatchesSource(source) || chat_protocol_owned_.load() ||
        protocol_generation != protocol_generation_.load() ||
        chat_source_connect_generation_.load() != connect_generation_.load()) return;
    signals->start_audio = {};
    ChatStartHandoff::Request request{source, protocol_generation, connect_generation_.load(), 0,
        receipt.received_us, receipt.admission_deadline_us};
    if (!signals->start.Publish(request)) {
        ESP_LOGW(TAG, "chat_source_fault reason=start_publication");
        signals->PublishConnectionFault(source, chat_source_connect_generation_.load(), ChatProtocolSignals::Error);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        return;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    // The receiver gate serializes following audio. Only app admission is
    // awaited here: decoder cleanup/readiness is deliberately not a condition.
    for (;;) {
        const auto now = static_cast<uint64_t>(esp_timer_get_time());
        ChatStartHandoff::Admission admission;
        if (signals->start.TryAdmission(request, now, admission) && signals->start.Confirm(request, now)) {
            signals->start_audio = {source, protocol_generation, request.connect_generation,
                admission.response_generation, admission.reset_token};
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            return;
        }
        const bool expired = signals->start.Expired(request, now);
        if (expired || !signals->MatchesSource(source) ||
            request.connect_generation != connect_generation_.load() ||
            protocol_generation != protocol_generation_.load() || chat_protocol_owned_.load()) {
            unsigned termination_site = 302;
            if (expired) termination_site = 301;
            const auto elapsed_us = now >= request.received_us ? now - request.received_us : 0;
            ESP_LOGW(TAG, "chat_start_receiver_end site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                termination_site, static_cast<unsigned long>(elapsed_us >> 32),
                static_cast<unsigned long>(static_cast<uint32_t>(elapsed_us)));
            break;
        }
        vTaskDelay(1);
    }
    signals->start.Expire(request);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::HandleChatAudio(const std::shared_ptr<ChatProtocolSignals>& signals,
    uint64_t protocol_generation, ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet) {
    if (!signals || !packet) return;
    const auto response = signals->start_audio;
    if (!signals->MatchesSource(source) || chat_protocol_owned_.load() ||
        response.source.source_id != source.source_id || response.source.connection_epoch != source.connection_epoch ||
        response.protocol_generation != protocol_generation || protocol_generation != protocol_generation_.load() ||
        response.connect_generation != connect_generation_.load() ||
        !response.response_generation || response.response_generation != speaking_generation_.load() ||
        !tts_audio_accepting_.load() || !audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) return;
    packet->generation = response.response_generation;
    packet->conversation_audio = true;
    if (audio_service_.PushChatPacketToDecodeQueue(std::move(packet), response.reset_token) &&
        response.response_generation == speaking_generation_.load() &&
        audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
        last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
    }
}

void Application::RecoverChatStart(const ChatStartHandoff::Request& request, uint32_t site) {
    if (!chat_protocol_signals_ || !chat_protocol_signals_->start.Current(request) ||
        !chat_protocol_signals_->MatchesSource(request.source) || chat_protocol_owned_.load() ||
        request.protocol_generation != protocol_generation_.load() ||
        request.connect_generation != connect_generation_.load() || chat_start_failed_serial_ == request.serial) return;
    ESP_LOGW(TAG, "chat_recovery site=%u", static_cast<unsigned>(site));
    chat_start_failed_serial_ = request.serial;
    chat_rearm_voice_intent_ = false;
    chat_playout_controller_.Cancel();
    chat_playout_recovery_ = true;
    chat_playout_ready_ = false;
    RetireChatOutbound();
    tts_audio_accepting_.store(false);
    auto generation = speaking_generation_.load();
    if (generation != UINT32_MAX) speaking_generation_.store(++generation);
    RequestChatAudioCleanup(generation, true, false, false);
    chat_rearm_phase_ = ChatRearmPhase::Recovery;
    chat_rearm_owner_ = {request.source, request.protocol_generation, request.connect_generation,
        generation, chat_audio_reset_serial_};
    chat_rearm_signals_ = chat_protocol_signals_;
    chat_rearm_source_era_ = chat_protocol_signals_->Capture();
    microphone_uplink_authorized_.store(false);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
}

void Application::PollChatStart(uint64_t now_us) {
    if (!chat_protocol_signals_) return;
    auto& handoff = chat_protocol_signals_->start;
    ChatStartHandoff::Request request;
    if (!handoff.TryRequest(request) || !chat_protocol_signals_->MatchesSource(request.source) ||
        request.protocol_generation != protocol_generation_.load() || chat_protocol_owned_.load() ||
        request.connect_generation != connect_generation_.load()) return;
    // The receiver may publish START after the caller samples the poll clock.
    if (now_us < request.received_us) now_us = static_cast<uint64_t>(esp_timer_get_time());
    if (request.serial == UINT32_MAX || (!handoff.Confirmed(request) && handoff.Expired(request, now_us))) {
        RecoverChatStart(request, 101); return;
    }
    if (handoff.Confirmed(request) && chat_start_handled_serial_ == request.serial &&
        chat_start_failed_serial_ != request.serial && chat_start_effects_serial_ != request.serial) {
        const auto state = GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            RecoverChatStart(request, 102); return;
        }
        chat_start_effects_serial_ = request.serial;
        speaking_arm_dispatch_.BeginResponse(speaking_generation_.load());
        aborted_ = false;
        last_speaking_activity_ms_.store(now_us / 1000);
        SetDeviceState(kDeviceStateSpeaking);
        ArmSpeakingTimeout();
    }
    if (chat_start_handled_serial_ == request.serial || chat_start_failed_serial_ == request.serial) return;
    chat_start_handled_serial_ = request.serial;
    chat_control_intents_.Supersede();
    if (chat_wake_read_serial_ != UINT32_MAX) ++chat_wake_read_serial_;
    chat_wake_read_pending_ = false;
    chat_wake_read_result_.reset();
    if (chat_protocol_infrastructure_fault_ || chat_outbound_fault_ || chat_reboot_audio_requested_ ||
        protocol_work_lifetime_.Pending() || (chat_protocol_fault_ &&
        chat_protocol_fault_generation_ == request.protocol_generation &&
        chat_protocol_fault_era_ == chat_protocol_signals_->Capture())) {
        RecoverChatStart(request, 103); return;
    }
    // Do not collect outbound here: final retirement can invoke protocol work.
    // A single retired generation bounds storage across arbitrarily many STARTs.
    if (chat_outbound_generation_ && chat_outbound_reservation_) {
        chat_start_obsolete_generation_ = chat_outbound_generation_;
        chat_start_obsolete_reservation_ = chat_outbound_reservation_;
        chat_start_obsolete_ack_ = chat_rearm_admitted_ ? chat_rearm_job_ : chat_playout_ack_;
        RetireChatOutbound();
    }
    auto generation = speaking_generation_.load();
    if (generation >= UINT32_MAX - 1) { RecoverChatStart(request, 104); return; }
    speaking_generation_.store(++generation);
    const auto reset = RequestChatPlaybackCleanup(generation);
    if (listening_mode_ != kListeningModeRealtime) {
        microphone_uplink_authorized_.store(false);
        RequestChatAudioCleanup(generation, false, false, false);
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
    }
    const ChatPlayoutIntake::Response response{request.source, request.protocol_generation,
        request.connect_generation, generation, reset};
    tts_audio_accepting_.store(true);
    if (!reset || reset == UINT32_MAX || !EstablishChatPlayoutResponse(response) ||
        !handoff.Admit(request, generation, reset, static_cast<uint64_t>(esp_timer_get_time()))) {
        RecoverChatStart(request, 105); return;
    }
}

bool Application::EstablishChatPlayoutResponse(const ChatPlayoutIntake::Response& response) {
    if (!chat_protocol_signals_ || !chat_protocol_signals_->MatchesSource(response.source) ||
        chat_protocol_owned_.load() || response.protocol_generation != protocol_generation_.load() ||
        response.connect_generation != connect_generation_.load() ||
        response.response_generation != speaking_generation_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(response.reset_token) ||
        chat_playout_unhandled_completion_ ||
        (chat_playout_ack_admitted_ && !chat_playout_ready_ && chat_outbound_reservation_ &&
         chat_start_obsolete_reservation_ != chat_outbound_reservation_)) return false;
    const auto stamp = chat_protocol_signals_->intake.Establish(response);
    if (!stamp) return false;
    const auto state = GetDeviceState();
    const bool transfer_voice_intent = chat_rearm_voice_intent_ &&
        (chat_rearm_phase_ == ChatRearmPhase::Pending || chat_rearm_phase_ == ChatRearmPhase::None ||
         chat_rearm_phase_ == ChatRearmPhase::Armed) &&
        chat_rearm_signals_ == chat_protocol_signals_ &&
        chat_rearm_owner_.source.source_id == response.source.source_id &&
        chat_rearm_owner_.source.connection_epoch == response.source.connection_epoch &&
        chat_rearm_owner_.protocol_generation == response.protocol_generation &&
        chat_rearm_owner_.connect_generation == response.connect_generation &&
        online_intent_.load() && !passive_ws_intent_.load() && !lesson_runtime_active_.load() &&
        (state == kDeviceStateSpeaking || state == kDeviceStateListening);
    chat_rearm_voice_intent_ = transfer_voice_intent;
    chat_rearm_phase_ = ChatRearmPhase::None;
    chat_listen_origin_ = ChatListenOrigin::Drain;
    chat_rearm_owner_ = response;
    chat_rearm_signals_ = chat_protocol_signals_;
    chat_rearm_source_era_ = chat_protocol_signals_->Capture();
    chat_rearm_job_ = {};
    chat_rearm_delivery_.reset();
    chat_rearm_prepared_ = 0;
    chat_rearm_admitted_ = false;
    chat_playout_controller_ = {};
    chat_playout_response_ = response;
    chat_playout_stamp_ = stamp;
    chat_playout_begun_ = chat_playout_ready_ = chat_playout_recovery_ = false;
    chat_playout_cancelled_ = false;
    chat_playout_stop_ = {};
    chat_playout_ack_ = {};
    chat_playout_ack_controller_id_ = 0;
    chat_playout_ack_admitted_ = false;
    return true;
}

void Application::HandleChatTerminalStop(const std::shared_ptr<ChatProtocolSignals>& signals,
    uint64_t protocol_generation, ConnectionSource source, const cJSON* root, uint64_t received_us) {
    if (!received_us) received_us = static_cast<uint64_t>(esp_timer_get_time());
    const auto* type = cJSON_GetObjectItem(root, "type");
    const auto* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "tts") != 0 ||
        !cJSON_IsString(state) || strcmp(state->valuestring, "stop") != 0) return;
    ChatPlayoutIntake::Stop stop;
    if (!signals || !signals->MatchesSource(source) ||
        protocol_generation != protocol_generation_.load() || chat_protocol_owned_.load() ||
        !signals->intake.TryCapture(stop.capture)) return;
    const auto& response = stop.capture.response;
    if (response.source.source_id != source.source_id || response.source.connection_epoch != source.connection_epoch ||
        response.protocol_generation != protocol_generation || response.connect_generation != connect_generation_.load() ||
        response.response_generation != speaking_generation_.load()) return;
    stop.received_us = received_us;
    const auto* reason = cJSON_GetObjectItem(root, "reason");
    stop.interrupt = cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0;
    const auto* resume = cJSON_GetObjectItem(root, "continue_listening");
    const auto* mode = cJSON_GetObjectItem(root, "listen_mode");
    stop.continue_listening = cJSON_IsTrue(resume);
    stop.realtime = cJSON_IsString(mode) && strcmp(mode->valuestring, "realtime") == 0;
    stop.explicit_manual_stop = cJSON_IsFalse(resume) && cJSON_IsString(mode) && strcmp(mode->valuestring, "manual") == 0;
    const auto* id = cJSON_GetObjectItem(root, "drainId");
    // A no-audio server keepalive refreshes an already active listener. It has
    // no drain identity and must not invalidate the previous completed reply.
    if (!id && !reason && stop.continue_listening && stop.realtime &&
        GetDeviceState() == kDeviceStateListening && microphone_uplink_authorized_.load() &&
        !signals->start_audio.reset_token && audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) return;
    // Seal only receiver-owned audio admission. Keep the application-published
    // intake identity intact so duplicate/conflicting STOPs retain their clock.
    signals->start_audio = {};
    if (cJSON_IsString(id)) {
        const size_t size = strnlen(id->valuestring, stop.drain_id.size());
        stop.valid = size > 5 && size <= 128 && strncmp(id->valuestring, "chat:", 5) == 0;
        if (stop.valid) {
            memcpy(stop.drain_id.data(), id->valuestring, size);
            stop.drain_id_size = size;
        }
    }
    stop.reset_captured = audio_service_.IsCurrentChatPlaybackReset(response.reset_token) &&
        audio_service_.TryGetPlaybackResetEpoch(stop.reset_epoch) &&
        audio_service_.IsCurrentChatPlaybackReset(response.reset_token);
    signals->intake.PublishStop(stop);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::RecoverChatPlayout(uint32_t site) {
    if (chat_playout_recovery_) return;
    ESP_LOGW(TAG, "chat_recovery site=%u", static_cast<unsigned>(site));
    if (!chat_playout_stamp_ && chat_protocol_signals_) {
        chat_protocol_signals_->TrySource(chat_playout_response_.source);
        chat_playout_response_.protocol_generation = protocol_generation_.load();
        chat_playout_response_.connect_generation = connect_generation_.load();
        chat_rearm_signals_ = chat_protocol_signals_;
        chat_rearm_source_era_ = chat_protocol_signals_->Capture();
    }
    chat_rearm_voice_intent_ = false;
    chat_playout_ready_ = false;
    chat_playout_recovery_ = true;
    chat_rearm_phase_ = ChatRearmPhase::Recovery;
    chat_playout_controller_.Cancel();
    RetireChatOutbound();
    tts_audio_accepting_.store(false);
    auto generation = speaking_generation_.load();
    if (generation != UINT32_MAX) speaking_generation_.store(++generation);
    RequestChatAudioCleanup(generation, true, false, false);
    chat_rearm_owner_ = chat_playout_response_;
    chat_rearm_owner_.response_generation = generation;
    chat_rearm_owner_.reset_token = chat_audio_reset_serial_;
    microphone_uplink_authorized_.store(false);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
}

void Application::PollChatPlayout(uint64_t now_us) {
    using Controller = ConversationPlayoutController;
    using Result = ChatOutboundMailbox::Result;
    if (!chat_playout_stamp_ || chat_playout_recovery_) return;
    const auto& response = chat_playout_response_;
    if (!chat_protocol_signals_ || !chat_protocol_signals_->intake.Current(chat_playout_stamp_) ||
        !chat_protocol_signals_->MatchesSource(response.source) || chat_protocol_owned_.load() ||
        response.protocol_generation != protocol_generation_.load() ||
        response.connect_generation != connect_generation_.load() ||
        response.response_generation != speaking_generation_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
        // Source/intent loss cancels the same live response immediately, but
        // obsolete drain work must not reset a successor response or reset token.
        if (response.response_generation == speaking_generation_.load() &&
            audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
            RecoverChatPlayout(201); return;
        }
        chat_playout_controller_.Cancel();
        if (chat_playout_ack_admitted_ && chat_playout_ack_.generation == chat_outbound_generation_)
            RetireChatOutbound();
        chat_playout_ready_ = false;
        chat_playout_cancelled_ = true;
        chat_playout_stamp_ = 0;
        return;
    }
    // The original clock remains owned even while mailbox/audio/outbound work
    // is Busy. Ready retains this same clock for the later listen-state consumer.
    if (chat_listen_origin_ == ChatListenOrigin::Drain && chat_playout_begun_ && chat_rearm_phase_ != ChatRearmPhase::Armed &&
        chat_rearm_phase_ != ChatRearmPhase::IdleComplete &&
        now_us - chat_playout_stop_.received_us >= Controller::kTimeoutUs) {
        RecoverChatPlayout(202); return;
    }
    ChatPlayoutIntake::Stop stop;
    // Local listen/abort ownership must not consume the retained server STOP.
    // A confirmed server START establishes a new Drain origin.
    const auto read = chat_listen_origin_ == ChatListenOrigin::Drain ?
        chat_protocol_signals_->intake.TryCollect(chat_playout_stamp_, stop) : ChatPlayoutIntake::Read::None;
    if (read == ChatPlayoutIntake::Read::Fault) { RecoverChatPlayout(203); return; }
    if (read == ChatPlayoutIntake::Read::Ready && (stop.interrupt || stop.conflict)) {
        chat_playout_stop_ = stop;
        RecoverChatPlayout(204); return;
    }
    if (!chat_playout_begun_ && read == ChatPlayoutIntake::Read::Ready) {
        chat_playout_stop_ = stop;
        // Observe time after a newer STOP without moving its original deadline.
        if (now_us < stop.received_us) now_us = static_cast<uint64_t>(esp_timer_get_time());
        if (now_us - stop.received_us >= Controller::kTimeoutUs) { RecoverChatPlayout(205); return; }
        const Controller::Ownership owner{response.source.connection_epoch,response.response_generation,stop.reset_epoch,false};
        const Controller::Token token{owner.connection_epoch,owner.response_generation,owner.reset_epoch,
                                     {stop.drain_id.data(),stop.drain_id_size}};
        if (!stop.valid || !stop.reset_captured || stop.interrupt ||
            !chat_playout_controller_.Begin(stop.received_us,token,owner).accepted) {
            RecoverChatPlayout(206); return;
        }
        chat_playout_begun_ = true;
    }
    if (chat_outbound_fault_ || chat_playback_fault_ || chat_protocol_infrastructure_fault_ || (chat_protocol_fault_ &&
        chat_protocol_fault_generation_ == response.protocol_generation &&
        chat_protocol_fault_era_ == chat_protocol_signals_->Capture())) {
        RecoverChatPlayout(207); return;
    }
    if (!chat_playout_unhandled_completion_) {
        ChatOutboundMailbox::Completion completion;
        if (PollChatOutbound(&completion)) {
            if (chat_start_obsolete_generation_ && completion.job.generation == chat_start_obsolete_generation_) {
                // Retain obsolete identity until the worker's final retirement;
                // its completion can never advance the successor controller.
            } else if (completion.job.kind == ChatOutboundMailbox::Kind::ListenStart &&
                chat_rearm_admitted_ && completion.job.request_id == chat_rearm_job_.request_id &&
                completion.job.generation == chat_rearm_job_.generation &&
                completion.job.protocol_generation == chat_rearm_job_.protocol_generation &&
                completion.job.connection_epoch == chat_rearm_job_.connection_epoch) {
                chat_rearm_delivery_ = completion;
                if (!IsChatOutboundCompletionCurrent(completion) || completion.result != Result::Sent) {
                    RecoverChatPlayout(208); return;
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
            } else if (completion.job.kind != ChatOutboundMailbox::Kind::DrainAck ||
                !chat_playout_ack_admitted_ || completion.job.request_id != chat_playout_ack_.request_id) {
                chat_playout_unhandled_completion_ = completion;
                RecoverChatPlayout(209); return;
            } else {
                const auto delivery = !IsChatOutboundCompletionCurrent(completion) ? Controller::Delivery::Stale :
                    completion.result == Result::Sent ? Controller::Delivery::Sent :
                    completion.result == Result::Busy ? Controller::Delivery::Busy :
                    completion.result == Result::Stale ? Controller::Delivery::Stale : Controller::Delivery::Failed;
                chat_playout_controller_.Deliver(chat_playout_ack_controller_id_,delivery);
                // Delivery retires the physical ACK even if drain observation is Busy.
                chat_playout_ack_admitted_ = false;
                chat_playout_ack_controller_id_ = 0;
            }
        }
        if (chat_outbound_fault_) { RecoverChatPlayout(210); return; }
    }
    if (chat_start_obsolete_reservation_) {
        if (chat_outbound_reservation_ == chat_start_obsolete_reservation_) return;
        chat_start_obsolete_reservation_ = 0;
        chat_start_obsolete_generation_ = 0;
        chat_start_obsolete_ack_ = {};
        if (ActivateChatOutbound(response.source.connection_epoch) != Result::Sent) {
            RecoverChatPlayout(211); return;
        }
    }
    if (chat_playout_ready_ || chat_rearm_phase_ == ChatRearmPhase::Pending)
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    if (chat_listen_origin_ != ChatListenOrigin::Drain || !chat_playout_begun_ || chat_playout_ready_ || chat_rearm_phase_ == ChatRearmPhase::Armed ||
        chat_rearm_phase_ == ChatRearmPhase::IdleComplete) return;
    PlaybackDrainSnapshot snapshot;
    const bool observed = audio_service_.TryGetPlaybackDrainSnapshot(snapshot);
    const Controller::Ownership owner{response.source.connection_epoch,response.response_generation,chat_playout_stop_.reset_epoch,false};
    const auto effect = chat_playout_controller_.Poll(now_us,owner,observed ? &snapshot : nullptr);
    if (effect.kind == Controller::EffectKind::Complete) {
        chat_playout_ready_ = true;
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return;
    }
    if (effect.kind == Controller::EffectKind::Cancel || effect.kind == Controller::EffectKind::Recover) {
        RecoverChatPlayout(212); return;
    }
    if (effect.kind == Controller::EffectKind::SubmitAck) {
        if (chat_playout_stop_.received_us > UINT64_MAX - Controller::kTimeoutUs) { RecoverChatPlayout(213); return; }
        chat_playout_ack_ = {};
        chat_playout_ack_.kind = ChatOutboundMailbox::Kind::DrainAck;
        chat_playout_ack_.deadline_us = chat_playout_stop_.received_us + Controller::kTimeoutUs;
        chat_playout_ack_.SetPayload(effect.drain_id.data(),effect.drain_id_size);
        chat_playout_ack_controller_id_ = effect.request_id;
        chat_playout_ack_admitted_ = false;
    }
    if (chat_playout_ack_controller_id_ && !chat_playout_ack_admitted_) {
        const auto admitted = SubmitChatOutbound(chat_playout_ack_);
        if (admitted == Result::Sent) chat_playout_ack_admitted_ = true;
        else if (admitted != Result::Busy) RecoverChatPlayout(214);
    }
}

bool Application::SelectChatProtocolSource(ConnectionSource source, uint64_t protocol_generation,
                                          uint32_t connect_generation) {
    // Caller proves current successful Open/source identity under lifetime protection. No socket
    // lookup or implicit activation can turn an old callback into a new source.
    if (!chat_protocol_signals_ || chat_protocol_owned_.load(std::memory_order_acquire) ||
        protocol_generation != protocol_generation_.load() ||
        connect_generation != connect_generation_.load()) return false;
    chat_source_connect_generation_.store(connect_generation);
    if (!chat_protocol_signals_->EnableForSource(source)) return false;
    chat_start_handled_serial_ = chat_start_failed_serial_ = chat_start_effects_serial_ = 0;
    return true;
}

void Application::PollChatSourceOpen(uint64_t now_us) {
    if (chat_recovery_.kind != ChatRecoveryIntent::Kind::None &&
        chat_recovery_.lesson_generation != lesson_runtime_generation_.load()) CancelChatRecovery();
    if (!chat_protocol_signals_) return;
    ChatProtocolSignals::Opened opened;
    if (!chat_protocol_signals_->ReadOpened(opened) || opened.source.source_id <= chat_source_open_handled_) return;
    if (!protocol_ || opened.connect_generation != connect_generation_.load() ||
        IsConnectSuccessPublicationSuppressed()) {
        chat_source_open_handled_ = opened.source.source_id;
        return;
    }
    if (opened.deadline_us && now_us >= opened.deadline_us) {
        chat_source_open_handled_ = opened.source.source_id;
        ESP_LOGW(TAG, "chat_source_fault reason=open_deadline");
        chat_protocol_signals_->PublishConnectionFault(opened.source, opened.connect_generation, ChatProtocolSignals::Error);
        return;
    }
    if (chat_protocol_owned_.load()) return;
    chat_source_open_handled_ = opened.source.source_id;
    if (protocol_->CurrentConnectionEpoch() != opened.source.connection_epoch) return;
    if (!SelectChatProtocolSource(opened.source, protocol_generation_.load(), opened.connect_generation)) return;
    if (chat_recovery_.kind != ChatRecoveryIntent::Kind::None && chat_recovery_.attempted &&
        chat_recovery_.protocol_generation == protocol_generation_.load() &&
        chat_recovery_.connect_generation == opened.connect_generation) {
        chat_recovery_.adopted = true;
        ESP_LOGI(TAG, "chat_recovery outcome=4");
    }
    backend_recovery_window_.Reset();
    if (passive_ws_intent_.load()) {
        online_intent_.store(false);
        microphone_uplink_authorized_.store(false);
        if (IsDeviceClaimed() && !lesson_runtime_active_.load()) {
            StartHeartbeat();
            DispatchDeviceHeartbeat();
        } else StopHeartbeat();
    } else {
        const bool lesson_answer_turn = lesson_interactive_listen_pending_.load() || lesson_interactive_listening_active_.load();
        if (lesson_runtime_active_.load() && !lesson_answer_turn) {
            online_intent_.store(false);
            StopHeartbeat();
            return;
        }
        online_intent_.store(true);
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    }
    backend_offline_.store(false);
    DismissAlert();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (IsDeviceClaimed()) StopClaimPoll();
    if (opened.sample_rate != board.GetAudioCodec()->output_sample_rate())
        ESP_LOGW(TAG, "Server sample rate %d differs from device output rate", opened.sample_rate);
}

void Application::PollChatProtocolSignals() {
    if (!chat_protocol_signals_) return;
    uint32_t flags = 0;
    chat_protocol_signals_->Collect(flags);
    ChatProtocolSignals::Failure failure;
    const bool current_failure = chat_protocol_signals_->ReadFailure(failure) && failure.connect_generation == connect_generation_.load();
    if (current_failure)
        flags |= failure.flags;
    if (!flags) return;
    if (chat_protocol_signals_->SourceSelected() &&
        (chat_source_connect_generation_.load() != connect_generation_.load() ||
         chat_protocol_owned_.load(std::memory_order_acquire))) return;
    chat_protocol_fault_ = true;
    chat_protocol_fault_generation_ = protocol_generation_.load();
    chat_protocol_fault_era_ = chat_protocol_signals_->Capture();
    if (chat_cleanup_enabled_ && current_failure &&
        failure.source.source_id > chat_source_failure_handled_) {
        chat_source_failure_handled_ = failure.source.source_id;
        HandleChatSourceFailure();
    }
    // Task 4b consumes this retained fault with the current chat intent. Never
    // call legacy recovery/audio side effects from a transport callback.
}

void Application::HandleChatSourceFailure() {
    if (!lesson_runtime_active_.load() && !passive_ws_intent_.load() && online_intent_.load() &&
        RetainChatRecovery(ChatRecoveryIntent::Kind::Background, GetDefaultListeningMode()) &&
        chat_recovery_.kind == ChatRecoveryIntent::Kind::Background && chat_rearm_phase_ == ChatRearmPhase::Pending &&
        (chat_listen_origin_ == ChatListenOrigin::Wake || chat_listen_origin_ == ChatListenOrigin::User)) {
        chat_recovery_.kind = chat_listen_origin_ == ChatListenOrigin::Wake ?
            ChatRecoveryIntent::Kind::Wake : ChatRecoveryIntent::Kind::Listen;
        chat_recovery_.received_us = chat_listen_received_us_;
        chat_recovery_.deadline_us = chat_rearm_job_.deadline_us;
        chat_recovery_.mode = chat_rearm_mode_;
        if (const auto* wake = chat_control_intents_.Front(); wake && wake->job.kind == ChatOutboundMailbox::Kind::Wake) {
            chat_recovery_.read_wake = wake->resolve_wake;
            chat_recovery_.wake_text = wake->job.payload;
            chat_recovery_.wake_size = wake->job.payload_size;
        }
    }
    chat_rearm_voice_intent_ = false;
    chat_rearm_phase_ = ChatRearmPhase::None;
    tts_audio_accepting_.store(false);
    microphone_uplink_authorized_.store(false);
    speaking_arm_dispatch_.Cancel();
    RetireChatOutbound();
    auto generation = speaking_generation_.load();
    if (generation != UINT32_MAX) speaking_generation_.store(++generation);
    RequestChatAudioCleanup(generation, true, false, false);
    RequestLessonStorageAbandonment();
    backend_offline_.store(true);
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    const auto state = GetDeviceState();
    if (state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting) return;
    if (connect_in_flight_.load()) return;
    deferred_close_generation_ = 0;
    protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
    PollChatProtocolCleanup();
    if (!lesson_runtime_active_.load() || !passive_ws_intent_.load()) SetDeviceState(kDeviceStateIdle);
    if (ShouldKeepManagementHeartbeat()) {
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    } else StopHeartbeat();
    auto* display = board.GetDisplay();
    if (!lesson_runtime_active_.load()) display->SetChatMessage("system", "");
    if (passive_ws_intent_.load()) {
        if (!reconnect_passive_.load()) SchedulePassiveLessonReconnect();
        return;
    }
    if (!online_intent_.load()) return;
    if (lesson_runtime_active_.load()) {
        online_intent_.store(false);
        lesson_interactive_listen_generation_.fetch_add(1);
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
        return;
    }
    display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
    display->SetEmotion("thinking");
    RequestChatCue(Lang::Sounds::OGG_EXCLAMATION);
    ScheduleReconnect(GetDefaultListeningMode(), false);
}

ChatOutboundMailbox::Result Application::ActivateChatOutbound(uint32_t connection_epoch) {
    using Result = ChatOutboundMailbox::Result;
    if (!chat_outbound_task_ || !protocol_ || !connection_epoch ||
        !protocol_generation_.load()) return Result::Failed;
    if (chat_outbound_reservation_ || protocol_work_lifetime_.Pending() ||
        protocol_work_lifetime_.Busy()) return Result::Busy;
    const auto reservation = protocol_work_lifetime_.Reserve();
    if (!reservation) return Result::Busy;
    const auto generation = chat_outbound_worker_.AdvanceGeneration();
    const auto protocol_generation = protocol_generation_.load();
    const ChatOutboundWorker::Activation activation{
        protocol_.get(), protocol_generation, connection_epoch, generation, this,
        [](void* context) {
            return static_cast<Application*>(context)->audio_service_.PopPacketFromSendQueue();
        },
        [](void* context, const AudioStreamPacket& packet) {
            return static_cast<Application*>(context)->audio_service_.IsCurrentChatUplink(packet);
        },
        [](void* context) {
            auto* app = static_cast<Application*>(context);
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        },
        [](void* context, ConnectionSource source, uint64_t protocol, uint32_t connect) {
            return static_cast<Application*>(context)->IsChatConnectionCurrent(source, protocol, connect);
        }};
    if (!chat_outbound_worker_.Publish(activation)) {
        protocol_work_lifetime_.Release(reservation);
        chat_outbound_fault_ = true;
        return Result::Failed;
    }
    chat_outbound_reservation_ = reservation;
    chat_outbound_generation_ = generation;
    chat_outbound_protocol_generation_ = protocol_generation;
    chat_outbound_connection_epoch_ = connection_epoch;
    chat_outbound_fault_ = false;
    NotifyChatOutbound();
    return Result::Sent;
}

ChatOutboundMailbox::Result Application::SubmitChatOutbound(ChatOutboundMailbox::Job& job) {
    using Result = ChatOutboundMailbox::Result;
    if (!chat_outbound_task_ || chat_outbound_fault_) return Result::Failed;
    if (!chat_outbound_reservation_ || !chat_outbound_generation_ ||
        protocol_work_lifetime_.Pending()) return Result::Stale;
    if (job.request_id == 0) {
        if (chat_outbound_request_id_ == UINT64_MAX) {
            chat_outbound_fault_ = true;
            RetireChatOutbound();
            return Result::Failed;
        }
        job.request_id = ++chat_outbound_request_id_;
        job.generation = chat_outbound_generation_;
        job.protocol_generation = chat_outbound_protocol_generation_;
        job.connection_epoch = chat_outbound_connection_epoch_;
        if (!job.deadline_us) {
            if (job.kind == ChatOutboundMailbox::Kind::DrainAck) return Result::Failed;
            job.deadline_us = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
        }
    }
    if (job.generation != chat_outbound_generation_ ||
        job.request_id <= chat_outbound_last_admitted_id_ ||
        job.request_id > chat_outbound_request_id_ ||
        job.protocol_generation != chat_outbound_protocol_generation_ ||
        job.connection_epoch != chat_outbound_connection_epoch_) return Result::Stale;
    if (!job.deadline_us || static_cast<uint64_t>(esp_timer_get_time()) >= job.deadline_us) return Result::Failed;
    if (!chat_outbound_worker_.Submit(job)) return Result::Busy;
    chat_outbound_last_admitted_id_ = job.request_id;
    NotifyChatOutbound();
    return Result::Sent;
}

void Application::RetireChatOutbound() {
    if (!chat_outbound_reservation_ || !chat_outbound_generation_) return;
    chat_outbound_worker_.AdvanceGeneration();
    chat_outbound_generation_ = 0;
    NotifyChatOutbound();
}

bool Application::PollChatOutbound(ChatOutboundMailbox::Completion* completion) {
    bool collected = false;
    if (chat_outbound_worker_.TakeAudioFailure()) {
        chat_outbound_fault_ = true;
        RetireChatOutbound();
    }
    if (protocol_work_lifetime_.Pending()) RetireChatOutbound();
    ChatOutboundMailbox::Completion discarded;
    if (completion || !chat_outbound_generation_ || chat_control_intents_.Size() || chat_connection_messages_.Size()) {
        collected = chat_outbound_worker_.Collect(completion ? *completion : discarded);
        if (collected) NotifyChatOutbound();
        if (collected && (chat_connection_messages_.Deliver(completion ? *completion : discarded) ||
            DeliverChatControl(completion ? *completion : discarded))) collected = false;
    }
    if (chat_outbound_reservation_ && chat_outbound_worker_.TakeRetired()) {
        chat_connection_messages_.ObserveRetirement(chat_outbound_reservation_);
        protocol_work_lifetime_.Release(chat_outbound_reservation_);
        chat_outbound_reservation_ = 0;
        CompletePendingProtocolWork();
    }
    return collected;
}

bool Application::IsChatOutboundCompletionCurrent(const ChatOutboundMailbox::Completion& completion) const {
    return !completion.stale && chat_outbound_generation_ != 0 &&
        protocol_ && protocol_->CurrentConnectionEpoch() == completion.job.connection_epoch &&
        chat_outbound_worker_.IsCurrent(completion.job.generation) &&
        completion.job.protocol_generation == protocol_generation_.load() &&
        completion.job.protocol_generation == chat_outbound_protocol_generation_ &&
        completion.job.connection_epoch == chat_outbound_connection_epoch_ &&
        !protocol_work_lifetime_.Pending();
}

void Application::NotifyChatOutbound() {
    if (chat_outbound_task_) xTaskNotifyGive(chat_outbound_task_);
}

void Application::PollChatOutboundEvents(uint32_t bits) {
    if (bits & MAIN_EVENT_CLOCK_TICK) ++clock_ticks_;
    if (bits & MAIN_EVENT_CLOCK_TICK) McpServer::GetInstance().PollLessonAssetSyncCompletion();
    if (bits & (MAIN_EVENT_CHAT_OUTBOUND | MAIN_EVENT_CLOCK_TICK)) {
        PollChatSourceOpen(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatProtocolSignals();
        PollChatStart(static_cast<uint64_t>(esp_timer_get_time()));
        if (chat_playout_stamp_ && !chat_playout_recovery_) PollChatPlayout(static_cast<uint64_t>(esp_timer_get_time()));
        else PollChatOutbound();
        PollChatControls(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatConnectionMessages(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatUnpair(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatInboundMessages();
        if (chat_cleanup_enabled_) {
            PollChatAudioCleanup();
            PollChatLessonCapture(static_cast<uint64_t>(esp_timer_get_time()));
            PollChatProtocolCleanup();
            if (bits & MAIN_EVENT_CLOCK_TICK) RetryChatAudioCleanup();
            PollChatReboot();
            PollChatRecovery(static_cast<uint64_t>(esp_timer_get_time()));
        }
    }
}

void Application::ChatOutboundTask(void* context) {
    auto* app = static_cast<Application*>(context);
    for (;;) {
        const bool retry = app->chat_outbound_worker_.RunOnce(esp_timer_get_time());
        ulTaskNotifyTake(pdTRUE, retry ? pdMS_TO_TICKS(10) : portMAX_DELAY);
    }
}

void Application::StartProtocolWorker() {
    auto* ctx = new ConnectContext{this, GetDefaultListeningMode(),
        connect_generation_.load(), std::string()};
    ctx->start_protocol = true;
    if (!StartOpenChannelWorker(ctx)) {
        delete ctx;
        protocol_start_pending_generation_ = protocol_generation_.load();
        return;
    }
    protocol_start_pending_generation_ = 0;
}

void Application::OpenChannelTask(void* arg) {
    auto* self = static_cast<Application*>(arg);
    for (;;) {
        NetworkWorkItem work{};
        if (xQueueReceive(open_channel_queue, &work, portMAX_DELAY) != pdTRUE ||
            work.context == nullptr) {
            continue;
        }
        if (work.kind == NetworkWorkKind::kHeartbeat) {
            Application::HeartbeatTask(work.context);
            continue;
        }
        if (work.kind == NetworkWorkKind::kProtocolCleanup) {
            self->RunChatProtocolCleanup();
            continue;
        }
        auto* ctx = static_cast<ConnectContext*>(work.context);
        ListeningMode mode = ctx->mode;
        uint32_t gen = ctx->generation;
        std::string wake_word = ctx->wake_word;
        bool wake_word_invoke = ctx->wake_word_invoke;
        bool passive_preconnect = ctx->passive_preconnect;
        Protocol* worker_protocol = ctx->protocol;
        const uint64_t worker_protocol_generation = ctx->protocol_generation;
        const uint64_t reservation = ctx->reservation;
        const bool start_protocol = ctx->start_protocol;
        delete ctx;
        self->protocol_callback_connect_generation_.store(gen);

    if (self->protocol_work_lifetime_.Pending() ||
        (!start_protocol && gen != self->connect_generation_.load())) {
        self->Schedule([self, reservation, start_protocol, worker_protocol_generation, gen]() {
            if (!self->protocol_work_lifetime_.Release(reservation)) return;
            if (!start_protocol) self->CompleteChatRecoveryOpen(gen, false);
            if (start_protocol && worker_protocol_generation == self->protocol_generation_.load()) {
                // A close may cancel queued audio work, but control startup is
                // still owed unless the drain resets/replaces this protocol.
                self->protocol_start_pending_generation_ = worker_protocol_generation;
            }
            self->CompletePendingProtocolWork();
        });
        continue;
    }

    if (start_protocol) {
        worker_protocol->Start();
        self->Schedule([self, reservation, worker_protocol_generation]() {
            if (!self->protocol_work_lifetime_.Release(reservation)) return;
            if (self->CompletePendingProtocolWork()) return;
            if (worker_protocol_generation == self->protocol_generation_.load()) {
                self->CompleteProtocolActivation();
            }
        });
        continue;
    }

    // The ONLY blocking call, now off the app task.
    bool ok = false;
    int max_attempts = wake_word_invoke ? kWakeWordAudioChannelOpenMaxAttempts : 1;
    for (int attempt = 1;
         !self->protocol_work_lifetime_.Pending() &&
         gen == self->connect_generation_.load() &&
         !worker_protocol->IsAudioChannelOpened() && attempt <= max_attempts;
         ++attempt) {
        worker_protocol->SetIncomingJsonTransportEpoch(
            self->lesson_transport_epoch_gate_.PublishedEpoch());
        ok = worker_protocol->OpenAudioChannel();
        if (ok) {
            break;
        }
        if (wake_word_invoke) {
            ESP_LOGW(TAG, "wake_audio_channel_open_failed attempt=%d max=%d", attempt,
                     kWakeWordAudioChannelOpenMaxAttempts);
        }
        if (wake_word_invoke && attempt < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(kWakeWordAudioChannelRetryDelayMs));
        }
    }
    if (worker_protocol->IsAudioChannelOpened()) {
        ok = true;
    }
    self->Schedule([self, ok, mode, gen, wake_word, wake_word_invoke, passive_preconnect,
                    reservation, worker_protocol_generation]() {
            if (!self->protocol_work_lifetime_.Release(reservation)) return;
            if (gen == self->connect_generation_.load()) {
                self->connect_in_flight_.store(false);
                self->CancelConnectWatchdog();
            }
            if (self->protocol_work_lifetime_.Pending()) self->CompleteChatRecoveryOpen(gen, false);
            if (self->CompletePendingProtocolWork()) return;
            if (gen != self->connect_generation_.load() ||
                worker_protocol_generation != self->protocol_generation_.load()) {
                return;  // superseded by a newer connect or the watchdog
            }
            if (self->CompleteChatRecoveryOpen(gen, ok)) return;
            if (!passive_preconnect) {
                const DeviceState state = self->GetDeviceState();
                if (wake_word_invoke) {
                    if (state != kDeviceStateConnecting && state != kDeviceStateIdle) {
                        return;
                    }
                } else if (state != kDeviceStateConnecting) {
                    return;
                }
            }
            if (ok) {
                self->backend_offline_.store(false);
                self->reconnect_attempt_ = 0;
                self->connect_attempt_active_.store(
                    false);  // WSS-8: connect cycle resolved (success)
                if (passive_preconnect) {
                    self->passive_reconnect_attempt_ = 0;
                    self->reconnect_passive_.store(false);
                    ESP_LOGI(TAG, "passive_lesson_websocket_opened");
                    const bool lesson_answer_turn =
                        self->lesson_interactive_listen_pending_.load() ||
                        self->lesson_interactive_listening_active_.load();
                    if (self->lesson_runtime_active_.load() && lesson_answer_turn) {
                        self->passive_ws_intent_.store(false);
                        self->StartHeartbeat();
                        self->DispatchDeviceHeartbeat();
                        self->SetListeningMode(kListeningModeManualStop);
                    } else if (self->IsDeviceClaimed() && !self->lesson_runtime_active_.load()) {
                        if (!self->lesson_asset_sync_quiet_.load()) {
                            const std::string deferred_wake_word = self->deferred_wake_word_;
                            self->deferred_wake_word_.clear();
                            if (!deferred_wake_word.empty()) {
                                ESP_LOGI(TAG, "passive_lesson_deferred_wake_resumed");
                                self->FinishWakeWordInvoke(deferred_wake_word);
                            } else {
                                // Give the server's initial asset burst time to
                                // enter quiet mode before allocating the AFE.
                                self->ScheduleLessonAssetSyncWakeRearm(5000ULL * 1000ULL);
                            }
                        }
                    }
                } else if (wake_word_invoke) {
                    self->FinishWakeWordInvoke(wake_word);
                } else {
                    const bool lesson_answer_turn =
                        self->lesson_interactive_listen_pending_.load() ||
                        self->lesson_interactive_listening_active_.load();
                    if (self->lesson_runtime_active_.load() && !lesson_answer_turn) {
                        ESP_LOGI(TAG, "lesson open worker ignored state=%d",
                                 static_cast<int>(self->GetDeviceState()));
                        self->online_intent_.store(false);
                        return;
                    }
                    if (self->reconnect_resume_listening_.exchange(true)) {
                        self->SetListeningMode(mode);
                    } else {
                        self->SetDeviceState(kDeviceStateIdle);
                    }
                }
            } else {
                const bool lesson_answer_turn = self->lesson_interactive_listen_pending_.load() ||
                                                self->lesson_interactive_listening_active_.load();
                if (self->lesson_runtime_active_.load()) {
                    if (lesson_answer_turn || (!passive_preconnect && !wake_word_invoke)) {
                        ESP_LOGW(TAG, "lesson open_audio_channel_failed -> wait");
                        self->backend_offline_.store(true);
                        self->passive_ws_intent_.store(false);
                        self->online_intent_.store(false);
                        self->connect_attempt_active_.store(false);
                        if (!lesson_answer_turn) {
                            self->lesson_interactive_listen_generation_.fetch_add(1);
                            self->lesson_interactive_listen_pending_.store(false);
                            self->lesson_interactive_listening_active_.store(false);
                        }
                        self->lesson_idle_repaint_suppressed_.store(true);
                        if (self->GetDeviceState() == kDeviceStateConnecting) {
                            self->SetDeviceState(kDeviceStateIdle);
                        }
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetStatus(Lang::Strings::PLEASE_WAIT);
                        if (lesson_answer_turn) {
                            self->SchedulePassiveLessonReconnect();
                        }
                        return;
                    }
                }
                if (passive_preconnect) {
                    ESP_LOGW(TAG, "passive_lesson_websocket_failed");
                    self->deferred_wake_word_.clear();
                    self->passive_ws_intent_.store(false);
                    if (self->GetDeviceState() == kDeviceStateConnecting) {
                        self->SetDeviceState(kDeviceStateIdle);
                    }
                    if (self->ShouldKeepManagementHeartbeat()) {
                        self->StartHeartbeat();
                        self->DispatchDeviceHeartbeat();
                    }
                    self->RearmClaimedIdleWakeWord();
                    self->SchedulePassiveLessonReconnect();
                } else if (wake_word_invoke) {
                    ESP_LOGW(TAG, "wake_audio_channel_open_failed -> idle");
                } else {
                    ESP_LOGW(TAG, "open_audio_channel_failed -> idle + backoff");
                }
                self->backend_offline_.store(true);
                if (wake_word_invoke) {
                    self->audio_service_.EnableWakeWordDetection(true);
                }
                if (self->GetDeviceState() == kDeviceStateConnecting) {
                    self->SetDeviceState(kDeviceStateIdle);
                }
                if (!wake_word_invoke && !passive_preconnect) {
                    self->ScheduleReconnect(
                        mode,
                        self->reconnect_resume_listening_.load());  // WSS-4: long-horizon retry
                } else if (wake_word_invoke) {
                    // WSS-8: the wake open-loop exhausted all attempts -> terminal.
                    // Per-attempt errors were suppressed (connect_attempt_active_),
                    // so surface the offline banner exactly once now.
                    self->connect_attempt_active_.store(false);
                    xEventGroupSetBits(self->event_group_, MAIN_EVENT_ERROR);
                }
            }
        });
    }
}

void Application::ArmConnectWatchdog() {
    if (connect_watchdog_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            uint32_t gen = self->connect_generation_.load();
            self->Schedule([self, gen]() { self->HandleConnectWatchdog(gen); });
        };
        args.arg = this;
        args.name = "connect_wdt";
        if (esp_timer_create(&args, &connect_watchdog_timer_) != ESP_OK) {
            connect_watchdog_timer_ = nullptr;
            return;
        }
    }
    esp_timer_stop(connect_watchdog_timer_);
    // OpenAudioChannel can spend up to 10s waiting for server hello after the
    // socket connect. Wake-word invokes may retry; the watchdog must outlive that
    // budget or the first valid "Hi ESP" is reset to Idle before success returns.
    esp_timer_start_once(connect_watchdog_timer_, kConnectWatchdogTimeoutUs);
}

void Application::CancelConnectWatchdog() {
    if (connect_watchdog_timer_ != nullptr) {
        esp_timer_stop(connect_watchdog_timer_);
    }
}

void Application::HandleConnectWatchdog(uint32_t generation) {
    if (generation != connect_generation_.load()) {
        return;  // connect already resolved
    }
    const bool recovery_watchdog = chat_recovery_.opening &&
        chat_recovery_.connect_generation == generation &&
        chat_recovery_.protocol_generation == protocol_generation_.load() &&
        chat_recovery_.lesson_generation == lesson_runtime_generation_.load();
    // Invalidate the still-running worker's eventual result, recover to Idle and
    // schedule a backoff retry. WebsocketProtocol keeps each open candidate
    // private until it has connected and received hello, so a timed-out passive
    // worker can no longer own reconnect forever.
    ++connect_generation_;
    connect_in_flight_.store(false);  // Attempt expired; reservation still owns the worker.
    if (chat_recovery_.opening && chat_recovery_.connect_generation == generation &&
        chat_recovery_.protocol_generation == protocol_generation_.load()) {
        chat_recovery_.connect_generation = connect_generation_.load();
        chat_recovery_.opening = chat_recovery_.adopted = false;
    }
    if (passive_ws_intent_.load()) {
        deferred_wake_word_.clear();
        const bool lesson_answer_turn =
            lesson_interactive_listen_pending_.load() ||
            lesson_interactive_listening_active_.load();
        if (lesson_runtime_active_.load() && lesson_answer_turn) {
            ESP_LOGW(TAG, "lesson passive connect watchdog timeout -> wait");
            backend_offline_.store(true);
            passive_ws_intent_.store(false);
            online_intent_.store(false);
            connect_attempt_active_.store(false);
            connect_in_flight_.store(false);
            auto display = Board::GetInstance().GetDisplay();
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
            lesson_idle_repaint_suppressed_.store(true);
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
            SchedulePassiveLessonReconnect();
            return;
        }
        ESP_LOGW(TAG, "passive_lesson_connect_watchdog_timeout -> passive backoff");
        backend_offline_.store(true);
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);
        if (GetDeviceState() == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        passive_ws_intent_.store(false);
        RearmClaimedIdleWakeWord();
        SchedulePassiveLessonReconnect();
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGW(TAG, "lesson connect watchdog timeout -> suppress generic reconnect");
        RequestLessonStorageAbandonment();
        backend_offline_.store(true);
        online_intent_.store(false);
        connect_attempt_active_.store(false);
        lesson_interactive_listen_generation_.fetch_add(1);
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
        lesson_idle_repaint_suppressed_.store(true);
        if (GetDeviceState() == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }
    // Explicit recovery expiry already made the UI Idle while the worker still
    // owned its reservation. Its watchdog must retain the background retry.
    if (recovery_watchdog && GetDeviceState() == kDeviceStateIdle && online_intent_.load() &&
        !reset_pending_.load() && !protocol_reinit_pending_.load() && !reboot_pending_.load()) {
        backend_offline_.store(true);
        ScheduleReconnect(reconnect_mode_, false);
        RearmClaimedIdleWakeWord();
        return;
    }
    if (GetDeviceState() == kDeviceStateConnecting) {
        ESP_LOGW(TAG, "connect_watchdog_timeout -> idle + backoff");
        backend_offline_.store(true);
        SetDeviceState(kDeviceStateIdle);
        ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());
    }
}

void Application::ScheduleReconnect(ListeningMode mode, bool resume_listening) {
    static constexpr int kFastReconnectAttempts = 6;
    static constexpr uint32_t kSlowReconnectRetryMs = 30000;
    reconnect_mode_ = mode;
    reconnect_resume_listening_.store(resume_listening);
    reconnect_passive_.store(false);
    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->HandleReconnectTick(); });
        };
        args.arg = this;
        args.name = "reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            return;
        }
    }
    uint32_t delay_ms = 0;
    if (reconnect_attempt_ < kFastReconnectAttempts) {
        // Fast recovery window: exponential backoff 0.5s -> 8s cap, plus
        // 0..50% jitter (anti fleet-sync).
        uint32_t base_ms = 500u << reconnect_attempt_;
        if (base_ms > 8000u) {
            base_ms = 8000u;
        }
        uint32_t jitter_ms = esp_random() % (base_ms / 2 + 1);
        delay_ms = base_ms + jitter_ms;
        reconnect_attempt_++;
        ESP_LOGW(TAG, "reconnect_scheduled attempt=%d phase=fast delay_ms=%lu",
                 reconnect_attempt_, (unsigned long)delay_ms);
    } else {
        // Long-horizon recovery: keep retrying slowly so a recovered endpoint
        // reconnects without another wake word or button press.
        uint32_t jitter_ms = esp_random() % (kSlowReconnectRetryMs / 4 + 1);
        delay_ms = kSlowReconnectRetryMs + jitter_ms;
        ESP_LOGW(TAG, "reconnect_slow_retry_scheduled attempt=%d phase=slow delay_ms=%lu",
                 reconnect_attempt_ + 1, (unsigned long)delay_ms);
        reconnect_attempt_ = kFastReconnectAttempts;
    }
    reconnect_count_.fetch_add(1, std::memory_order_relaxed);  // OBS-2
    if (chat_cleanup_enabled_ && !lesson_runtime_active_.load() && online_intent_.load() &&
        RetainChatRecovery(ChatRecoveryIntent::Kind::Background, mode)) {
        chat_recovery_.ready = false;
        const auto now = static_cast<uint64_t>(esp_timer_get_time());
        const uint64_t delay = static_cast<uint64_t>(delay_ms) * 1000ULL;
        chat_recovery_.retry_at_us = now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000ULL);
}

void Application::SchedulePassiveLessonReconnect() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    if (IsDeviceClaimed() && !lesson_runtime_active_.load() &&
        WifiManager::GetInstance().IsConnected() &&
        backend_recovery_window_.ShouldEnterWifiConfig(
            static_cast<uint64_t>(esp_timer_get_time() / 1000))) {
        ESP_LOGW(TAG, "passive_backend_timeout_entering_wifi_config");
        reconnect_passive_.store(false);
        passive_ws_intent_.store(false);
        static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
        return;
    }
#endif
    if (reconnect_passive_.load()) {
        ESP_LOGD(TAG, "passive_lesson_reconnect_already_pending");
        return;
    }
    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->HandleReconnectTick(); });
        };
        args.arg = this;
        args.name = "reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            reconnect_passive_.store(false);
            return;
        }
    }
    uint32_t capped_attempt = passive_reconnect_attempt_ > 4 ? 4 : passive_reconnect_attempt_;
    uint32_t base_ms = 500u << capped_attempt;
    uint32_t jitter_ms = esp_random() % (base_ms / 2 + 1);
    uint32_t delay_ms = base_ms + jitter_ms;
    passive_reconnect_attempt_++;
    reconnect_passive_.store(true);
    ESP_LOGW(TAG, "passive_lesson_reconnect_scheduled attempt=%d delay_ms=%lu",
             passive_reconnect_attempt_, (unsigned long)delay_ms);
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000ULL);
}

void Application::HandleReconnectTick() {
    if (chat_cleanup_enabled_ && chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !reconnect_passive_.load() && !lesson_runtime_active_.load()) {
        if (chat_recovery_.kind == ChatRecoveryIntent::Kind::None ||
            static_cast<uint64_t>(esp_timer_get_time()) < chat_recovery_.retry_at_us) return;
        chat_recovery_.ready = true;
        PollChatRecovery(static_cast<uint64_t>(esp_timer_get_time()));
        return;
    }
    if (protocol_work_lifetime_.Pending()) {
        reconnect_passive_.store(false);
        return;
    }
    if (protocol_ == nullptr) {
        reconnect_attempt_ = 0;
        passive_reconnect_attempt_ = 0;
        reconnect_passive_.store(false);
        connect_attempt_active_.store(false);
        return;
    }
    if (reconnect_passive_.exchange(false)) {
        if (lesson_asset_sync_quiet_.load()) {
            ESP_LOGI(TAG, "lesson asset sync quiet deferred passive reconnect");
            SchedulePassiveLessonReconnect();
            return;
        }
        if (protocol_->IsAudioChannelOpened()) {
            passive_reconnect_attempt_ = 0;
            return;
        }
        if (connect_in_flight_.load() || protocol_work_lifetime_.Busy()) {
            SchedulePassiveLessonReconnect();
            return;
        }
        auto state = GetDeviceState();
        if (state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting) {
            passive_reconnect_attempt_ = 0;
            return;
        }
        if (state != kDeviceStateIdle) {
            const bool lesson_answer_turn =
                lesson_runtime_active_.load() &&
                (lesson_interactive_listen_pending_.load() ||
                 lesson_interactive_listening_active_.load());
            if (lesson_answer_turn &&
                (state == kDeviceStateSpeaking ||
                 state == kDeviceStateListening ||
                 state == kDeviceStateConnecting)) {
                ESP_LOGI(TAG, "passive_lesson_reconnect_tick answer_turn state=%d",
                         static_cast<int>(state));
                StartPassiveLessonWebsocket();
                return;
            }
            SchedulePassiveLessonReconnect();
            return;
        }
        ESP_LOGI(TAG, "passive_lesson_reconnect_tick attempt=%d", passive_reconnect_attempt_);
        StartPassiveLessonWebsocket();
        return;
    }
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet deferred voice reconnect");
        ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson reconnect ignored");
        reconnect_attempt_ = 0;
        connect_attempt_active_.store(false);
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        reconnect_attempt_ = 0;  // user moved on; abandon the retry chain
        connect_attempt_active_.store(false);
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        reconnect_attempt_ = 0;
        connect_attempt_active_.store(false);
        return;
    }
    if (connect_in_flight_.load() || protocol_work_lifetime_.Busy()) {
        ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());  // previous worker still finishing; retry later
        return;
    }
    ESP_LOGI(TAG, "reconnect_tick attempt=%d", reconnect_attempt_);
    SetDeviceState(kDeviceStateConnecting);
    ContinueOpenAudioChannel(reconnect_mode_);
}

void Application::HandleStartListeningEvent() {
    if (IsWifiConfigEntryPending()) return;
    if (chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() && !lesson_runtime_active_.load() &&
        (chat_protocol_owned_.load() || chat_source_connect_generation_.load() != connect_generation_.load())) {
        BeginChatListen(kListeningModeManualStop, ChatListenOrigin::User);
        return;
    }
    if (IsSelectedNormalChatRoute()) {
        if (GetDeviceState() == kDeviceStateSpeaking &&
            !(chat_rearm_phase_ == ChatRearmPhase::Pending && chat_rearm_mode_ == kListeningModeManualStop)) {
            ConnectionSource source;
            if (!protocol_ || lesson_asset_sync_quiet_.load() ||
                !chat_protocol_signals_->TrySource(source) || protocol_->CurrentConnectionEpoch() != source.connection_epoch) return;
            if (!HandleChatAbort(kAbortReasonNone, false) || chat_playout_recovery_) return;
        }
        BeginChatListen(kListeningModeManualStop, ChatListenOrigin::User);
        return;
    }
    auto state = GetDeviceState();
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet ignored start listening state=%d",
                 static_cast<int>(state));
        return;
    }
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() ||
        lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson start listening ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        if (!audio_service_.IsRunning()) {
            ESP_LOGI(TAG, "Audio test unavailable while provisioning workers are deferred");
            return;
        }
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        if (lesson_interactive_listen_pending_.load()) {
            ESP_LOGI(TAG, "lesson prompt still speaking; defer listening");
            listening_mode_ = kListeningModeManualStop;
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ClearChatMessages();
                display->SetStatus("Sắp đến lượt con...");
            }
            return;
        }
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateListening) {
        ESP_LOGI(TAG, "lesson/manual listening rearm");
        listening_mode_ = kListeningModeManualStop;
        if (lesson_interactive_listening_active_.load() && !lesson_interactive_listen_pending_.load()) {
            ESP_LOGI(TAG, "lesson listening already active; duplicate start ignored");
            return;
        }
        if (lesson_interactive_listen_pending_.exchange(false)) {
            lesson_interactive_listening_active_.store(true);
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ClearChatMessages();
                display->SetStatus("Con nói nhé...");
                display->SetChatMessage("system", "Con nói nhé.");
            }
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        }
        {
            int64_t now_ms = esp_timer_get_time() / 1000;
            listening_started_ms_.store(now_ms);
            last_listening_activity_ms_.store(now_ms);
        }
        protocol_->SendStartListening(kListeningModeManualStop);
        if (!RequestChatLessonCapture()) audio_service_.EnableVoiceProcessing(true);
    }
}

bool Application::HandleLessonPlayoutTts(const cJSON* root, ChatRequestContext context) {
    std::lock_guard<std::mutex> lock(lesson_playout_mutex_);
    const auto* id = cJSON_GetObjectItem(root, "playoutId");
    const auto* state = cJSON_GetObjectItem(root, "state");
    const auto* reason = cJSON_GetObjectItem(root, "reason");
    const bool interrupt = cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0 &&
        cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0;
    if (!id && lesson_playout_id_.empty()) return false;
    if (!id && !interrupt) return true;
    const std::string playout_id = id && cJSON_IsString(id) ? id->valuestring :
        !id ? lesson_playout_id_ : std::string{};
    if (!IsChatLessonRequestCurrent(context) || !lesson_runtime_active_.load() ||
        lesson_terminal_audio_generation_.load() != 0 ||
        !LessonAudioPlayout::ValidId(playout_id)) return true;
    if (!cJSON_IsString(state)) return true;
    if (strcmp(state->valuestring, "start") == 0) {
        lesson_audio_playout_.SetScope(protocol_generation_.load(), lesson_transport_epoch_gate_.PublishedEpoch());
        if (lesson_audio_playout_.Seen(playout_id)) return true;
        if (auto token = std::atomic_load(&lesson_playout_authorization_)) token->store(false);
        tts_audio_accepting_.store(false);
        lesson_audio_playout_.Cancel();
        lesson_playout_id_.clear();
        lesson_playout_pending_.store(false);
        lesson_idle_repaint_suppressed_.store(true);
        if (GetDeviceState() == kDeviceStateSpeaking && listening_mode_ != kListeningModeRealtime)
            SetDeviceState(kDeviceStateIdle);
        audio_service_.SetPlaybackGeneration(++speaking_generation_);
        audio_service_.ResetDecoder();
        uint64_t reset_epoch = 0;
        if (!audio_service_.TryGetPlaybackResetEpoch(reset_epoch) ||
            !lesson_audio_playout_.Begin(speaking_generation_.load(), playout_id, reset_epoch)) return true;
        lesson_playout_context_ = context;
        lesson_playout_protocol_generation_ = protocol_generation_.load();
        lesson_playout_epoch_ = lesson_transport_epoch_gate_.PublishedEpoch();
        lesson_playout_generation_ = speaking_generation_.load();
        lesson_playout_id_ = playout_id;
        lesson_playout_pending_.store(true);
        lesson_playout_drain_id_.clear();
        lesson_playout_stop_ms_ = 0;
        lesson_playout_drained_at_ms_ = 0;
        lesson_playout_start_sent_ = false;
        lesson_playout_stop_sent_ = false;
        std::atomic_store(&lesson_playout_authorization_, std::make_shared<std::atomic<bool>>(true));
        aborted_ = false;
        if (GetDeviceState() == kDeviceStateListening && listening_mode_ != kListeningModeRealtime) {
            audio_service_.EnableVoiceProcessing(false);
            listening_started_ms_.store(0);
            last_listening_activity_ms_.store(0);
        }
        last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
        // START admits bytes. The output callback below owns the speech cue.
        lesson_idle_repaint_suppressed_.store(true);
        if (GetDeviceState() == kDeviceStateSpeaking && listening_mode_ != kListeningModeRealtime)
            SetDeviceState(kDeviceStateIdle);
        tts_audio_accepting_.store(true);
    } else if (strcmp(state->valuestring, "stop") == 0 &&
               lesson_audio_playout_.Current(playout_id, speaking_generation_.load())) {
        if (cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0) {
            if (auto token = std::atomic_load(&lesson_playout_authorization_)) token->store(false);
            lesson_audio_playout_.Cancel();
            lesson_playout_id_.clear();
            lesson_playout_pending_.store(false);
            tts_audio_accepting_.store(false);
            audio_service_.SetPlaybackGeneration(++speaking_generation_);
            audio_service_.ResetDecoder();
            speaking_arm_dispatch_.Cancel();
            lesson_idle_repaint_suppressed_.store(true);
            if (GetDeviceState() == kDeviceStateSpeaking) SetDeviceState(kDeviceStateIdle);
        } else if (lesson_playout_stop_ms_ == 0 && lesson_audio_playout_.Stop(playout_id)) {
            const auto* drain = cJSON_GetObjectItem(root, "drainId");
            if (cJSON_IsString(drain) && strlen(drain->valuestring) <= 64)
                lesson_playout_drain_id_ = drain->valuestring;
            lesson_playout_stop_ms_ = esp_timer_get_time() / 1000;
            tts_audio_accepting_.store(false);
        }
    }
    return true;
}

bool Application::QueueLessonPlayoutAck(const char* state, uint64_t at_ms, bool drain) {
    if (!lesson_playout_context_ || chat_connection_messages_.Size() >= 2 ||
        !IsChatLessonRequestCurrent(lesson_playout_context_)) return false;
    const auto authorization = std::atomic_load(&lesson_playout_authorization_);
    if (!authorization || !authorization->load(std::memory_order_acquire)) return false;
    const auto text = drain
        ? Protocol::EncodeTtsDrainAck(lesson_playout_drain_id_, lesson_playout_context_->session_id)
        : Protocol::EncodeLessonPlayoutAck(lesson_playout_id_, state, at_ms, lesson_playout_context_->session_id);
    return !text.empty() && RequestChatConnectionText(text, lesson_playout_context_, 0, authorization) != 0;
}

void Application::PollLessonAudioPlayout() {
    std::unique_lock<std::mutex> lock(lesson_playout_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (lesson_playout_id_.empty()) return;
    const auto now_ms = esp_timer_get_time() / 1000;
    const auto owned = [this]() {
        const auto authorization = std::atomic_load(&lesson_playout_authorization_);
        return authorization && authorization->load(std::memory_order_acquire) &&
        protocol_ && lesson_runtime_active_.load() &&
        IsChatLessonRequestCurrent(lesson_playout_context_) &&
        lesson_playout_protocol_generation_ == protocol_generation_.load() &&
        lesson_playout_epoch_ == lesson_transport_epoch_gate_.PublishedEpoch() &&
        lesson_audio_playout_.Current(lesson_playout_id_, speaking_generation_.load()); };
    const bool timed_out = lesson_playout_stop_ms_ ?
        now_ms - lesson_playout_stop_ms_ >= kTtsStopPlaybackDrainTimeoutMs :
        now_ms - last_speaking_activity_ms_.load() >= kSpeakingTimeoutMs;
    auto retire = [this](bool flush) {
        if (flush) {
            const auto authorization = std::atomic_load(&lesson_playout_authorization_);
            if (authorization) authorization->store(false, std::memory_order_release);
        }
        lesson_audio_playout_.Cancel();
        lesson_playout_id_.clear();
        lesson_playout_pending_.store(false);
        lesson_playout_context_.reset();
        if (lesson_playout_generation_ == speaking_generation_.load()) {
            tts_audio_accepting_.store(false);
            if (flush) {
                audio_service_.SetPlaybackGeneration(++speaking_generation_);
                audio_service_.ResetDecoder();
            }
            last_speaking_activity_ms_.store(0);
            lesson_idle_repaint_suppressed_.store(true);
            if (GetDeviceState() == kDeviceStateSpeaking) SetDeviceState(kDeviceStateIdle);
        }
    };
    if (!owned()) { retire(true); return; }
    if (timed_out || lesson_audio_playout_.Failed()) {
        const auto context = lesson_playout_context_;
        retire(true);
        if (context) FailChatRequest(context);
        return;
    }
    if (!lesson_playout_start_sent_ && chat_connection_messages_.Size() >= 2) return;
    if (const auto start = lesson_audio_playout_.TakeStart()) {
        if (start->generation != lesson_playout_generation_ ||
            !QueueLessonPlayoutAck("start", start->at_ms)) {
            retire(true);
            return;
        }
        if (!owned()) { retire(true); return; }
        lesson_playout_start_sent_ = true;
        SetDeviceState(kDeviceStateSpeaking);
        ArmSpeakingTimeout();
    }
    if (!lesson_playout_stop_ms_ || !lesson_playout_start_sent_) return;
    PlaybackDrainSnapshot snapshot;
    if (!audio_service_.TryGetPlaybackDrainSnapshot(snapshot) ||
        !lesson_audio_playout_.Drained(snapshot)) return;
    // Revalidate after the audio lock boundary and before sending either receipt.
    if (!protocol_ || !lesson_runtime_active_.load() ||
        lesson_playout_protocol_generation_ != protocol_generation_.load() ||
        !IsChatLessonRequestCurrent(lesson_playout_context_) ||
        lesson_playout_epoch_ != lesson_transport_epoch_gate_.PublishedEpoch() ||
        !lesson_audio_playout_.Current(lesson_playout_id_, speaking_generation_.load())) {
        retire(true);
        return;
    }
    if (!lesson_playout_drained_at_ms_) lesson_playout_drained_at_ms_ = static_cast<uint64_t>(now_ms);
    lesson_idle_repaint_suppressed_.store(true);
    if (GetDeviceState() == kDeviceStateSpeaking) {
        const bool lesson_interactive_turn = lesson_interactive_listen_pending_.load() ||
            lesson_interactive_listening_active_.load();
        SetDeviceState(lesson_interactive_turn && listening_mode_ != kListeningModeAutoStop
            ? kDeviceStateListening : kDeviceStateIdle);
    }
    if (!lesson_playout_stop_sent_) {
        if (chat_connection_messages_.Size() >= 2) return;
        if (!QueueLessonPlayoutAck("stop", lesson_playout_drained_at_ms_)) {
            retire(true);
            return;
        }
        lesson_playout_stop_sent_ = true;
    }
    if (!owned()) { retire(true); return; }
    if (!lesson_playout_drain_id_.empty()) {
        if (chat_connection_messages_.Size() >= 2) return;
        if (!QueueLessonPlayoutAck("stop", lesson_playout_drained_at_ms_, true)) {
            retire(true);
            return;
        }
    }
    retire(false);
}

void Application::DispatchIncomingJson(const cJSON* root, uint64_t callback_transport_epoch,
    bool is_websocket_protocol, ChatRequestContext context) {
    ChatRuntimeTiming timing(1, []() { return static_cast<uint64_t>(esp_timer_get_time()); },
        [](uint32_t site, uint32_t hi, uint32_t lo) {
            ESP_LOGW(TAG, "chat_slow_scope site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                static_cast<unsigned>(site), static_cast<unsigned long>(hi), static_cast<unsigned long>(lo));
        });
    if (!IsChatRequestCurrent(context)) return;
    auto* display = Board::GetInstance().GetDisplay();
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        // US-006 Slice-01 (DIV-FW-NULLDEREF): guard the type deref on the path the
        // additive lesson_ branch joins. A missing/non-string type would null-deref
        // type->valuestring below. Both transports already pre-guard this
        // (websocket_protocol.cc, mqtt_protocol.cc), so no valid frame changes
        // behavior — defense-in-depth on the shared dispatch path only.
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Missing or non-string message type, dropping frame");
            return;
        }
        if (lesson_asset_sync_quiet_.load() &&
            (strcmp(type->valuestring, "tts") == 0 ||
             strcmp(type->valuestring, "stt") == 0)) {
            ESP_LOGI(TAG, "lesson asset sync quiet dropped voice frame type=%s",
                     type->valuestring);
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            if (!IsChatLessonRequestCurrent(context)) return;
            if (HandleLessonPlayoutTts(root, context)) return;
            auto state = cJSON_GetObjectItem(root, "state");
            // Guard the state deref: a tts frame with no "state" or a non-string
            // state null-derefs state->valuestring below (deep-audit #4 HIGH — a
            // malformed/MITM frame crashes the audio task). cJSON_IsString covers
            // both the missing-key (null node) and wrong-type cases.
            if (!cJSON_IsString(state)) {
                ESP_LOGW(TAG, "tts frame missing or non-string state; dropping");
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                audio_service_.ResetDecoder();
                if (GetDeviceState() == kDeviceStateListening && listening_mode_ != kListeningModeRealtime) {
                    audio_service_.EnableVoiceProcessing(false);
                    listening_started_ms_.store(0);
                    last_listening_activity_ms_.store(0);
                }
                // Bump the response generation and publish it to the audio
                // service BEFORE opening the intake gate, so every packet of
                // this response is stamped with — and gated against — the same
                // generation. Done synchronously here (same task as
                // OnIncomingAudio) to avoid dropping the first frames.
                audio_service_.SetPlaybackGeneration(++speaking_generation_);
                speaking_arm_dispatch_.BeginResponse(speaking_generation_.load());
                tts_audio_accepting_.store(true);
                const auto started_generation = speaking_generation_.load();
                Schedule([this, context, started_generation]() {
                    if (!IsChatLessonRequestCurrent(context) ||
                        started_generation != speaking_generation_.load()) return;
                    aborted_ = false;
                    auto current_generation = speaking_generation_.load();
                    last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
                    SetDeviceState(kDeviceStateSpeaking);
                    ESP_LOGI(TAG, "tts_start_received generation=%lu", (unsigned long)current_generation);
                    ArmSpeakingTimeout();
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                speaking_arm_dispatch_.Cancel();
                tts_audio_accepting_.store(false);
                int64_t t_recv = esp_timer_get_time() / 1000;
                const auto t_recv_sec = static_cast<unsigned long>(t_recv / 1000);
                const auto t_recv_ms = static_cast<unsigned long>(t_recv % 1000);
                ESP_LOGI(TAG, "tts_stop_received ts=%lu%03lu", t_recv_sec, t_recv_ms);
                // Patch 3.4: the backend tags an interrupt-driven stop with
                // reason="interrupt" (barge-in) vs a normal end-of-turn stop.
                auto reason = cJSON_GetObjectItem(root, "reason");
                bool is_interrupt = cJSON_IsString(reason) &&
                                    strcmp(reason->valuestring, "interrupt") == 0;
                auto drain_id = cJSON_GetObjectItem(root, "drainId");
                std::string tts_drain_id;
                if (cJSON_IsString(drain_id) &&
                    strlen(drain_id->valuestring) <= 64) {
                    tts_drain_id = drain_id->valuestring;
                }
                const std::uint64_t stopped_audio_generation =
                    static_cast<std::uint64_t>(speaking_generation_.load()) + 1;
                if (is_interrupt) {
                    // Barge-in: cut NOW instead of draining. Bump+publish the
                    // generation so any in-flight frame is gen-gated (Patch 3.3),
                    // then clear the playback/decode queues. Idempotent if the
                    // local VAD path already aborted.
                    audio_service_.SetPlaybackGeneration(++speaking_generation_);
                    audio_service_.ResetDecoder();
                    ESP_LOGI(TAG, "tts_stop_interrupt_flush ts=%lu%03lu",
                             t_recv_sec, t_recv_ms);
                }
                auto continue_listening = cJSON_GetObjectItem(root, "continue_listening");
                bool force_continue_listening = cJSON_IsTrue(continue_listening);
                auto listen_mode = cJSON_GetObjectItem(root, "listen_mode");
                bool force_realtime_listen = cJSON_IsString(listen_mode) &&
                                             strcmp(listen_mode->valuestring, "realtime") == 0;
                bool explicit_stop_listening =
                    cJSON_IsBool(continue_listening) && !cJSON_IsTrue(continue_listening) &&
                    cJSON_IsString(listen_mode) &&
                    strcmp(listen_mode->valuestring, "manual") == 0;
                // NOTE: for a NORMAL end-of-turn stop we deliberately do NOT
                // ResetDecoder — that cut the final 200-500ms of every response
                // because the server sends `tts state=stop` immediately after
                // audio_end while the playback queue still holds buffered frames.
                // User reported: "phản hồi không ổn định chưa trả lời hết câu
                // chuyển sang đang lắng nghe". Normal stops rely on natural queue
                // drain; only the interrupt branch above cuts early.
                const auto stop_callback_generation = speaking_generation_.load();
                Schedule([this, force_continue_listening, force_realtime_listen,
                          explicit_stop_listening, stopped_audio_generation,
                          is_interrupt, tts_drain_id, context, stop_callback_generation]() {
                    if (!IsChatLessonRequestCurrent(context) ||
                        stop_callback_generation != speaking_generation_.load()) return;
                    ++speaking_generation_;
                    last_speaking_activity_ms_.store(0);
                    if (!is_interrupt && !tts_drain_id.empty()) {
                        const bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(
                            kTtsStopPlaybackDrainTimeoutMs);
                        if (!IsChatLessonRequestCurrent(context)) return;
                        if (playback_drained) {
                            if (protocol_) protocol_->SendTtsDrainAck(tts_drain_id);
                        } else {
                            ESP_LOGW(TAG,
                                     "tts_stop_playback_drain_timeout timeout_ms=%lu action=drain_ack",
                                     static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                        }
                    }
                    const bool lesson_interactive_turn =
                        lesson_interactive_listen_pending_.load() ||
                        lesson_interactive_listening_active_.load();
                    const std::uint64_t terminal_audio_generation =
                        lesson_terminal_audio_generation_.exchange(0);
                    if (terminal_audio_generation == stopped_audio_generation) {
                        ESP_LOGI(TAG,
                                 "terminal lesson tts stop matched generation_hi=%lu generation_lo=%lu state=%d",
                                 static_cast<unsigned long>(stopped_audio_generation >> 32),
                                 static_cast<unsigned long>(stopped_audio_generation),
                                 static_cast<int>(GetDeviceState()));
                        lesson_idle_repaint_suppressed_.store(true);
                        SetDeviceState(kDeviceStateIdle);
                        return;
                    }
                    if (terminal_audio_generation != 0) {
                        ESP_LOGI(TAG,
                                 "stale terminal lesson tts stop ignored terminal_hi=%lu terminal_lo=%lu stopped_hi=%lu stopped_lo=%lu",
                                 static_cast<unsigned long>(terminal_audio_generation >> 32),
                                 static_cast<unsigned long>(terminal_audio_generation),
                                 static_cast<unsigned long>(stopped_audio_generation >> 32),
                                 static_cast<unsigned long>(stopped_audio_generation));
                    }
                    if (lesson_runtime_active_.load() && !lesson_interactive_turn) {
                        ESP_LOGI(TAG, "lesson tts stop continue ignored state=%d",
                                 static_cast<int>(GetDeviceState()));
                        lesson_idle_repaint_suppressed_.store(true);
                        SetDeviceState(kDeviceStateIdle);
                        return;
                    }
                    if (explicit_stop_listening && GetDeviceState() == kDeviceStateListening) {
                        audio_service_.EnableVoiceProcessing(false);
                        listening_started_ms_.store(0);
                        last_listening_activity_ms_.store(0);
                        while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
                        SetDeviceState(kDeviceStateIdle);
                        ESP_LOGI(TAG, "manual_tts_stop -> idle from listening");
                        return;
                    }
                    const bool voice_turn_owned =
                        microphone_uplink_authorized_.load() &&
                        !passive_ws_intent_.load() &&
                        online_intent_.load() &&
                        (GetDeviceState() == kDeviceStateSpeaking ||
                         GetDeviceState() == kDeviceStateListening);
                    if (force_continue_listening && !lesson_interactive_turn) {
                        if (!voice_turn_owned) {
                            ESP_LOGW(TAG,
                                     "tts_stop_continue_listening_rejected state=%d passive=%d online=%d",
                                     static_cast<int>(GetDeviceState()),
                                     passive_ws_intent_.load() ? 1 : 0,
                                     online_intent_.load() ? 1 : 0);
                            return;
                        }
                        bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                        if (!IsChatLessonRequestCurrent(context)) return;
                        if (!playback_drained) {
                            ESP_LOGW(TAG,
                                     "tts_stop_playback_drain_timeout timeout_ms=%lu action=continue_listening",
                                     static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                        }
                        if (force_realtime_listen) {
                            listening_mode_ = kListeningModeRealtime;
                        } else {
                            listening_mode_ = GetDefaultListeningMode();
                        }
                        SetDeviceState(kDeviceStateListening);
                        if (protocol_) {
                            protocol_->SendStartListening(kListeningModeRealtime);
                        }
                        audio_service_.EnableVoiceProcessing(true);
                        const uint64_t resumed_ms = esp_timer_get_time() / 1000;
                        ESP_LOGI(TAG,
                                 "mic_loop_resumed ts=%lu%03lu reason=tts_stop_continue_listening",
                                 static_cast<unsigned long>(resumed_ms / 1000),
                                 static_cast<unsigned long>(resumed_ms % 1000));
                        return;
                    }
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            if (lesson_interactive_turn) {
                                bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                                if (!IsChatLessonRequestCurrent(context)) return;
                                if (!playback_drained) {
                                    ESP_LOGW(TAG,
                                             "tts_stop_playback_drain_timeout timeout_ms=%lu action=lesson_listening",
                                             static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                                }
                                SetDeviceState(kDeviceStateListening);
                                ESP_LOGI(TAG, "lesson prompt complete -> listening");
                            } else {
                                SetDeviceState(kDeviceStateIdle);
                            }
                        } else if (listening_mode_ == kListeningModeAutoStop) {
                            bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                            if (!IsChatLessonRequestCurrent(context)) return;
                            if (!playback_drained) {
                                ESP_LOGW(TAG,
                                         "tts_stop_playback_drain_timeout timeout_ms=%lu action=idle",
                                         static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                            }
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                            const uint64_t resumed_ms = esp_timer_get_time() / 1000;
                            ESP_LOGI(TAG, "mic_loop_resumed ts=%lu%03lu",
                                     static_cast<unsigned long>(resumed_ms / 1000),
                                     static_cast<unsigned long>(resumed_ms % 1000));
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGD(TAG, "<< %s", text->valuestring);  // PRIV-1: transcript content debug-only (COPPA)
                    if (!lesson_runtime_active_.load()) {
                        if (context) display->SetChatMessage("assistant", text->valuestring);
                        else Schedule([display, message = std::string(text->valuestring)]() {
                            display->SetChatMessage("assistant", message.c_str());
                        });
                    }
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGD(TAG, ">> %s", text->valuestring);  // PRIV-1: transcript content debug-only (COPPA)
                if (!lesson_runtime_active_.load()) {
                    if (context) display->SetChatMessage("user", text->valuestring);
                    else Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("user", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
#if CONFIG_TBOT_VOICE_DEMO
            // Keep the current animated face; transcript chunks can otherwise
            // reconstruct a GIF faster than the screen can render it.
            return;
#endif
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                if (!lesson_runtime_active_.load()) {
                    if (context) {
                        display->SetEmotion(emotion->valuestring);
                        HandleEmotionGesture(emotion->valuestring);
                    } else Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                        display->SetEmotion(emotion_str.c_str());
                        HandleEmotionGesture(emotion_str.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload, context);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    if (lesson_runtime_active_.load()) {
                        ESP_LOGI(TAG, "System reboot ignored during lesson");
                        return;
                    }
                    // Do a reboot if user requests a OTA update
                    if (context) Reboot(context);
                    else Schedule([this]() {
                        Reboot();
                    });
                } else if (strcmp(command->valuestring, "unpair") == 0) {
                    if (context) {
                        BeginChatUnpair(root, context);
                        return;
                    }
                    if (lesson_runtime_active_.load()) {
                        ESP_LOGI(TAG, "System unpair ignored during lesson");
                        return;
                    }
                    const auto* request_id = cJSON_GetObjectItem(root, "request_id");
                    if (cJSON_IsString(request_id) && request_id->valuestring != nullptr &&
                        request_id->valuestring[0] != '\0' && std::strlen(request_id->valuestring) <= 64) {
                        cJSON* ack = cJSON_CreateObject();
                        if (ack != nullptr) {
                            cJSON_AddStringToObject(ack, "type", "system_ack");
                            cJSON_AddStringToObject(ack, "command", "unpair");
                            cJSON_AddStringToObject(ack, "request_id", request_id->valuestring);
                            char* encoded = cJSON_PrintUnformatted(ack);
                            const bool sent = encoded != nullptr && protocol_ != nullptr &&
                                              protocol_->SendLessonFrame(encoded);
                            if (!sent) {
                                ESP_LOGW(TAG, "System unpair acknowledgement could not be sent");
                            }
                            if (encoded != nullptr) cJSON_free(encoded);
                            cJSON_Delete(ack);
                        }
                    }
                    EnterRepairPairingMode();
                } else if (strcmp(command->valuestring, "wifi_setup") == 0) {
                    if (lesson_runtime_active_.load()) {
                        ESP_LOGI(TAG, "System WiFi setup ignored during lesson");
                        return;
                    }
                    static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                if (!lesson_runtime_active_.load()) {
                    Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
                }
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else if (strcmp(type->valuestring, "robot_action") == 0) {
            if (!HandleRobotActionMessage(root, context)) {
                ESP_LOGW(TAG, "Unsupported robot action");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            char* root_str = cJSON_PrintUnformatted(root);
            ESP_LOGI(TAG, "Received custom message: %s", root_str ? root_str : "(null)");
            if (root_str != nullptr) {
                cJSON_free(root_str);
            }
            if (cJSON_IsObject(payload)) {
                if (HandleRobotActionMessage(payload, context)) {
                    return;
                }
                char* payload_str_raw = cJSON_PrintUnformatted(payload);
                std::string payload_str = (payload_str_raw != nullptr) ? std::string(payload_str_raw) : std::string();
                if (payload_str_raw != nullptr) {
                    cJSON_free(payload_str_raw);
                }
                if (!lesson_runtime_active_.load()) {
                    if (context) display->SetChatMessage("system", payload_str.c_str());
                    else Schedule([this, display, payload_str = std::move(payload_str)]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
        } else if (strncmp(type->valuestring, "lesson_", 7) == 0) {
            // US-006 Slice-01 (S10): additive lesson_* dispatch. Placed immediately
            // ABOVE the unknown-type no-op so un-upgraded firmware keeps dropping
            // lesson_* silently (backward-compat). Queue it so HTTP/TLS image fetch
            // and decode never run on the WebSocket receive callback / lwIP stack.
            if (!is_websocket_protocol) {
                ESP_LOGW(TAG, "lesson_* ignored on non-WebSocket transport");
                return;
            }
            EnqueueLessonMessage(root, callback_transport_epoch, context);
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
}

bool Application::IsSelectedNormalChatRoute() const {
    return chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !lesson_runtime_active_.load() && !chat_protocol_owned_.load() &&
        chat_source_connect_generation_.load() == connect_generation_.load();
}

void Application::CancelChatRecovery(uint32_t outcome) {
    if (chat_recovery_.kind == ChatRecoveryIntent::Kind::None) return;
    ESP_LOGI(TAG, "chat_recovery outcome=%lu", static_cast<unsigned long>(outcome));
    if (outcome == 2 && chat_recovery_.protocol_generation == protocol_generation_.load() &&
        chat_recovery_.connect_generation == connect_generation_.load()) {
        if (chat_recovery_.lesson_generation == lesson_runtime_generation_.load()) online_intent_.store(false);
        if (chat_recovery_.opening && connect_generation_.load() != UINT32_MAX) ++connect_generation_;
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);
        CancelConnectWatchdog();
    }
    chat_recovery_ = {};
}

bool Application::RetainChatRecovery(ChatRecoveryIntent::Kind kind, ListeningMode mode) {
    if (IsWifiConfigEntryPending()) return false;
    using Kind = ChatRecoveryIntent::Kind;
    const auto state = GetDeviceState();
    if (!chat_cleanup_enabled_ || !chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() ||
        lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load() || chat_unpair_context_ ||
        reset_pending_.load() || protocol_reinit_pending_.load() || reboot_pending_.load() ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting ||
        chat_protocol_infrastructure_fault_ || (!protocol_ && !chat_protocol_owned_.load()) ||
        (protocol_work_lifetime_.Pending() && !online_intent_.load() &&
         chat_recovery_.kind == Kind::None)) {
        ESP_LOGI(TAG, "chat_recovery outcome=5");
        return false;
    }
    auto& intent = chat_recovery_;
    if (intent.kind != Kind::None && (intent.protocol_generation != protocol_generation_.load() ||
        intent.connect_generation != connect_generation_.load() ||
        intent.lesson_generation != lesson_runtime_generation_.load())) {
        CancelChatRecovery();
        if (kind == Kind::Background) return false;
    }
    if (intent.kind == Kind::None) {
        intent.kind = Kind::Background;
        intent.protocol_generation = protocol_generation_.load();
        intent.connect_generation = connect_generation_.load();
        intent.lesson_generation = lesson_runtime_generation_.load();
        intent.mode = mode;
    }
    if (kind != Kind::Background && intent.kind == Kind::Background) {
        const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
        if (now > UINT64_MAX - 10000000ULL) {
            ESP_LOGI(TAG, "chat_recovery outcome=3");
            RequestChatAudioCleanup(speaking_generation_.load(), false, false, true);
            return false;
        }
        intent.kind = kind;
        intent.received_us = now;
        intent.deadline_us = now + 10000000ULL;
        intent.mode = mode;
        intent.ready = !intent.opening;
    }
    online_intent_.store(true);
    passive_ws_intent_.store(false);
    microphone_uplink_authorized_.store(false);
    ESP_LOGI(TAG, "chat_recovery outcome=1");
    return true;
}

void Application::PollChatRecovery(uint64_t now_us) {
    using Kind = ChatRecoveryIntent::Kind;
    auto& intent = chat_recovery_;
    if (intent.kind == Kind::None) return;
    const auto state = GetDeviceState();
    if (intent.protocol_generation != protocol_generation_.load() ||
        intent.lesson_generation != lesson_runtime_generation_.load() ||
        intent.connect_generation != connect_generation_.load() || !online_intent_.load() ||
        lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load() || chat_unpair_context_ ||
        reset_pending_.load() || protocol_reinit_pending_.load() || reboot_pending_.load() ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting ||
        chat_protocol_infrastructure_fault_) { CancelChatRecovery(); return; }
    if (intent.kind != Kind::Background && (now_us < intent.received_us || now_us >= intent.deadline_us)) {
        ESP_LOGI(TAG, "chat_recovery outcome=3");
        intent.kind = Kind::Background;
        intent.received_us = intent.deadline_us = 0;
        SetDeviceState(kDeviceStateIdle);
        RearmClaimedIdleWakeWord();
    }
    if (intent.opening || protocol_work_lifetime_.Pending() || chat_protocol_owned_.load() ||
        chat_protocol_state_.load() || connect_in_flight_.load() || !protocol_) return;
    ConnectionSource source;
    if (intent.adopted && chat_protocol_signals_->TrySource(source) &&
        chat_source_connect_generation_.load() == intent.connect_generation &&
        protocol_->CurrentConnectionEpoch() == source.connection_epoch) {
        if (intent.kind == Kind::Background) {
            chat_recovery_ = {};
            SetDeviceState(kDeviceStateIdle);
            RearmClaimedIdleWakeWord();
            return;
        }
        // Old controls keep their original source. Wait for bounded retirement,
        // then construct new controls with the original explicit deadline.
        if (chat_control_intents_.Size() || (chat_outbound_reservation_ && !chat_outbound_generation_)) return;
        const auto accepted = intent;
        chat_recovery_ = {};
        ESP_LOGI(TAG, "chat_recovery outcome=6");
        SetDeviceState(kDeviceStateIdle);
        try {
            if (accepted.kind == Kind::Wake)
                HandleChatWake(std::string(accepted.wake_text.data(), accepted.wake_size), accepted.read_wake);
            else BeginChatListen(accepted.mode, ChatListenOrigin::User);
        } catch (...) {
            RequestChatAudioCleanup(speaking_generation_.load(), false, false, true);
            ESP_LOGI(TAG, "chat_recovery outcome=5");
            return;
        }
        if (auto* wake = chat_control_intents_.Front()) wake->job.deadline_us = accepted.deadline_us;
        if (chat_rearm_phase_ == ChatRearmPhase::Pending) {
            chat_listen_received_us_ = accepted.received_us;
            chat_rearm_job_.deadline_us = accepted.deadline_us;
        }
        return;
    }
    if (!intent.ready || protocol_work_lifetime_.Busy()) return;
    if (protocol_->IsAudioChannelOpened()) {
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
        PollChatProtocolCleanup();
        return;
    }
    if (connect_generation_.load() == UINT32_MAX) { CancelChatRecovery(); return; }
    intent.ready = false;
    intent.opening = true;
    intent.attempted = true;
    intent.adopted = false;
    intent.connect_generation = ++connect_generation_;
    connect_in_flight_.store(true);
    connect_attempt_active_.store(true);
    reconnect_resume_listening_.store(false);
    SetDeviceState(kDeviceStateConnecting);
    ArmConnectWatchdog();
    auto* context = new (std::nothrow) ConnectContext{this, intent.mode, intent.connect_generation, std::string(), false, false};
    if (!context || !StartOpenChannelWorker(context)) {
        delete context;
        CompleteChatRecoveryOpen(intent.connect_generation, false);
    }
}

bool Application::CompleteChatRecoveryOpen(uint32_t generation, bool success) {
    if (chat_recovery_.kind == ChatRecoveryIntent::Kind::None || !chat_recovery_.opening ||
        chat_recovery_.connect_generation != generation || generation != connect_generation_.load() ||
        chat_recovery_.protocol_generation != protocol_generation_.load()) return false;
    if (chat_recovery_.lesson_generation != lesson_runtime_generation_.load()) {
        CancelChatRecovery();
        return true;
    }
    chat_recovery_.opening = false;
    connect_in_flight_.store(false);
    connect_attempt_active_.store(false);
    CancelConnectWatchdog();
    if (success) {
        chat_recovery_.ready = false;
        reconnect_attempt_ = 0;
        PollChatRecovery(static_cast<uint64_t>(esp_timer_get_time()));
    } else {
        backend_offline_.store(true);
        SetDeviceState(kDeviceStateIdle);
        ScheduleReconnect(chat_recovery_.mode, false);
    }
    return true;
}

bool Application::RequestChatLessonCapture() {
    if (!chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() || !IsLessonVoiceRoute()) return false;
    microphone_uplink_authorized_.store(false);
    ConnectionSource source;
    if (!chat_protocol_signals_->TrySource(source)) return true;
    chat_lesson_capture_owner_ = {source, protocol_generation_.load(), connect_generation_.load()};
    chat_lesson_capture_epoch_ = lesson_transport_epoch_gate_.PublishedEpoch();
    chat_lesson_capture_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
    chat_audio_fault_ = false;
    chat_lesson_capture_token_ = RequestChatAudioCleanup(speaking_generation_.load(), false, true, false,
        false, false, ChatWakePolicy::Listening);
    return true;
}

void Application::PollChatLessonCapture(uint64_t now_us) {
    const auto token = chat_lesson_capture_token_;
    if (!token) return;
    if (!IsLessonVoiceRoute() || GetDeviceState() != kDeviceStateListening || lesson_asset_sync_quiet_.load() ||
        chat_lesson_capture_epoch_ != lesson_transport_epoch_gate_.PublishedEpoch() ||
        !IsChatConnectionCurrent(chat_lesson_capture_owner_.source, chat_lesson_capture_owner_.protocol_generation,
            chat_lesson_capture_owner_.connect_generation) || chat_audio_desired_.revoked != token) {
        chat_lesson_capture_token_ = 0;
        return;
    }
    if (now_us >= chat_lesson_capture_deadline_us_ || chat_audio_fault_) {
        chat_lesson_capture_token_ = 0;
        ESP_LOGW(TAG, "chat_source_fault reason=lesson_capture");
        chat_protocol_signals_->PublishConnectionFault(chat_lesson_capture_owner_.source,
            chat_lesson_capture_owner_.connect_generation, ChatProtocolSignals::Error);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        return;
    }
    if (chat_audio_prepared_ != token || !audio_service_.ArmChatUplink(token, false)) return;
    chat_lesson_capture_token_ = 0;
    microphone_uplink_authorized_.store(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
}

void Application::PollChatInboundMessages() {
    for (size_t count = 0; count < 4; ++count) {
        // A START can arrive during the previous display update. Admit it
        // before another update consumes the receiver's 250 ms deadline.
        PollChatStart(static_cast<uint64_t>(esp_timer_get_time()));
        auto next = chat_inbound_messages_.TryTake();
        if (next.status == ChatInboundMessages::ReadStatus::Busy) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            return;
        }
        if (next.status == ChatInboundMessages::ReadStatus::Empty) return;
        auto context = std::move(next.context);
        if (!IsChatRequestCurrent(context)) continue;
        if (static_cast<uint64_t>(esp_timer_get_time()) >= context->deadline_us) {
            FailChatRequest(context);
            continue;
        }
        try { DispatchIncomingJson(context->root.get(), context->lesson_epoch, true, context); }
        catch (...) { FailChatRequest(context); }
        PollChatStart(static_cast<uint64_t>(esp_timer_get_time()));
    }
    if (chat_inbound_messages_.Pending()) xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::FailChatRequest(const ChatRequestContext& context) {
    if (!context || !IsChatRequestCurrent(context)) return;
    const auto signals = std::atomic_load(&chat_protocol_signals_);
    if (!signals || !signals->MatchesSource(context->owner.source)) return;
    ESP_LOGW(TAG, "chat_source_fault reason=request_failed");
    signals->PublishConnectionFault(context->owner.source,
        context->owner.connect_generation, ChatProtocolSignals::Error);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

bool Application::IsChatConnectionCurrent(ConnectionSource source, uint64_t protocol_generation, uint32_t connect_generation) const {
    const auto signals = std::atomic_load(&chat_protocol_signals_);
    return signals && signals->MatchesSource(source) &&
        !chat_protocol_owned_.load() && protocol_generation == protocol_generation_.load() &&
        connect_generation == connect_generation_.load();
}

bool Application::TakeChatCaption(ChatCaptionMailbox::Message& caption) {
#if CONFIG_TBOT_VOICE_DEMO
    const auto signals = std::atomic_load(&chat_protocol_signals_);
    if (!signals || !signals->captions.TryTake(caption)) return false;
    const auto state = GetDeviceState();
    return !IsLessonVoiceRoute() && !lesson_asset_sync_quiet_.load() &&
        (state == kDeviceStateSpeaking || state == kDeviceStateListening) &&
        caption.owner.response_generation == speaking_generation_.load() &&
        IsChatConnectionCurrent(caption.owner.source, caption.owner.protocol_generation,
            caption.owner.connect_generation);
#else
    (void)caption;
    return false;
#endif
}

bool Application::IsChatRequestCurrent(const ChatRequestContext& context) const {
    return !context || IsChatConnectionCurrent(context->owner.source, context->owner.protocol_generation,
        context->owner.connect_generation);
}

uint64_t Application::RequestChatConnectionText(const std::string& text, ChatRequestContext context, uint64_t received_us,
    std::shared_ptr<std::atomic<bool>> authorization) {
    ChatConnectionMessages::Owner owner;
    if (context) owner = context->owner;
    else {
        if (!chat_protocol_signals_ || !chat_protocol_signals_->TrySource(owner.source)) return 0;
        owner.protocol_generation = protocol_generation_.load();
        owner.connect_generation = connect_generation_.load();
    }
    if (!IsChatConnectionCurrent(owner.source, owner.protocol_generation, owner.connect_generation)) return 0;
    const auto id = chat_connection_messages_.Admit(owner, text,
        received_us ? received_us : static_cast<uint64_t>(esp_timer_get_time()), std::move(authorization));
    if (!id) {
        ESP_LOGW(TAG, "chat_source_fault reason=reply_admission");
        chat_protocol_signals_->PublishConnectionFault(owner.source, owner.connect_generation, ChatProtocolSignals::Error);
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    return id;
}

void Application::PollChatConnectionMessages(uint64_t now_us) {
    auto* record = chat_connection_messages_.Front();
    if (!record) {
        if (passive_ws_intent_.load() && !chat_control_intents_.Size() &&
            chat_rearm_phase_ != ChatRearmPhase::Pending && !chat_playout_stamp_) RetireChatOutbound();
        return;
    }
    using Outcome = ChatConnectionMessages::Outcome;
    using Result = ChatOutboundMailbox::Result;
    if (!IsChatConnectionCurrent(record->owner.source, record->owner.protocol_generation, record->owner.connect_generation) ||
        (record->outcome == Outcome::Pending && record->authorization &&
         !record->authorization->load(std::memory_order_acquire))) {
        record->outcome = Outcome::Cancelled;
        if (record->submitted) RetireChatOutbound();
    }
    if (record->outcome == Outcome::Pending && now_us >= record->deadline_us) {
        record->outcome = Outcome::Failed;
        if (record->authorization) record->authorization->store(false, std::memory_order_release);
        if (record->submitted) RetireChatOutbound();
    }
    if (record->outcome != Outcome::Pending) {
        if (record->outcome == Outcome::Failed && record->authorization && !record->submitted) {
            ESP_LOGW(TAG, "chat_source_fault reason=playout_receipt_delivery");
            chat_protocol_signals_->PublishConnectionFault(record->owner.source,
                record->owner.connect_generation, ChatProtocolSignals::Error);
        }
        if (record->id == chat_unpair_id_) chat_unpair_completed_ = !record->submitted;
        if (!record->submitted && record->id == chat_passive_ping_id_) {
            chat_passive_ping_id_ = 0;
            if (record->outcome == Outcome::Failed) {
                ESP_LOGW(TAG, "chat_source_fault reason=ping_delivery");
                chat_protocol_signals_->PublishConnectionFault(record->owner.source,
                    record->owner.connect_generation, ChatProtocolSignals::Error);
            }
        }
        if (!record->submitted) chat_connection_messages_.Pop();
        return;
    }
    if (record->submitted || chat_control_intents_.Size() || chat_start_obsolete_reservation_) return;
    if (!chat_outbound_generation_) {
        if (chat_outbound_reservation_) return;
        if (ActivateChatOutbound(record->owner.source.connection_epoch) != Result::Sent) return;
    }
    auto& job = record->physical;
    job.kind = ChatOutboundMailbox::Kind::FullText;
    job.source = record->owner.source;
    job.connect_generation = record->owner.connect_generation;
    job.full_text = record->payload;
    job.authorization = record->authorization;
    job.deadline_us = record->deadline_us;
    const auto result = SubmitChatOutbound(job);
    if (result == Result::Sent) {
        record->submitted = true;
        record->reservation = chat_outbound_reservation_;
    } else if (result == Result::Busy) {
        // No admission occurred; a newer control may overtake this attempt.
        record->physical = {};
    } else {
        record->outcome = Outcome::Failed;
        if (record->authorization) record->authorization->store(false, std::memory_order_release);
    }
}

bool Application::MaintainChatPassiveLiveness() {
    ConnectionSource source;
    if (!protocol_ || !chat_protocol_signals_ || !chat_protocol_signals_->TrySource(source)) return false;
    if (chat_passive_ping_id_) return true;
    const int state = protocol_->ObserveChatPassiveLiveness(source);
    if (state < 0) {
        ESP_LOGW(TAG, "chat_source_fault reason=passive_liveness");
        chat_protocol_signals_->PublishConnectionFault(source, chat_source_connect_generation_.load(), ChatProtocolSignals::Error);
        return false;
    }
    if (state == 0 || chat_connection_messages_.Size() >= 2) return true;
    chat_passive_ping_id_ = RequestChatConnectionText("{\"type\":\"ping\"}");
    return chat_passive_ping_id_ != 0;
}

void Application::BeginChatUnpair(const cJSON* root, ChatRequestContext context) {
    if (!context || !IsChatRequestCurrent(context) || chat_unpair_context_ ||
        lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load()) return;
    chat_unpair_context_ = context;
    CancelChatRecovery();
    const auto received_us = static_cast<uint64_t>(esp_timer_get_time());
    chat_unpair_deadline_us_ = received_us + 10000000ULL;
    chat_unpair_id_ = 0;
    chat_unpair_completed_ = false;
    const auto* request_id = cJSON_GetObjectItem(root, "request_id");
    if (!cJSON_IsString(request_id) || !request_id->valuestring || !request_id->valuestring[0] ||
        std::strlen(request_id->valuestring) > 64) {
        chat_unpair_completed_ = true;
        return;
    }
    try {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> ack(cJSON_CreateObject(), cJSON_Delete);
        if (!ack || !cJSON_AddStringToObject(ack.get(), "type", "system_ack") ||
            !cJSON_AddStringToObject(ack.get(), "command", "unpair") ||
            !cJSON_AddStringToObject(ack.get(), "request_id", request_id->valuestring)) return;
        std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(ack.get()), cJSON_free);
        if (encoded) chat_unpair_id_ = RequestChatConnectionText(encoded.get(), context, received_us);
    } catch (...) {
        // Keep the original teardown deadline when ACK allocation/admission fails.
    }
}

void Application::PollChatUnpair(uint64_t now_us) {
    if (!chat_unpair_context_) return;
    if (!IsChatRequestCurrent(chat_unpair_context_) || lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load()) {
        chat_unpair_context_.reset();
        chat_unpair_id_ = 0;
        return;
    }
    if (!chat_unpair_completed_ && now_us < chat_unpair_deadline_us_) return;
    auto context = std::move(chat_unpair_context_);
    chat_unpair_id_ = 0;
    EnterRepairPairingMode(std::move(context));
}

bool Application::RequestChatControl(ChatOutboundMailbox::Kind kind, int32_t argument, const std::string& payload, bool read_wake) {
    if (!IsSelectedNormalChatRoute()) return false;
    if (chat_protocol_owned_.load() || chat_source_connect_generation_.load() != connect_generation_.load()) return false;
    ChatControlIntents::Intent intent;
    if (!chat_protocol_signals_->TrySource(intent.source)) return false;
    intent.protocol_generation = protocol_generation_.load();
    intent.connect_generation = connect_generation_.load();
    intent.response_generation = speaking_generation_.load();
    intent.job.kind = kind;
    intent.job.argument = argument;
    intent.resolve_wake = read_wake;
    intent.job.deadline_us = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
    const size_t listen_slot = chat_rearm_phase_ == ChatRearmPhase::Pending ? 1 : 0;
    if (chat_control_intents_.Size() + listen_slot >= 4 ||
        !intent.job.SetPayload(payload.data(), payload.size()) || !chat_control_intents_.Push(intent)) {
        chat_protocol_infrastructure_fault_ = true;
        RecoverChatPlayout(215);
        return false;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    return true;
}

void Application::PollChatControls(uint64_t now_us) {
    auto* intent = chat_control_intents_.Front();
    if (!intent) return;
    if (intent->outcome == ChatControlIntents::Outcome::Superseded ||
        intent->outcome == ChatControlIntents::Outcome::Failed) {
        if (!intent->admitted || intent->reservation != chat_outbound_reservation_) chat_control_intents_.Pop();
        return;
    }
    if (!chat_protocol_signals_ || !chat_protocol_signals_->MatchesSource(intent->source) ||
        intent->protocol_generation != protocol_generation_.load() ||
        intent->connect_generation != connect_generation_.load()) {
        chat_control_intents_.Supersede();
        RetireChatOutbound();
        chat_wake_read_pending_ = false;
        chat_wake_read_result_.reset();
        if (chat_wake_read_serial_ != UINT32_MAX) ++chat_wake_read_serial_;
        return;
    }
    if (now_us >= intent->job.deadline_us) {
        intent->outcome = ChatControlIntents::Outcome::Failed;
        RecoverChatPlayout(216);
        return;
    }
    if (intent->resolve_wake) {
        if (!chat_wake_read_result_) {
            if (!chat_wake_read_pending_) {
                if (chat_wake_read_serial_ == UINT32_MAX) { RecoverChatPlayout(217); return; }
                ++chat_wake_read_serial_;
                chat_wake_read_pending_ = true;
            }
            PollChatAudioCleanup();
            return;
        }
        if (!intent->job.SetPayload(chat_wake_read_result_->data(), chat_wake_read_result_->size())) {
            intent->outcome = ChatControlIntents::Outcome::Failed;
            chat_wake_read_result_.reset();
            RecoverChatPlayout(218);
            return;
        }
        chat_wake_read_result_.reset();
        intent->resolve_wake = false;
    }
    if (!intent->admitted) {
        const auto result = SubmitChatOutbound(intent->job);
        if (result == ChatOutboundMailbox::Result::Sent) {
            intent->admitted = true;
            intent->reservation = chat_outbound_reservation_;
        }
        else if (result != ChatOutboundMailbox::Result::Busy) {
            intent->outcome = ChatControlIntents::Outcome::Failed;
            RecoverChatPlayout(219);
        }
    }
}

bool Application::DeliverChatControl(const ChatOutboundMailbox::Completion& completion) {
    auto* intent = chat_control_intents_.Front();
    if (!intent || !intent->admitted || intent->job.request_id != completion.job.request_id ||
        intent->job.generation != completion.job.generation || intent->job.protocol_generation != completion.job.protocol_generation ||
        intent->job.connection_epoch != completion.job.connection_epoch) return false;
    if (intent->outcome == ChatControlIntents::Outcome::Superseded ||
        intent->outcome == ChatControlIntents::Outcome::Failed) return true;
    intent->outcome = IsChatOutboundCompletionCurrent(completion) && completion.result == ChatOutboundMailbox::Result::Sent ?
        ChatControlIntents::Outcome::Sent : ChatControlIntents::Outcome::Failed;
    if (intent->outcome == ChatControlIntents::Outcome::Failed) RecoverChatPlayout(220);
    chat_control_intents_.Pop();
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND | MAIN_EVENT_STATE_CHANGED);
    return true;
}

bool Application::RetainChatActiveListen() {
    const bool listen = chat_rearm_admitted_ && !chat_rearm_delivery_;
    const bool ack = chat_playout_ack_admitted_ && !chat_playout_ready_;
    if (listen || ack) {
        ChatControlIntents::Intent active;
        active.source = chat_rearm_owner_.source;
        active.protocol_generation = chat_rearm_owner_.protocol_generation;
        active.connect_generation = chat_rearm_owner_.connect_generation;
        active.response_generation = speaking_generation_.load();
        active.job = listen ? chat_rearm_job_ : chat_playout_ack_;
        active.admitted = true;
        active.reservation = chat_outbound_reservation_;
        if (!chat_control_intents_.PrependActive(active)) {
            chat_protocol_infrastructure_fault_ = true;
            RecoverChatPlayout(221);
            return false;
        }
        chat_rearm_admitted_ = false;
        chat_playout_ack_admitted_ = false;
    }
    chat_rearm_phase_ = ChatRearmPhase::IdleComplete;
    return true;
}

bool Application::HandleChatStopListening() {
    CancelChatRecovery();
    if (!IsSelectedNormalChatRoute())
        return chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() && !lesson_runtime_active_.load();
    if (GetDeviceState() != kDeviceStateListening && chat_rearm_phase_ != ChatRearmPhase::Pending) return true;
    if (!chat_playout_stamp_) {
        ConnectionSource source;
        if (!audio_service_.IsCurrentChatPlaybackReset(chat_audio_reset_serial_))
            chat_audio_reset_serial_ = RequestChatPlaybackCleanup(speaking_generation_.load());
        if (!chat_protocol_signals_->TrySource(source) ||
            !EstablishChatPlayoutResponse({source, protocol_generation_.load(), connect_generation_.load(),
                speaking_generation_.load(), chat_audio_reset_serial_})) {
            RecoverChatPlayout(222);
            return true;
        }
    }
    if (!RetainChatActiveListen()) return true;
    chat_rearm_voice_intent_ = false;
    microphone_uplink_authorized_.store(false);
    RequestChatAudioCleanup(speaking_generation_.load(), false, false,
        IsDeviceClaimed() && !connect_in_flight_.load() && !lesson_asset_sync_quiet_.load());
    chat_rearm_phase_ = ChatRearmPhase::IdleComplete;
    RequestChatControl(ChatOutboundMailbox::Kind::ListenStop);
    chat_playout_ready_ = false;
    SetDeviceState(kDeviceStateIdle);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    return true;
}

bool Application::BeginChatListen(ListeningMode mode, ChatListenOrigin origin) {
    if (IsWifiConfigEntryPending()) return false;
    ConnectionSource available;
    if (origin == ChatListenOrigin::User && chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !lesson_runtime_active_.load() && (chat_recovery_.kind != ChatRecoveryIntent::Kind::None ||
        chat_protocol_owned_.load() || !chat_protocol_signals_->TrySource(available) ||
        chat_source_connect_generation_.load() != connect_generation_.load()))
        return RetainChatRecovery(ChatRecoveryIntent::Kind::Listen, mode);
    if (!IsSelectedNormalChatRoute() || (passive_ws_intent_.load() && origin != ChatListenOrigin::User)) return false;
    const auto state = GetDeviceState();
    if (!protocol_ || lesson_asset_sync_quiet_.load() ||
        (state != kDeviceStateIdle && state != kDeviceStateSpeaking && state != kDeviceStateListening &&
         state != kDeviceStateConnecting)) return false;
    ConnectionSource source;
    if (!chat_protocol_signals_->TrySource(source) || protocol_->CurrentConnectionEpoch() != source.connection_epoch) return false;
    if (origin == ChatListenOrigin::User) passive_ws_intent_.store(false);
    online_intent_.store(true);
    if (chat_rearm_phase_ == ChatRearmPhase::Pending) return true;
    if (chat_control_intents_.Size() >= 4) {
        chat_protocol_infrastructure_fault_ = true;
        RecoverChatPlayout(223);
        return true;
    }
    if (!chat_outbound_generation_) {
        if (chat_outbound_reservation_) {
            chat_start_obsolete_reservation_ = chat_outbound_reservation_;
        } else if (ActivateChatOutbound(source.connection_epoch) != ChatOutboundMailbox::Result::Sent) {
            RecoverChatPlayout(224);
            return true;
        }
    }
    if (!chat_playout_stamp_ || chat_playout_response_.source.source_id != source.source_id ||
        chat_playout_response_.source.connection_epoch != source.connection_epoch ||
        chat_playout_response_.protocol_generation != protocol_generation_.load() ||
        chat_playout_response_.connect_generation != connect_generation_.load() ||
        chat_playout_response_.response_generation != speaking_generation_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(chat_playout_response_.reset_token)) {
        auto generation = speaking_generation_.load();
        if (generation >= UINT32_MAX - 1) return false;
        speaking_generation_.store(++generation);
        const auto reset = RequestChatPlaybackCleanup(generation);
        if (!EstablishChatPlayoutResponse({source, protocol_generation_.load(), connect_generation_.load(), generation, reset})) return false;
    }
    chat_listen_origin_ = origin;
    chat_listen_received_us_ = static_cast<uint64_t>(esp_timer_get_time());
    chat_rearm_mode_ = mode;
    chat_rearm_phase_ = ChatRearmPhase::Pending;
    chat_rearm_voice_intent_ = true;
    chat_rearm_job_ = {};
    chat_rearm_delivery_.reset();
    chat_rearm_admitted_ = false;
    chat_playout_recovery_ = false;
    chat_playout_ready_ = false;
    microphone_uplink_authorized_.store(false);
    chat_rearm_prepared_ = RequestChatAudioCleanup(speaking_generation_.load(), false, true, false, true, false, ChatWakePolicy::Listening);
    chat_rearm_job_.kind = ChatOutboundMailbox::Kind::ListenStart;
    chat_rearm_job_.argument = mode;
    chat_rearm_job_.deadline_us = chat_listen_received_us_ + 10000000ULL;
    if (state != kDeviceStateConnecting) SetDeviceState(kDeviceStateSpeaking);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    return true;
}

bool Application::HandleChatAbort(AbortReason reason, bool resume) {
    CancelChatRecovery();
    if (!IsSelectedNormalChatRoute())
        return chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() && !lesson_runtime_active_.load();
    if (!RetainChatActiveListen()) return true;
    speaking_arm_dispatch_.Cancel();
    interrupt_count_.fetch_add(1, std::memory_order_relaxed);
    aborted_ = true;
    tts_audio_accepting_.store(false);
    last_speaking_activity_ms_.store(0);
    chat_rearm_voice_intent_ = false;
    microphone_uplink_authorized_.store(false);
    RequestChatAudioCleanup(speaking_generation_.load(), true, false,
        !resume && IsDeviceClaimed() && !connect_in_flight_.load() && !lesson_asset_sync_quiet_.load());
    RequestChatControl(ChatOutboundMailbox::Kind::Abort, reason);
    chat_playout_response_.reset_token = chat_audio_reset_serial_;
    chat_rearm_owner_ = chat_playout_response_;
    chat_listen_origin_ = ChatListenOrigin::Abort;
    chat_playout_begun_ = chat_playout_ready_ = false;
    chat_playout_controller_.Cancel();
    if (resume) BeginChatListen(GetDefaultListeningMode(), ChatListenOrigin::Abort);
    else {
        chat_rearm_phase_ = ChatRearmPhase::IdleComplete;
        SetDeviceState(kDeviceStateIdle);
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    }
    return true;
}

bool Application::HandleChatWake(const std::string& wake_word, bool read_worker) {
    if (IsWifiConfigEntryPending()) return true;
    if (!chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() || lesson_runtime_active_.load()) return false;
    ConnectionSource available;
    if (chat_recovery_.kind != ChatRecoveryIntent::Kind::None || chat_protocol_owned_.load() ||
        !chat_protocol_signals_->TrySource(available) ||
        chat_source_connect_generation_.load() != connect_generation_.load()) {
        const bool new_wake = chat_recovery_.kind == ChatRecoveryIntent::Kind::None ||
            chat_recovery_.kind == ChatRecoveryIntent::Kind::Background;
        if (RetainChatRecovery(ChatRecoveryIntent::Kind::Wake, kListeningModeAutoStop) && new_wake) {
            if (wake_word.size() > ChatOutboundMailbox::kMaxPayloadSize) { CancelChatRecovery(5); return true; }
            chat_recovery_.read_wake = read_worker;
            chat_recovery_.wake_size = wake_word.size();
            std::memcpy(chat_recovery_.wake_text.data(), wake_word.data(), wake_word.size());
        }
        return true;
    }
    const auto state = GetDeviceState();
    if (!protocol_ || lesson_asset_sync_quiet_.load() ||
        (state != kDeviceStateIdle && state != kDeviceStateSpeaking && state != kDeviceStateListening &&
         state != kDeviceStateConnecting)) {
        ESP_LOGI(TAG, "chat_recovery outcome=5");
        return true;
    }
    ConnectionSource source;
    if (!chat_protocol_signals_->TrySource(source) || protocol_->CurrentConnectionEpoch() != source.connection_epoch) return true;
    ESP_LOGI(TAG, "chat_recovery outcome=6");
    passive_ws_intent_.store(false);
    online_intent_.store(true);
    const bool active = state == kDeviceStateSpeaking || state == kDeviceStateListening;
    if (active)
        HandleChatAbort(kAbortReasonWakeWordDetected, false);
#if CONFIG_SEND_WAKE_WORD_DATA
    if (!active && !RequestChatControl(ChatOutboundMailbox::Kind::Wake, 0, wake_word, read_worker)) return true;
#else
    (void)wake_word;
    (void)read_worker;
#endif
    BeginChatListen(active ? GetDefaultListeningMode() : kListeningModeAutoStop, ChatListenOrigin::Wake);
    if (active && !RequestChatCue(Lang::Sounds::OGG_POPUP)) ESP_LOGW(TAG, "chat_popup_busy");
#if !CONFIG_SEND_WAKE_WORD_DATA
    if (!active && !RequestChatCue(Lang::Sounds::OGG_POPUP)) ESP_LOGW(TAG, "chat_popup_busy");
#endif
    return true;
}

void Application::HandleStopListeningEvent() {
    if (HandleChatStopListening()) return;
    auto state = GetDeviceState();
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() ||
        lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson stop listening ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (state == kDeviceStateListening) {
        lesson_interactive_listen_generation_.fetch_add(1);
        if (protocol_) {
            protocol_->SendStopListening();
        }
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (HandleChatWake({}, true)) return;
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet ignored wake word");
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson wake ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateIdle) {
        // NOTE: VAD-gate approach was attempted but failed — VAD runs inside
        // AudioProcessor which is only active in Listening state. In Idle,
        // raw mic data feeds the wake-word engine directly (audio_service.cc
        // line 274), so on_vad_change never fires before wake-word does.
        // To reduce false-positives, raise wake-word threshold in sdkconfig
        // (CONFIG_USE_AFE_WAKE_WORD_THRESHOLD) or add post-wake RMS check
        // on the buffered wake-word audio. Both are out of scope here.
        // Hardware evidence shows the temporary AFE Opus encoder corrupts execution
        // after close even when all encoder calls are serialized. Use live uplink only.

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue on a worker because OpenAudioChannel can block on
            // TCP/TLS/server hello long enough to starve the app/audio loop.
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        if (lesson_interactive_listen_pending_.load() ||
            lesson_interactive_listening_active_.load()) {
            ESP_LOGI(TAG, "lesson wake ignored state=%d", static_cast<int>(state));
            return;
        }
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    if (HandleChatWake(wake_word)) return;
    // Check state again in case it was changed during scheduling
    auto state = GetDeviceState();
    if (state != kDeviceStateConnecting && state != kDeviceStateIdle) {
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson wake continue ignored state=%d", static_cast<int>(state));
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (connect_in_flight_.load()) {
            if (passive_ws_intent_.load()) {
                deferred_wake_word_ = wake_word;
            }
            ESP_LOGW(TAG, "wake_audio_channel_open_deferred: connect already in flight");
            return;
        }
        reconnect_mode_ = GetDefaultListeningMode();
        uint32_t gen = ++connect_generation_;
        connect_in_flight_.store(true);
        connect_attempt_active_.store(true);  // WSS-8: suppress per-attempt error banner until terminal
        ArmConnectWatchdog();
        passive_ws_intent_.store(false);
        auto* ctx = new ConnectContext{this, reconnect_mode_, gen, wake_word, true, false};
        if (!StartOpenChannelWorker(ctx)) {
            delete ctx;
            connect_in_flight_.store(false);
            connect_attempt_active_.store(false);  // WSS-8: no worker -> cycle ended
            CancelConnectWatchdog();
            ESP_LOGE(TAG, "wake_ws_open worker unavailable -> idle");
            audio_service_.EnableWakeWordDetection(true);
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    FinishWakeWordInvoke(wake_word);
}

void Application::FinishWakeWordInvoke(const std::string& wake_word) {
    if (HandleChatWake(wake_word)) return;
    auto state = GetDeviceState();
    if (state != kDeviceStateConnecting && state != kDeviceStateIdle) {
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson wake finish ignored state=%d", static_cast<int>(state));
        if (state == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        audio_service_.EnableWakeWordDetection(true);
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    if (!protocol_->IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "wake_detect_send_failed -> reopen audio channel");
        SetDeviceState(kDeviceStateConnecting);
        Schedule([this, wake_word]() {
            ContinueWakeWordInvoke(wake_word);
        });
        return;
    }
    SetListeningMode(kListeningModeAutoStop);
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(kListeningModeAutoStop);
#endif
}

// H3: localized screen copy for a connect state. The connect-state spec table
// (kTbotConnectStateSpecs) is the single source of truth for WHAT copy a state
// shows; this returns the vi-VN-localized equivalent where a Lang::Strings key
// exists and falls back to the contract screen_text otherwise. Mirrors the
// RenderClaimSubstate() pattern so display copy never drifts from the contract.
static const char* ConnectStateScreenCopy(const TbotConnectStateSpec* spec) {
    switch (spec->state) {
        case TbotConnectState::BACKEND_CONNECTING:
            return Lang::Strings::CONNECTING;
        case TbotConnectState::ONLINE:
            return Lang::Strings::CONNECTED;
        case TbotConnectState::OFFLINE_RETRY:
            return Lang::Strings::SERVER_UNAVAILABLE_RETRYING;
        case TbotConnectState::OTA_UPDATING:
            return Lang::Strings::UPGRADING;
        case TbotConnectState::CLAIM_AVAILABLE:
            return Lang::Strings::READY_TO_CONNECT;
        case TbotConnectState::CLAIM_CONFIRM_TIMEOUT:
            return Lang::Strings::SETUP_EXPIRED;
        case TbotConnectState::BLE_SETUP_ADVERTISING:
            return Lang::Strings::SEARCHING_FOR_DEVICE;
        default:
            // No localized key for this state -> use the contract copy directly
            // (e.g. BOOT "Starting", WIFI_CONNECTING, BOOTSTRAP_FETCHING,
            // ERROR_RECOVERABLE "Hold button 5s to retry").
            return spec->screen_text;
    }
}

bool Application::IsMicrophoneUplinkAuthorized() const {
    if (!microphone_uplink_authorized_.load() ||
        passive_ws_intent_.load() || !online_intent_.load()) {
        return false;
    }
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateListening) {
        return !lesson_runtime_active_.load() ||
               lesson_interactive_listening_active_.load();
    }
    return state == kDeviceStateSpeaking &&
           listening_mode_ == kListeningModeRealtime &&
           !lesson_runtime_active_.load();
}

void Application::HandleListeningWatchdogTick() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

#if !CONFIG_USE_AUDIO_PROCESSOR || CONFIG_USE_DEVICE_AEC
    // Realtime silence expiry is safe only when the active processor supplies VAD.
    if (listening_mode_ == kListeningModeRealtime) {
        ESP_LOGD(TAG, "realtime_watchdog_disabled_without_vad");
        return;
    }
#endif

    const int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t started_ms = listening_started_ms_.load();
    int64_t last_activity_ms = last_listening_activity_ms_.load();
    if (started_ms <= 0 || last_activity_ms <= 0) {
        listening_started_ms_.store(now_ms);
        last_listening_activity_ms_.store(now_ms);
        return;
    }
    if (IsVoiceDetected()) {
        last_listening_activity_ms_.store(now_ms);
        last_activity_ms = now_ms;
    }

    const int64_t idle_ms = now_ms - last_activity_ms;
    const int64_t turn_ms = now_ms - started_ms;
    const uint32_t idle_limit_ms = listening_mode_ == kListeningModeAutoStop
        ? kListeningNoSpeechTimeoutMs
        : (listening_mode_ == kListeningModeRealtime
               ? kListeningRealtimeNoSpeechTimeoutMs
               : kListeningMaxTurnMs);
    const uint32_t turn_limit_ms = listening_mode_ == kListeningModeAutoStop
        ? kListeningAutoStopMaxTurnMs
        : kListeningMaxTurnMs;
    const bool turn_timed_out =
        listening_mode_ != kListeningModeRealtime && turn_ms >= turn_limit_ms;
    if (idle_ms < idle_limit_ms && !turn_timed_out) {
        return;
    }
    if (HandleChatStopListening()) {
        if (!RequestChatCue(Lang::Sounds::OGG_EXCLAMATION)) ESP_LOGW(TAG, "chat_timeout_cue_busy");
        return;
    }

    uint32_t decode_q = 0, send_q = 0, playback_q = 0;
    audio_service_.GetQueueDepths(decode_q, send_q, playback_q);
    auto audio_stats = audio_service_.GetDebugStatistics();
    ESP_LOGW(TAG,
             "listening_watchdog_timeout mode=%d idle_ms=%ld turn_ms=%ld decode_q=%lu send_q=%lu playback_q=%lu decode_drop=%lu encode_drop=%lu reconnects=%lu",
             static_cast<int>(listening_mode_),
             static_cast<long>(idle_ms),
             static_cast<long>(turn_ms),
             (unsigned long)decode_q,
             (unsigned long)send_q,
             (unsigned long)playback_q,
             (unsigned long)audio_stats.decode_drop_count,
             (unsigned long)audio_stats.encode_drop_count,
             (unsigned long)reconnect_count_.load());

    if (protocol_) {
        protocol_->SendStopListening();
    }
    audio_service_.EnableVoiceProcessing(false);
    microphone_uplink_authorized_.store(false);
    while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
    listening_started_ms_.store(0);
    last_listening_activity_ms_.store(0);
    lesson_interactive_listen_pending_.store(false);
    lesson_interactive_listening_active_.store(false);
    SetDeviceState(kDeviceStateIdle);
    auto display = Board::GetInstance().GetDisplay();
    if (lesson_runtime_active_.load()) {
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
    } else {
        display->SetStatus(Lang::Strings::SERVER_TIMEOUT);
        display->SetEmotion("thinking");
        audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    }
}

bool Application::IsChatRearmRecoveryCurrent() const {
    return chat_protocol_signals_ && chat_protocol_signals_ == chat_rearm_signals_ &&
        (chat_protocol_signals_->Capture() == chat_rearm_source_era_ || !chat_protocol_signals_->Capture()) &&
        !chat_protocol_owned_.load() && chat_rearm_owner_.protocol_generation == protocol_generation_.load() &&
        chat_rearm_owner_.connect_generation == connect_generation_.load() &&
        chat_rearm_owner_.response_generation == speaking_generation_.load() &&
        audio_service_.IsCurrentChatPlaybackReset(chat_rearm_owner_.reset_token);
}

bool Application::AdvanceChatRearm(uint64_t now_us) {
    using Phase = ChatRearmPhase;
    using Result = ChatOutboundMailbox::Result;
    if (!chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() ||
        lesson_runtime_active_.load()) return false;
    const auto state = GetDeviceState();
    if (state != kDeviceStateSpeaking && state != kDeviceStateListening && state != kDeviceStateIdle &&
        !(state == kDeviceStateConnecting && chat_rearm_phase_ == Phase::Pending)) return false;
    if (chat_rearm_phase_ == Phase::Recovery) {
        if (!IsChatRearmRecoveryCurrent()) return true;
        microphone_uplink_authorized_.store(false);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    if (!chat_playout_stamp_) return false;
    if (!chat_playout_ready_ && chat_rearm_phase_ == Phase::None) return true;
    const auto& response = chat_playout_response_;
    const bool current = chat_protocol_signals_->MatchesSource(response.source) &&
        !chat_protocol_owned_.load() && response.protocol_generation == protocol_generation_.load() &&
        response.connect_generation == connect_generation_.load() &&
        response.response_generation == speaking_generation_.load() &&
        audio_service_.IsCurrentChatPlaybackReset(response.reset_token);
    if (!current) {
        if (response.response_generation == speaking_generation_.load() &&
            chat_rearm_signals_ == chat_protocol_signals_ &&
            (chat_protocol_signals_->Capture() == chat_rearm_source_era_ || !chat_protocol_signals_->Capture()) &&
            audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
            RecoverChatPlayout(225); SetDeviceState(kDeviceStateIdle);
        }
        return true;
    }
    PollChatProtocolSignals();
    ChatPlayoutIntake::Stop terminal;
    const auto terminal_read = chat_listen_origin_ == ChatListenOrigin::Drain ?
        chat_protocol_signals_->intake.TryCollect(chat_playout_stamp_, terminal) : ChatPlayoutIntake::Read::None;
    if (terminal_read == ChatPlayoutIntake::Read::Fault ||
        (terminal_read == ChatPlayoutIntake::Read::Ready && (terminal.interrupt || terminal.conflict))) {
        RecoverChatPlayout(226); SetDeviceState(kDeviceStateIdle); return true;
    }
    const bool fault = chat_outbound_fault_ || chat_audio_fault_ || chat_playback_fault_ ||
        chat_protocol_infrastructure_fault_ || protocol_work_lifetime_.Pending() ||
        !protocol_ || protocol_->CurrentConnectionEpoch() != response.source.connection_epoch ||
        (chat_protocol_fault_ && chat_protocol_fault_generation_ == response.protocol_generation &&
         chat_protocol_fault_era_ == chat_protocol_signals_->Capture());
    if (fault || !online_intent_.load() || passive_ws_intent_.load()) {
        RecoverChatPlayout(227); SetDeviceState(kDeviceStateIdle); return true;
    }
    if ((chat_rearm_phase_ == Phase::Pending && state != kDeviceStateSpeaking && state != kDeviceStateConnecting) ||
        (chat_rearm_phase_ == Phase::Armed && state != kDeviceStateListening)) {
        RecoverChatPlayout(228); SetDeviceState(kDeviceStateIdle); return true;
    }
    if (chat_rearm_phase_ == Phase::Armed || chat_rearm_phase_ == Phase::IdleComplete) return true;
    const auto received_us = chat_listen_origin_ == ChatListenOrigin::Drain ? chat_playout_stop_.received_us : chat_listen_received_us_;
    if (now_us < received_us ||
        now_us - received_us >= ConversationPlayoutController::kTimeoutUs ||
        !online_intent_.load() || passive_ws_intent_.load()) {
        RecoverChatPlayout(229); SetDeviceState(kDeviceStateIdle); return true;
    }
    if (chat_rearm_phase_ == Phase::None) {
        const bool owned = (microphone_uplink_authorized_.load() || chat_rearm_voice_intent_) &&
            (state == kDeviceStateSpeaking || state == kDeviceStateListening);
        const bool resume = !chat_playout_stop_.explicit_manual_stop && owned &&
            (chat_playout_stop_.continue_listening ||
             (state == kDeviceStateSpeaking && listening_mode_ == kListeningModeRealtime));
        microphone_uplink_authorized_.store(false);
        if (!resume) {
            ESP_LOGW(TAG, "chat_rearm_idle site=401 owned=%u manual=%u continuation=%u",
                static_cast<unsigned>(owned), static_cast<unsigned>(chat_playout_stop_.explicit_manual_stop),
                static_cast<unsigned>(chat_playout_stop_.continue_listening));
            chat_rearm_voice_intent_ = false;
            chat_rearm_phase_ = Phase::IdleComplete;
            chat_playout_ready_ = false;
            RequestChatAudioCleanup(response.response_generation, false, false,
                IsDeviceClaimed() && !connect_in_flight_.load() && !lesson_asset_sync_quiet_.load());
            SetDeviceState(kDeviceStateIdle);
            return true;
        }
        chat_rearm_mode_ = chat_playout_stop_.continue_listening ?
            (chat_playout_stop_.realtime ? kListeningModeRealtime : GetDefaultListeningMode()) : listening_mode_;
        chat_rearm_phase_ = Phase::Pending;
        chat_rearm_voice_intent_ = true;
        chat_rearm_prepared_ = RequestChatAudioCleanup(response.response_generation, false, true, false,
            true, false, ChatWakePolicy::Listening);
        chat_rearm_job_ = {};
        chat_rearm_job_.kind = ChatOutboundMailbox::Kind::ListenStart;
        chat_rearm_job_.argument = chat_rearm_mode_;
        chat_rearm_job_.deadline_us = chat_playout_stop_.received_us + ConversationPlayoutController::kTimeoutUs;
        SetDeviceState(kDeviceStateSpeaking);
    }
    if (chat_control_intents_.Size()) return true;
    if (chat_start_obsolete_reservation_) return true;
    if (!chat_rearm_admitted_) {
        const auto result = SubmitChatOutbound(chat_rearm_job_);
        if (result == Result::Sent) chat_rearm_admitted_ = true;
        else if (result != Result::Busy) { RecoverChatPlayout(230); SetDeviceState(kDeviceStateIdle); }
        return true;
    }
    if (!chat_rearm_delivery_ || chat_audio_prepared_ != chat_rearm_prepared_ ||
        audio_service_.IsChatPlaybackResetPending() || chat_audio_reset_completed_ != response.reset_token) return true;
    if (!IsChatOutboundCompletionCurrent(*chat_rearm_delivery_) ||
        chat_rearm_delivery_->result != Result::Sent ||
        static_cast<uint64_t>(esp_timer_get_time()) - received_us >= ConversationPlayoutController::kTimeoutUs ||
        !audio_service_.ArmChatUplink(chat_rearm_prepared_)) {
        RecoverChatPlayout(231); SetDeviceState(kDeviceStateIdle); return true;
    }
    chat_rearm_phase_ = Phase::Armed;
    chat_playout_ready_ = false;
    listening_mode_ = chat_rearm_mode_;
    microphone_uplink_authorized_.store(true);
    const auto now_ms = esp_timer_get_time() / 1000;
    listening_started_ms_.store(now_ms);
    last_listening_activity_ms_.store(now_ms);
    SetDeviceState(kDeviceStateListening);
    return true;
}

void Application::RenderChatRearm() {
    // START admission can precede its confirmed Speaking state event.
    if (chat_rearm_phase_ == ChatRearmPhase::None && GetDeviceState() != kDeviceStateSpeaking) return;
    if (chat_rearm_phase_ == ChatRearmPhase::Recovery && !IsChatRearmRecoveryCurrent()) return;
    if (chat_rearm_phase_ != ChatRearmPhase::Recovery &&
        (!chat_protocol_signals_ || chat_rearm_signals_ != chat_protocol_signals_ ||
         !chat_protocol_signals_->MatchesSource(chat_rearm_owner_.source) ||
         chat_rearm_owner_.protocol_generation != protocol_generation_.load() ||
         chat_rearm_owner_.connect_generation != connect_generation_.load() ||
         chat_rearm_owner_.response_generation != speaking_generation_.load() ||
         !audio_service_.IsCurrentChatPlaybackReset(chat_rearm_owner_.reset_token))) return;
    if (chat_rearm_phase_ == chat_rearm_rendered_phase_ &&
        chat_rearm_owner_.reset_token == chat_rearm_rendered_reset_ &&
        backend_offline_.load() == chat_rearm_rendered_offline_) return;
    // Polling audio readiness must not continuously rebuild the same GIF.
    chat_rearm_rendered_phase_ = chat_rearm_phase_;
    chat_rearm_rendered_reset_ = chat_rearm_owner_.reset_token;
    chat_rearm_rendered_offline_ = backend_offline_.load();
    auto& board = Board::GetInstance();
    board.GetLed()->OnStateChanged();
    auto* display = board.GetDisplay();
    if (chat_rearm_phase_ == ChatRearmPhase::None) {
        display->SetStatus(Lang::Strings::SPEAKING);
    } else if (chat_rearm_phase_ == ChatRearmPhase::Pending) {
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
    } else if (chat_rearm_phase_ == ChatRearmPhase::Armed) {
        display->SetStatus(Lang::Strings::LISTENING);
        display->SetEmotion("thinking");
    } else if (chat_rearm_phase_ == ChatRearmPhase::IdleComplete || chat_rearm_phase_ == ChatRearmPhase::Recovery) {
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        const auto* spec = TbotConnectMapper::Resolve(GetDeviceState(), claim_substate_, GetBleSubstate(), backend_offline_.load());
        display->SetStatus(ConnectStateScreenCopy(spec));
        display->ClearChatMessages();
        display->SetEmotion(backend_offline_.load() ? "thinking" : "neutral");
    }
}

void Application::HandleStateChangedEvent() {
    ChatRuntimeTiming timing(2, []() { return static_cast<uint64_t>(esp_timer_get_time()); },
        [](uint32_t site, uint32_t hi, uint32_t lo) {
            ESP_LOGW(TAG, "chat_slow_scope site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                static_cast<unsigned>(site), static_cast<unsigned long>(hi), static_cast<unsigned long>(lo));
        });
    if (AdvanceChatRearm(static_cast<uint64_t>(esp_timer_get_time()))) {
        RenderChatRearm();
        return;
    }
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    if (new_state != kDeviceStateListening && new_state != kDeviceStateSpeaking) {
        microphone_uplink_authorized_.store(false);
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // H3 (LOCKED: all 21 states runtime-driven): resolve the live runtime to a
    // connect-state spec so the screen copy + timeout come from the contract
    // table, not hand-coded xiaozhi literals. The audio/wake-word side effects
    // per DeviceState are unchanged below; only the SetStatus SOURCE is
    // redirected through the mapper for the contract-owned states (BOOT,
    // WIFI_CONNECTING/CONNECTED, BOOTSTRAP_FETCHING, BACKEND_CONNECTING, ONLINE,
    // OTA_UPDATING, ERROR_RECOVERABLE). AP states stay defined-but-dormant.
    const TbotConnectStateSpec* connect_spec = TbotConnectMapper::Resolve(
        new_state, claim_substate_, GetBleSubstate(), backend_offline_.load());
    const char* connect_copy = ConnectStateScreenCopy(connect_spec);

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle: {
            if (IsWifiConfigEntryPending()) break;
            const bool suppress_lesson_idle_repaint =
                lesson_idle_repaint_suppressed_.exchange(false);
            if (lesson_runtime_active_.load()) {
                if (!suppress_lesson_idle_repaint) {
                    display->SetLessonCaption("");
                    display->ClearChatMessages();
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                }
                listening_started_ms_.store(0);
                last_listening_activity_ms_.store(0);
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
                break;
            }
            if (suppress_lesson_idle_repaint) {
                listening_started_ms_.store(0);
                last_listening_activity_ms_.store(0);
                audio_service_.EnableVoiceProcessing(false);
                if (IsDeviceClaimed() && !connect_in_flight_.load() &&
                    !lesson_asset_sync_quiet_.load()) {
                    audio_service_.EnableWakeWordDetection(true);
                } else {
                    audio_service_.EnableWakeWordDetection(false);
                }
                break;
            }
            // ONLINE (or OFFLINE_RETRY / a claim overlay) per the mapper.
            display->SetStatus(connect_copy);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion(backend_offline_.load() ? "thinking" : "neutral"); // Then set emotion (wechat mode checks child count)
            listening_started_ms_.store(0);
            last_listening_activity_ms_.store(0);
            audio_service_.EnableVoiceProcessing(false);
            // TBOT BLE+audio contention fix: the AFE/mic input only runs while
            // wake-word (or voice processing) is enabled — that is what feeds the
            // AFE FEED ringbuffer. While the device is UNCLAIMED it is sitting in
            // claimable standby (BLE advertising + claim poll) and has no lessons,
            // so we keep the mic OFF here. Running the AFE mic pipeline alongside
            // BLE advertising on real hardware overflows the FEED ringbuffer
            // ("Ringbuffer of AFE(FEED) is full") and errors the robot. Once the
            // device is CLAIMED, Idle enables wake-word exactly as before so
            // lessons (wake word -> talk) work normally. A fresh claim confirm
            // enables wake-word explicitly (see ConfirmPendingTbotClaim) so audio
            // comes up without a reboot.
            if (IsDeviceClaimed() && !connect_in_flight_.load() &&
                !lesson_asset_sync_quiet_.load()) {
                audio_service_.EnableWakeWordDetection(true);
            } else {
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        }
        case kDeviceStateConnecting:
            if (lesson_runtime_active_.load()) {
                if (lesson_interactive_listen_pending_.load()) {
                    display->ClearChatMessages();
                    display->SetStatus("Sắp đến lượt con...");
                } else {
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                }
                break;
            }
            // BACKEND_CONNECTING per the mapper ("Connecting...").
            display->SetStatus(connect_copy);
            display->SetEmotion(backend_offline_.load() ? "thinking" : "neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening: {
            microphone_uplink_authorized_.store(false);
            {
                int64_t now_ms = esp_timer_get_time() / 1000;
                listening_started_ms_.store(now_ms);
                last_listening_activity_ms_.store(now_ms);
            }
            const bool lesson_interactive_listen = lesson_interactive_listen_pending_.exchange(false);
            const bool lesson_interactive_active = lesson_interactive_listening_active_.load();
            if (lesson_interactive_listen || lesson_interactive_active) {
                lesson_interactive_listening_active_.store(true);
                display->ClearChatMessages();
                display->SetStatus("Con nói nhé...");
                display->SetChatMessage("system", "Con nói nhé.");
            } else {
                display->SetStatus(Lang::Strings::LISTENING);
                display->SetEmotion("thinking");
            }

            protocol_->SendStartListening(listening_mode_);
            if (!protocol_->IsAudioChannelOpened()) {
                ESP_LOGW(TAG, "listen_start_send_failed -> reconnect");
                ListeningMode mode = listening_mode_;
                audio_service_.EnableVoiceProcessing(false);
                SetDeviceState(kDeviceStateConnecting);
                Schedule([this, mode]() {
                    ContinueOpenAudioChannel(mode);
                });
                break;
            }

            const bool lesson_capture_requested = RequestChatLessonCapture();
            if (!lesson_capture_requested) microphone_uplink_authorized_.store(true);

            // Make sure the audio processor is running
            if (lesson_capture_requested || play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop && !aborted_) {
                    bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kListenPlaybackDrainTimeoutMs);
                    if (!playback_drained) {
                        ESP_LOGW(TAG,
                                 "playback_queue_drain_timeout timeout_ms=%lu action=force_listening",
                                 static_cast<unsigned long>(kListenPlaybackDrainTimeoutMs));
                    }
                }
                if (!lesson_capture_requested) audio_service_.EnableVoiceProcessing(true);
            }

            if (!lesson_capture_requested) {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            }
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (lesson_interactive_listen) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            } else if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        }
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            if (!lesson_runtime_active_.load()) {
                display->SetEmotion("happy");
            }
            listening_started_ms_.store(0);
            last_listening_activity_ms_.store(0);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            // NOTE: in Realtime mode we KEEP wake-word + voice-processing
            // running so user can barge in. Echo from speaker is suppressed
            // by device-side AEC (CONFIG_USE_DEVICE_AEC=y) BEFORE the
            // signal reaches the wake-word ML, so echo no longer false-fires.
            break;
        case kDeviceStateStarting:
            // BOOT per the mapper ("Starting").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateActivating:
            // BOOTSTRAP_FETCHING per the mapper ("Loading setup...").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateUpgrading:
            // OTA_UPDATING per the mapper ("Updating...").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateFatalError:
            // ERROR_RECOVERABLE per the mapper ("Hold button 5s to retry").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateWifiConfiguring:
            // H2: entering Wi-Fi setup -> stop the heartbeat (not a live online
            // session; it (re)starts only from OnConnected).
            display->SetStatus(connect_copy);
            StopHeartbeat();
            StopClaimPoll();
            if (chat_cleanup_enabled_) RequestChatAudioCleanup(speaking_generation_.load(), false, false, false);
            else {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ScheduleDeferredProtocolClose(Protocol* expected,
                                                uint32_t connection_epoch) {
    const uint64_t expected_generation = protocol_generation_.load(std::memory_order_acquire);
    Schedule([this, expected, expected_generation, connection_epoch]() {
        if (ProtocolLifetimeMatches(
                protocol_.get(), expected,
                protocol_generation_.load(std::memory_order_acquire),
                expected_generation)) {
            RetireChatOutbound();
            if (chat_cleanup_enabled_) {
                deferred_close_generation_ = expected_generation;
                deferred_close_epoch_ = connection_epoch;
                protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
                PollChatProtocolCleanup();
                return;
            }
            if (protocol_work_lifetime_.Busy()) {
                deferred_close_generation_ = expected_generation;
                deferred_close_epoch_ = connection_epoch;
                protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
                return;
            }
            protocol_->CompleteDeferredClose(connection_epoch);
        }
    });
}

bool Application::ScheduleAndWait(std::function<bool()>&& callback, int timeout_ms) {
    struct WaitState {
        enum Status { kPending, kRunning, kDone, kCancelled };
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        std::atomic<Status> status{kPending};
        std::atomic<bool> result{false};
        ~WaitState() { if (done != nullptr) vSemaphoreDelete(done); }
    };
    auto state = std::make_shared<WaitState>();
    if (state->done == nullptr) return false;
    Schedule([state, callback = std::move(callback)]() mutable {
        auto expected = WaitState::kPending;
        if (!state->status.compare_exchange_strong(expected, WaitState::kRunning)) return;
        state->result.store(callback());
        state->status.store(WaitState::kDone);
        xSemaphoreGive(state->done);
    });
    if (xSemaphoreTake(state->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return state->result.load();
    }
    auto expected = WaitState::kPending;
    if (state->status.compare_exchange_strong(expected, WaitState::kCancelled)) return false;
    if (expected == WaitState::kRunning) {
        xSemaphoreTake(state->done, portMAX_DELAY);
    }
    return state->status.load() == WaitState::kDone && state->result.load();
}

void Application::RunScheduledTasks() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto tasks = std::move(main_tasks_);
    lock.unlock();
    for (auto& task : tasks) {
        task();
    }
}

void Application::ArmSpeakingTimeout() {
    auto current_generation = speaking_generation_.load();
    speaking_timeout_generation_.store(current_generation, std::memory_order_relaxed);
    if (speaking_timeout_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* app = static_cast<Application*>(arg);
                auto generation = app->speaking_timeout_generation_.load(std::memory_order_relaxed);
                app->Schedule([app, generation]() {
                    app->HandleSpeakingTimeout(generation);
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "speaking_timer",
            .skip_unhandled_events = true
        };
        auto err = esp_timer_create(&timer_args, &speaking_timeout_timer_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "speaking_timeout_timer_create_failed err=%s generation=%lu",
                     esp_err_to_name(err), (unsigned long)current_generation);
            return;
        }
    }
    esp_timer_stop(speaking_timeout_timer_);
    auto err = esp_timer_start_once(speaking_timeout_timer_, kSpeakingTimeoutMs * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaking_timeout_timer_start_failed err=%s generation=%lu",
                 esp_err_to_name(err), (unsigned long)current_generation);
    }
}

void Application::HandleSpeakingTimeout(uint32_t generation) {
    if (generation != speaking_generation_.load() || GetDeviceState() != kDeviceStateSpeaking) {
        return;
    }

    auto now_ms = esp_timer_get_time() / 1000;
    auto last_activity_ms = last_speaking_activity_ms_.load();
    if (last_activity_ms > 0 && now_ms - last_activity_ms < kSpeakingTimeoutMs) {
        ArmSpeakingTimeout();
        return;
    }
    if (HandleChatAbort(kAbortReasonNone, listening_mode_ == kListeningModeRealtime)) {
        if (listening_mode_ != kListeningModeRealtime && !RequestChatCue(Lang::Sounds::OGG_EXCLAMATION))
            ESP_LOGW(TAG, "chat_timeout_cue_busy");
        return;
    }

    ESP_LOGW(TAG, "speaking_timeout generation=%lu idle_ms=%ld",
             (unsigned long)generation,
             static_cast<long>(last_activity_ms > 0 ? now_ms - last_activity_ms : -1));
    speaking_arm_dispatch_.Cancel();
    tts_audio_accepting_.store(false);
    ++speaking_generation_;
    // Publish the new generation (cancel path) so late frames from the timed-out
    // response are gen-gated out at dequeue.
    audio_service_.SetPlaybackGeneration(speaking_generation_.load());
    last_speaking_activity_ms_.store(0);
    aborted_ = true;
    audio_service_.ResetDecoder();
    if (protocol_) {
        protocol_->SendAbortSpeaking(kAbortReasonNone);
    }
    const bool lesson_answer_turn =
        lesson_runtime_active_.load() && lesson_interactive_listen_pending_.load();
    if (!lesson_answer_turn) {
        CancelLessonInteractiveListening();
    }
    auto show_timeout_cue = [this]() {
        auto display = Board::GetInstance().GetDisplay();
        if (lesson_runtime_active_.load()) {
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
        } else {
            display->SetStatus(Lang::Strings::SERVER_TIMEOUT);
            display->SetEmotion("thinking");
            audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    };
    if (listening_mode_ == kListeningModeManualStop) {
        if (lesson_answer_turn) {
            SetDeviceState(kDeviceStateListening);
            ESP_LOGI(TAG, "lesson prompt timeout -> listening");
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        show_timeout_cue();
    } else if (listening_mode_ == kListeningModeAutoStop) {
        SetDeviceState(kDeviceStateIdle);
        show_timeout_cue();
    } else {
        SetDeviceState(kDeviceStateListening);
        const uint64_t resumed_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "mic_loop_resumed ts=%lu%03lu reason=speaking_timeout",
                 static_cast<unsigned long>(resumed_ms / 1000),
                 static_cast<unsigned long>(resumed_ms % 1000));
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    if (HandleChatAbort(reason, listening_mode_ != kListeningModeManualStop)) return;
    speaking_arm_dispatch_.Cancel();
    ESP_LOGI(TAG, "Abort speaking");
    interrupt_count_.fetch_add(1, std::memory_order_relaxed);  // OBS-2
    aborted_ = true;
    tts_audio_accepting_.store(false);
    ++speaking_generation_;
    // Publish the new generation so any in-flight/late decode frame from the
    // cancelled response is gen-gated out even if it slips past ResetDecoder.
    audio_service_.SetPlaybackGeneration(speaking_generation_.load());
    last_speaking_activity_ms_.store(0);
    audio_service_.ResetDecoder();
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
    // BARGE-4: make sure we actually leave SPEAKING. Some callers transition
    // themselves; guard so we only move when still SPEAKING (don't override a
    // caller that already advanced the state).
    if (GetDeviceState() == kDeviceStateSpeaking) {
        SetDeviceState(listening_mode_ == kListeningModeManualStop
                           ? kDeviceStateIdle
                           : kDeviceStateListening);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    if (chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() && !lesson_runtime_active_.load()) {
        BeginChatListen(mode, ChatListenOrigin::User);
        return;
    }
    passive_ws_intent_.store(false);
    online_intent_.store(true);
    const bool already_listening = GetDeviceState() == kDeviceStateListening;
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
    if (already_listening) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    }
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot(ChatRequestContext context) {
    if (!IsChatRequestCurrent(context)) return;
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson reboot ignored");
        return;
    }
    Schedule([this, context]() {
        if (!IsChatRequestCurrent(context)) return;
        reboot_pending_.store(true);
        CloseAudioChannelByIntent();
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReboot);
        CompletePendingProtocolWork();
    });
}

void Application::CompleteReboot() {
    RetireChatOutbound();
    if (chat_cleanup_enabled_) {
        reboot_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReboot);
        PollChatProtocolCleanup();
        return;
    }
    if (protocol_work_lifetime_.Busy()) {
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReboot);
        return;
    }
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        CloseAudioChannelByIntent();
    }
    CancelLessonRobotEntranceOnDisplay();
    protocol_.reset();
    protocol_generation_.fetch_add(1, std::memory_order_acq_rel);
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::IsConnectSuccessPublicationSuppressed() const {
    return protocol_work_lifetime_.Pending() || connect_close_deferral_.Pending() ||
           reset_pending_.load() ||
           reboot_pending_.load();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson firmware upgrade ignored");
        return false;
    }
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        CloseAudioChannelByIntent();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade");

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        if (!audio_service_.Start()) {
            ESP_LOGE(TAG, "Firmware upgrade rollback could not restart audio service");
        }
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() && !lesson_runtime_active_.load()) {
        if (lesson_asset_sync_quiet_.load()) return;
        if (GetDeviceState() == kDeviceStateSpeaking)
            HandleChatAbort(kAbortReasonNone, listening_mode_ != kListeningModeManualStop);
        else if (GetDeviceState() == kDeviceStateListening) CloseAudioChannelByIntent();
        else HandleChatWake(wake_word);
        return;
    }
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet ignored direct wake");
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson direct wake ignored state=%d", static_cast<int>(state));
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                CloseAudioChannelByIntent();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson sleep mode blocked");
        return false;
    }

    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload, ChatRequestContext context) {
    const auto received_us = static_cast<uint64_t>(esp_timer_get_time());
    // Always schedule to run in main task for thread safety
    try { Schedule([this, payload = std::move(payload), context, received_us]() {
        if (!IsChatRequestCurrent(context)) return;
        if (context) {
            try { RequestChatConnectionText(context->EncodeMcpReply(payload), context, received_us); }
            catch (...) { FailChatRequest(context); }
            return;
        }
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    }); } catch (...) {
        if (!context) throw;
        FailChatRequest(context);
    }
}

void Application::SetAecMode(AecMode mode) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson aec mode ignored");
        return;
    }
    Schedule([this, mode]() {
        if (lesson_runtime_active_.load()) {
            ESP_LOGI(TAG, "scheduled lesson aec mode ignored");
            return;
        }
        aec_mode_ = mode;
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            CloseAudioChannelByIntent();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    if (chat_cleanup_enabled_.load() && !IsLessonVoiceRoute()) {
        if (!RequestChatCue(sound)) ESP_LOGW(TAG, "chat_cue_busy_or_unavailable");
        return;
    }
    audio_service_.PlaySound(sound);
}

void Application::CloseAudioChannelByIntent() {
    if (xTaskGetCurrentTaskHandle() != application_task_) {
        Schedule([this]() { CloseAudioChannelByIntent(); });
        return;
    }
    // User/system-initiated close: we no longer want an open channel, so
    CancelChatRecovery();
    RetireChatOutbound();
    // OnAudioChannelClosed must NOT auto-reconnect. Cancel any pending retry.
    deferred_wake_word_.clear();
    passive_ws_intent_.store(false);
    reconnect_passive_.store(false);
    online_intent_.store(false);
    microphone_uplink_authorized_.store(false);
    reconnect_attempt_ = 0;
    passive_reconnect_attempt_ = 0;
    backend_recovery_window_.Reset();
    connect_attempt_active_.store(false);
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    if (chat_cleanup_enabled_) {
        ++connect_generation_;
        connect_close_deferral_.Request(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
        PollChatProtocolCleanup();
        return;
    }
    if (!connect_close_deferral_.Request(protocol_work_lifetime_.Busy())) {
        ++connect_generation_;
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
        ESP_LOGI(TAG, "channel_close_deferred_until_connect_worker_exit");
        return;
    }
    if (protocol_) {
        protocol_->CloseAudioChannel();
    }
}

bool Application::CompletePendingProtocolWork() {
    if (chat_cleanup_enabled_) return PollChatProtocolCleanup();
    if (protocol_work_lifetime_.Pending()) RetireChatOutbound();
    using Action = ProtocolWorkLifetime::Action;
    const auto action = protocol_work_lifetime_.TakeReady();
    if (action == Action::kNone) return protocol_work_lifetime_.Pending();
    connect_in_flight_.store(false);
    CancelConnectWatchdog();
    const bool intentional_close = connect_close_deferral_.TakeAfterWorker();
    reset_pending_.store(false);
    protocol_reinit_pending_.store(false);
    if (action == Action::kReboot) {
        reboot_pending_.store(false);
        protocol_activation_pending_ = ProtocolActivation::kNone;
        claim_protocol_completion_pending_ = false;
        if (protocol_heap_monitor_pending_) {
            SystemInfo::StopHeapPhaseMonitor();
            protocol_heap_monitor_pending_ = false;
        }
        CompleteReboot();
        return true;
    }
    if (action == Action::kReinitialize || action == Action::kReset) {
        protocol_start_pending_generation_ = 0;
        deferred_close_generation_ = 0;
        if (protocol_heap_monitor_pending_) {
            SystemInfo::StopHeapPhaseMonitor();
            protocol_heap_monitor_pending_ = false;
        }
        DoResetProtocol();
        if (action == Action::kReinitialize) {
            InitializeProtocol();
        } else {
            protocol_activation_pending_ = ProtocolActivation::kNone;
            claim_protocol_completion_pending_ = false;
        }
        return true;
    }
    if (protocol_) {
        if (intentional_close) protocol_->CloseAudioChannel();
        else if (deferred_close_generation_ == protocol_generation_.load()) {
            protocol_->CompleteDeferredClose(deferred_close_epoch_);
        }
    }
    deferred_close_generation_ = 0;
    if (protocol_start_pending_generation_ == 0) CompleteProtocolActivation();
    return true;
}

void Application::DoResetProtocol() {
    RetireChatOutbound();
    if (chat_cleanup_enabled_) {
        reset_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReset);
        PollChatProtocolCleanup();
        return;
    }
    if (protocol_work_lifetime_.Busy()) {
        reset_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReset);
        return;
    }
    RequestLessonStorageAbandonment();
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        CloseAudioChannelByIntent();
    }
    CancelLessonRobotEntranceOnDisplay();
    protocol_.reset();
    protocol_generation_.fetch_add(1, std::memory_order_acq_rel);
}

void Application::ResetProtocol() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "ResetProtocol ignored during lesson");
        return;
    }
    Schedule([this]() {
        ++connect_generation_;  // invalidate any in-flight connect's result
        reset_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReset);
        CompletePendingProtocolWork();
    });
}
