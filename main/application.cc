#include "application.h"
#include "board.h"
#include "display.h"
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
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "boards/common/blufi.h"
#include "boards/common/system_reset.h"
#include <ssid_manager.h>
#include <wifi_manager.h>
#endif

#include <ctime>
#include <cstring>
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

#define TAG "Application"

static constexpr uint32_t kListenPlaybackDrainTimeoutMs = 650;
static constexpr uint32_t kSpeakingTimeoutMs = 12000;
static constexpr uint32_t kTtsStopPlaybackDrainTimeoutMs = 15000;
static constexpr uint32_t kListeningNoSpeechTimeoutMs = 15000;
static constexpr uint32_t kListeningAutoStopMaxTurnMs = 10000;
static constexpr uint32_t kListeningMaxTurnMs = 60000;
static constexpr int kWakeWordAudioChannelOpenMaxAttempts = 3;
static constexpr uint32_t kWakeWordAudioChannelRetryDelayMs = 700;
static constexpr uint64_t kConnectWatchdogTimeoutUs = 35ULL * 1000000ULL;
static constexpr uint32_t kMaxAudioPacketsPerMainLoop = 4;
static constexpr UBaseType_t kLessonMessageQueueDepth = 16;
static constexpr uint32_t kLessonMessageWorkerStackBytes = 12288;

// TBOT claim poll (C4): cadence 10s. The backend's 5-minute cap applies only
// after a pending claim exists; unclaimed standby must keep polling so a late
// phone scan can still find and claim the robot.
static constexpr uint64_t kClaimPollIntervalUs = 10ULL * 1000000ULL;      // 10s (was 4s: blocking HTTP/TLS poll was hammering main task + flaky backend)
// "Hi ESP needs many tries" fix: once the realtime WS is up (online_intent_)
// the device is fully functional and the claim poll is pure background. Back it
// off hard so a residual still-polling state (e.g. online-but-not-yet-claimed)
// cannot keep waking the network stack every 10s and jittering live audio.
static constexpr uint64_t kClaimPollIntervalIdleUs = 60ULL * 1000000ULL;  // 60s while online
static constexpr int64_t kClaimPollWindowMs = 5LL * 60LL * 1000LL;        // 5 min cap

// TBOT heartbeat (C5): POST /v1/device/heartbeat every 20s while claimed/online.
static constexpr uint64_t kHeartbeatIntervalUs = 20ULL * 1000000ULL;     // 20s


Application::Application() {
    event_group_ = xEventGroupCreate();

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
    if (heartbeat_timer_ != nullptr) {
        esp_timer_stop(heartbeat_timer_);
        esp_timer_delete(heartbeat_timer_);
    }
    if (speaking_timeout_timer_ != nullptr) {
        esp_timer_stop(speaking_timeout_timer_);
        esp_timer_delete(speaking_timeout_timer_);
    }
    if (lesson_message_task_handle_ != nullptr) {
        vTaskDelete(lesson_message_task_handle_);
        lesson_message_task_handle_ = nullptr;
    }
    if (lesson_message_queue_ != nullptr) {
        char* payload = nullptr;
        while (xQueueReceive(lesson_message_queue_, &payload, 0) == pdTRUE) {
            if (payload != nullptr) cJSON_free(payload);
        }
        vQueueDelete(lesson_message_queue_);
        lesson_message_queue_ = nullptr;
    }
    vEventGroupDelete(event_group_);
}

void Application::EnqueueLessonMessage(const cJSON* root) {
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const cJSON* sequence = cJSON_GetObjectItem(root, "sequence");
    const char* type_value = cJSON_IsString(type) ? type->valuestring : "(missing)";
    const int sequence_value = cJSON_IsNumber(sequence) ? sequence->valueint : -1;

    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        ESP_LOGW(TAG, "lesson_* dropped: worker unavailable type=%s seq=%d",
                 type_value, sequence_value);
        return;
    }

    char* payload = cJSON_PrintUnformatted(root);
    if (payload == nullptr) {
        ESP_LOGW(TAG, "lesson_* dropped: serialize failed type=%s seq=%d",
                 type_value, sequence_value);
        return;
    }

    if (xQueueSend(lesson_message_queue_, &payload, 0) != pdTRUE) {
        ESP_LOGW(TAG, "lesson_* dropped: worker queue full type=%s seq=%d",
                 type_value, sequence_value);
        cJSON_free(payload);
    } else {
        ESP_LOGI(TAG, "lesson_* enqueued type=%s seq=%d bytes=%u",
                 type_value, sequence_value, (unsigned)strlen(payload));
    }
}

void Application::LessonMessageTask(void* arg) {
    auto* self = static_cast<Application*>(arg);
    char* payload = nullptr;
    for (;;) {
        if (xQueueReceive(self->lesson_message_queue_, &payload, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        cJSON* root = cJSON_Parse(payload);
        if (root != nullptr) {
            const cJSON* type = cJSON_GetObjectItem(root, "type");
            const cJSON* sequence = cJSON_GetObjectItem(root, "sequence");
            ESP_LOGI(TAG, "lesson_worker handling type=%s seq=%d",
                     cJSON_IsString(type) ? type->valuestring : "(missing)",
                     cJSON_IsNumber(sequence) ? sequence->valueint : -1);
            self->HandleLessonMessage(root);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "lesson_* dropped: worker parse failed");
        }
        cJSON_free(payload);
        payload = nullptr;
    }
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    robot_uart_.Initialize();

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
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        // req#1: bounded wait (was portMAX_DELAY) so the loop always makes a pass,
        // feeds the watchdog, and never blocks forever.
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
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
            static uint32_t send_event_count = 0;
            static uint32_t send_packet_count = 0;
            static uint32_t lesson_render_defer_count = 0;
            send_event_count++;
            if (IsLessonNetworkRenderQuiet()) {
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
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            HandleListeningWatchdogTick();

            if (clock_ticks_ % 10 == 0 &&
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
                // Audio realtime metrics snapshot: queue depths + drop/stale
                // counters. Cheap, on the app task (NOT the audio hot path), so
                // it never jitters capture/playback. Lets us measure backpressure
                // (Patch 3.1/3.2) and barge-in stale-frame drops (Patch 3.3).
                uint32_t decode_q = 0, send_q = 0, playback_q = 0;
                audio_service_.GetQueueDepths(decode_q, send_q, playback_q);
                auto audio_stats = audio_service_.GetDebugStatistics();
                ESP_LOGI(TAG, "audio_metrics decode_q=%lu send_q=%lu playback_q=%lu input_count=%lu wake_running=%d vp_running=%d decode_drop=%lu encode_drop=%lu stale_frames=%lu interrupts=%lu reconnects=%lu",
                         (unsigned long)decode_q, (unsigned long)send_q, (unsigned long)playback_q,
                         (unsigned long)audio_stats.input_count,
                         audio_service_.IsWakeWordRunning() ? 1 : 0,
                         audio_service_.IsAudioProcessorRunning() ? 1 : 0,
                         (unsigned long)audio_stats.decode_drop_count,
                         (unsigned long)audio_stats.encode_drop_count,
                         (unsigned long)audio_stats.stale_frame_count,
                         (unsigned long)interrupt_count_.load(),
                         (unsigned long)reconnect_count_.load());
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
        // UI freezes on "Loading setup..." forever. Unclaimed devices cannot
        // finish cloud bootstrap without a parent claim — exit Activating now.
        if (!IsDeviceClaimed()) {
            ESP_LOGW(TAG,
                     "Unclaimed device on Wi-Fi: skip activation worker "
                     "(heap too tight with BLE; leave claim-standby)");
            if (!ota_) {
                ota_ = std::make_unique<Ota>();
                ota_->MarkCurrentVersionValid();
            }
            xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
            return;
        }
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
    // H2: network is gone -> stop the heartbeat (no live online session to report
    // and no point blocking the main task on an unreachable backend). It restarts
    // only from OnConnected.
    StopHeartbeat();

    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    auto display = Board::GetInstance().GetDisplay();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        backend_offline_.store(true);
        audio_service_.ResetDecoder();
        CloseAudioChannelByIntent();
        if (lesson_runtime_active_.load()) {
            lesson_interactive_listen_generation_.fetch_add(1);
            lesson_interactive_listen_pending_.store(false);
            lesson_interactive_listening_active_.store(false);
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
        } else {
            display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
            display->SetEmotion("thinking");
            audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    }

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
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

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    RefreshPendingTbotClaim();

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::RefreshPendingTbotClaim() {
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

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    {
        const auto ble_state = Blufi::GetInstance().GetBleState();
        if (!pending_tbot_claim_.active && !token.empty() &&
            (ble_state == Blufi::BleState::kAdvertising ||
             ble_state == Blufi::BleState::kConnected)) {
            // We ALREADY hold a bootstrap token but BLE is still active - e.g. a
            // flaky BLE link (MIUI BLE contention) never reached the clean
            // wifi-success deinit path. Do NOT strand behind this gate forever:
            // free the BLE radio now (same StopBleAdvertising-before-TLS pattern as
            // the confirm path below) so the blocking TLS claim fetch/confirm can
            // run, then fall through instead of returning.
            ESP_LOGW(TAG, "Bootstrap token present but BLE still active; stopping BLE to proceed with claim fetch/confirm");
            Blufi::GetInstance().CancelBleSetupTimeout();
            StopBleAdvertising();
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
        pending_tbot_claim_token_.clear();
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
        pending_tbot_claim_token_ = token;
        StopClaimPoll();
        StopBleAdvertising();
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        ESP_LOGI(TAG, "Cached pending claim has BLE bootstrap token -> auto-confirming");
        ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);
        if (claim_substate_ != TbotClaimSubstate::Confirmed) {
            ESP_LOGW(TAG, "Auto-confirm POST did not land; clearing stale claim token + reopening BLE");
            Settings websocket_settings("websocket", true);
            websocket_settings.SetString("bootstrap_token", "");
            pending_tbot_claim_token_.clear();
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            EnsureBleAdvertisingForStandby();
            StartClaimPoll();
        }
        return;
    }

    if (pending_tbot_claim_.active && token.empty()) {
        ESP_LOGW(TAG, "Pending claim cached but no BLE bootstrap token yet; keeping BLE advertising");
        pending_tbot_claim_api_url_ = api_url;
        pending_tbot_claim_token_.clear();
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
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
    DispatchPendingTbotClaimFetch(api_url, token);
}

// Off-task continuation: runs back on the Application task (Schedule()d from
// ClaimFetchTask) so every claim_substate_/pending_tbot_claim_*/BLE/SetDeviceState
// mutation below stays serialized on the one task that owns them (OQ1). This is
// the verbatim result-handling tail of the old RefreshPendingTbotClaim(); only
// the blocking fetch moved off-task.
void Application::ApplyPendingTbotClaimFetchResult(const std::string& api_url,
                                                   const std::string& token,
                                                   const PendingTbotClaim& pending_claim,
                                                   bool fetched, int device_config_status) {
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
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        pending_tbot_claim_token_.clear();
        claim_fetch_failures_ = 0;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
        return;
    }
    if (!fetched || !pending_claim.active) {
        // Unowned / no pending claim yet -> enter claimable standby ("Ready to
        // connect") and run a bounded poll so a phone tap is caught without a
        // tight loop. This is the C4 "if unowned -> claimable standby" trigger.
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_ = api_url;
        pending_tbot_claim_token_ = token;

        // Unclaimed + claimable standby is exactly the state the mobile app
        // discovers over BLE: start (or keep) advertising "TBOT-<MAC>" so the
        // app's BLE scan can find this robot and begin the cloud claim. This
        // call is idempotent (guards against double-init) and re-arms the BLE
        // hard-timeout each poll so advertising persists while we stay unclaimed.
        EnsureBleAdvertisingForStandby();

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
        // Give up the poll ONLY when the device is CLAIMED. The give-up exists
        // because the blocking poll on the main task starved the wake-word/AFE
        // pipeline — but an UNCLAIMED robot now runs NO audio at all (the realtime
        // WS + the AFE mic input are deferred until claimed), so there is nothing
        // to starve, AND it must stay claimable: a transient free-tier backend
        // hiccup must NOT permanently stop the poll. If it did, BLE is torn down
        // after the 300s setup timeout and the screen shows "Hết hạn thiết lập"
        // (Setup expired), leaving the robot un-pairable until a reboot — exactly
        // the failure observed. So while UNCLAIMED we keep polling + advertising
        // indefinitely; claim_fetch_failures_ only drives the "Server unavailable,
        // retrying" copy, never a give-up.
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
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
        return;
    }

    StopClaimPoll();
    // We have the claim bootstrap token in hand; BLE discovery/custom-data has
    // served its purpose for this attempt. Stop it before TLS confirm to reduce
    // BLE+Wi-Fi heap/radio contention.
    StopBleAdvertising();
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
    // call succeeds and turns the mic on. Callers also rely on its SYNCHRONOUS
    // post-condition (they inspect claim_substate_ == Confirmed immediately after,
    // e.g. just below at the stale-token-clear block and in the cached-token path
    // of RefreshPendingTbotClaim) to clear a stale bootstrap token + reopen BLE;
    // making it async would break that. It persists the claimed flag + websocket
    // credentials, brings audio up, and Alerts CONNECTED on success (or a
    // localized failure copy on error). The BOOT-button path stays wired as a
    // manual fallback if a future build wants to re-enable explicit consent.
    ESP_LOGI(TAG, "Pending claim detected -> auto-confirming (press-to-allow skipped by product decision)");
    ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);

    // Resilience: if the confirm POST did not land (claim_substate_ stays
    // WaitingConfirm rather than Confirmed) — typically a transient HTTP -1 on a
    // weak Wi-Fi link / BLE+TLS contention — do NOT strand on a stale pending
    // claim. Stranding would (a) leave the robot un-confirmed forever and (b) make
    // a later BOOT press dead-end on "Hết hạn thiết lập" (Setup expired) via the
    // stale claim's expiry check. Clear the consumed/failed bootstrap token and
    // re-open BLE so the phone can send a fresh token instead of us retrying a
    // known-bad Authorization bearer forever.
    if (claim_substate_ != TbotClaimSubstate::Confirmed) {
        ESP_LOGW(TAG, "Auto-confirm POST did not land; clearing stale claim token + reopening BLE");
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        pending_tbot_claim_token_.clear();
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
    }
}

bool Application::ConfirmPendingTbotClaim(bool trust_backend_expiry) {
    if (!pending_tbot_claim_.active) {
        return false;
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

    const bool confirmed = ClaimConfirmationReporter::Confirm(pending_tbot_claim_,
        pending_tbot_claim_api_url_, pending_tbot_claim_token_);
    if (!confirmed) {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        pending_tbot_claim_token_.clear();
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTION_CONFIRM_FAILED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    CancelClaimExpiryTimer();
    StopClaimPoll();
    // Claim confirmed -> the device is becoming claimed. Stop advertising for
    // pairing; an owned robot must not be BLE-discoverable for a new claim.
    StopBleAdvertising();
    if (protocol_) {
        CloseAudioChannelByIntent();
    }
    // The bootstrap token is single-attempt claim auth. Device credentials are
    // already persisted by the reporter; clear the consumed token so a reboot
    // never polls /device/config with stale Authorization before WS comes up.
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
    }
    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    pending_tbot_claim_token_.clear();
    claim_substate_ = TbotClaimSubstate::Confirmed;

    // TBOT claim complete -> return to explicit wake standby. Do not warm the
    // realtime WebSocket here: opening the channel can route through the normal
    // connect path and leave the device in Listening, where this board disables
    // wake-word detection. The first "Hi ESP" opens the channel on the existing
    // wake worker path instead.
    if (IsDeviceClaimed()) {
        SetDeviceState(kDeviceStateIdle);
        audio_service_.EnableWakeWordDetection(true);
        // On first claim, the realtime session may already have connected while
        // unclaimed, before backend credentials existed. Start and fire one
        // heartbeat now so the claimed online device reports immediately.
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    }

    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTED, "link", Lang::Sounds::OGG_SUCCESS);
    return true;
}

void Application::SchedulePendingTbotClaimRefresh() {
    Schedule([this]() {
        // BluFi-provisioned reconnect: drive the FSM out of kDeviceStateWifiConfiguring
        // (Activating -> ActivationTask -> Idle) FIRST, otherwise RefreshPendingTbotClaim
        // would be swallowed by its own WifiConfiguring early-return and the pending
        // claim would never auto-confirm without an extra manual power-cycle.
        PromoteFromWifiConfigAfterProvisioning();
        if (GetDeviceState() != kDeviceStateWifiConfiguring) {
            // Already out of setup (promoted-and-no-async-activation, or never in
            // setup) -> run the claim refresh now.
            RefreshPendingTbotClaim();
        }
        // else: still in setup (stale success report / not actually connected) ->
        // either we left it in WifiConfiguring on purpose, or activation is running
        // and HandleActivationDoneEvent() will call RefreshPendingTbotClaim() from Idle.
    });
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
        // advertising for claim standby, the 8KB activation task often cannot
        // be created. Signal done so we reach Idle and keep BLE discoverable.
        ESP_LOGW(TAG,
                 "Unclaimed after BluFi Wi-Fi success: skip activation worker, "
                 "finish setup to Idle claim-standby");
        if (!ota_) {
            ota_ = std::make_unique<Ota>();
            ota_->MarkCurrentVersionValid();
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
        return;
    }
    if (activation_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Activation task already running");
        return;  // Activation already running -> it will reach Idle on its own.
    }
    // Reuse the same activation worker as a normal boot (OTA check + protocol/WS),
    // which ends by setting MAIN_EVENT_ACTIVATION_DONE -> HandleActivationDoneEvent
    // -> kDeviceStateIdle -> RefreshPendingTbotClaim() (the proven auto-confirm path).
    BaseType_t created = xTaskCreate([](void* arg) {
        Application* app = static_cast<Application*>(arg);
        app->ActivationTask();
        app->activation_task_handle_ = nullptr;
        vTaskDelete(NULL);
    }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create activation task after provisioning");
        activation_task_handle_ = nullptr;
        if (!ota_) {
            ota_ = std::make_unique<Ota>();
            ota_->MarkCurrentVersionValid();
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
    }
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
};
}  // namespace

void Application::DispatchPendingTbotClaimFetch(const std::string& api_url,
                                                const std::string& token) {
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
        return;
    }

    // Single-flight: a slow backend must never let the 10s timer stack workers.
    bool expected = false;
    if (!claim_poll_inflight_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Claim fetch already in flight; skipping this tick");
        return;
    }

    auto* ctx = new ClaimFetchContext{this, api_url, token};
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
        delete ctx;
        claim_poll_inflight_.store(false);
    }
}

void Application::ClaimFetchTask(void* arg) {
    auto* ctx = static_cast<ClaimFetchContext*>(arg);
    Application* self = ctx->app;
    std::string api_url = ctx->api_url;
    std::string token = ctx->token;
    delete ctx;

    // The ONLY work on this worker: the blocking ~3s HTTP/TLS fetch. No shared
    // state is touched here.
    PendingTbotClaim pending_claim;
    int device_config_status = 0;
    const bool fetched = FetchPendingTbotClaimFromDeviceConfig(api_url, token, pending_claim,
                                                               &device_config_status);

    // Marshal result-application back onto the Application task (OQ1): all
    // claim_substate_/pending_tbot_claim_*/BLE/SetDeviceState mutation stays on
    // the one task that owns them. Clear the single-flight guard there so the
    // next tick can dispatch again.
    self->Schedule([self, api_url, token, pending_claim, fetched, device_config_status]() {
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
        if (!self->claim_poll_active_ && token.empty()) {
            return;
        }
        self->ApplyPendingTbotClaimFetchResult(api_url, token, pending_claim,
                                               fetched, device_config_status);
    });

    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Bounded claim poll (C4)
// ---------------------------------------------------------------------------

void Application::StartClaimPoll() {
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
        ESP_LOGI(TAG, "Claim poll re-armed (every %llus)",
                 claim_poll_interval_us_ / 1000000ULL);
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
    ESP_LOGI(TAG, "Claim poll started (every %llus, %llds cap)",
             claim_poll_interval_us_ / 1000000ULL, kClaimPollWindowMs / 1000);
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
    ESP_LOGI(TAG, "Claim expiry armed in %llds", remaining_s);
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
    pending_tbot_claim_ = PendingTbotClaim{};
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

bool Application::IsDeviceClaimed() const {
    // Primary claimed-signal: a DEDICATED flag written ONLY by a successful
    // physical-claim confirm (PersistTbotClaimConfirmationResponse). Recovery
    // signal: backend device_id + device_secret are also written only by that
    // successful confirm, so after a reboot/marker loss they are enough to keep
    // the robot on the claimed online path. We must NOT key off websocket
    // "token": OTA CheckVersion can write that realtime-WS token on every boot.
    Settings claim_state("tbot_claim", false);
    if (claim_state.GetInt("confirmed", 0) != 0) {
        return true;
    }

    Settings websocket_settings("websocket", false);
    const std::string websocket_token = websocket_settings.GetString("token");
    const bool factory_test_claimed = claim_state.GetInt("factory_test", 0) != 0;
    if (factory_test_claimed && !websocket_token.empty()) {
        return true;
    }

    Settings backend_settings("backend", false);
    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
    return !device_id.empty() && !device_secret.empty();
}

void Application::EnsureBleAdvertisingForStandby() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();

    if (IsDeviceClaimed()) {
        StopBleAdvertising();
        return;
    }

    if (blufi.GetBleState() == Blufi::BleState::kOff) {
        // Not advertising yet -> bring BLE up. Guarded by kOff so we never call
        // init() twice without a deinit() in between (double-init would leak the
        // BT controller/host). init() resets the re-advertise cap for this fresh
        // discoverable window. init() does NOT disturb the connected station.
        ESP_LOGI(TAG, "Claim standby: starting BLE advertising (TBOT-<MAC>)");
        blufi.init();
    }

    // Re-arm the BLE hard-timeout on every standby poll. The poll cadence
    // (kClaimPollIntervalUs, ~10s) is far shorter than CONFIG_BLE_SETUP_TIMEOUT
    // _SEC (300s), so the one-shot timer is continually pushed forward and never
    // tears BLE down WHILE we remain unclaimed in standby — the robot stays
    // discoverable. The hard-timeout still fires (and tears BLE down) if we ever
    // stop re-arming, i.e. the moment we leave standby. The §9 re-advertise cap
    // (kMaxBleReadvertiseAttempts) is untouched and still bounds a flapping peer.
    blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);
#endif
}

void Application::EnsureBleAdvertisingForUnclaimedSavedWifi() {
    if (IsDeviceClaimed()) {
        return;
    }

    ESP_LOGI(TAG, "Stored WiFi exists but device is unclaimed; keeping BLE advertising open for setup");
    EnsureBleAdvertisingForStandby();
}

void Application::StopBleAdvertising() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();
    // Cancel the hard-timeout first so a stale timer callback cannot post a
    // redundant teardown after deinit() (mirrors WifiBoard::OnNetworkEvent).
    blufi.CancelBleSetupTimeout();
    if (blufi.GetBleState() != Blufi::BleState::kOff) {
        ESP_LOGI(TAG, "Leaving claimable standby: stopping BLE advertising");
        blufi.deinit();
    }
#endif
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
    if (heartbeat_active_) {
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
    ESP_LOGI(TAG, "Heartbeat started (every %llus)", kHeartbeatIntervalUs / 1000000ULL);
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
    ESP_LOGW(TAG, "Heartbeat auth failed (HTTP %d); clearing stale claim credentials", status_code);
    StopHeartbeat();
    CloseAudioChannelByIntent();

    {
        Settings backend_settings("backend", true);
        backend_settings.SetString("device_secret", "");
    }
    {
        Settings claim_state("tbot_claim", true);
        claim_state.SetInt("confirmed", 0);
    }
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
    }

    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    pending_tbot_claim_token_.clear();
    claim_substate_ = TbotClaimSubstate::AvailableStandby;
    backend_offline_.store(false);
    EnsureBleAdvertisingForStandby();
    StartClaimPoll();
    RenderClaimSubstate(claim_substate_);
}

void Application::EnterRepairPairingMode() {
    // Callable from the BOOT button task; marshal ALL claim-FSM + NVS mutation onto
    // the Application task (OQ1: the claim state machine is single-threaded).
    Schedule([this]() {
        if (lesson_runtime_active_.load()) {
            ESP_LOGW(TAG, "lesson re-pair ignored during lesson");
            return;
        }
        ESP_LOGW(TAG, "BOOT re-pair: forgetting current claim so a new parent phone can connect");
        StopHeartbeat();
        CloseAudioChannelByIntent();

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
        bool released = false;
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
                // Backend row is freed; the now-orphaned secret would only 401 a
                // retry, so drop it and stop deferring.
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
        }
        {
            Settings websocket_settings("websocket", true);
            websocket_settings.SetString("bootstrap_token", "");
            websocket_settings.SetString("url", "");
        }

        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        pending_tbot_claim_token_.clear();
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
        SsidManager::GetInstance().Clear();
        ESP_LOGW(TAG, "BOOT re-pair: Wi-Fi forgotten; rebooting into Wi-Fi setup for a new network");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    });
}

void Application::MaybeDispatchDeferredCloudRelease() {
    Settings backend_settings("backend", false);
    if (backend_settings.GetInt("release_pending", 0) == 0) {
        return;  // No pending BOOT re-pair release.
    }
    const std::string api_url = backend_settings.GetString("api_url");
    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
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
    if (xTaskCreateWithCaps(&Application::CloudReleaseTask, "cloud_release", 6144, this,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "cloud_release task create failed; retrying next refresh");
        cloud_release_inflight_.store(false);
    }
}

void Application::CloudReleaseTask(void* arg) {
    Application* self = static_cast<Application*>(arg);

    // ONLY work on this worker: the blocking ~5s factory-reset POST. It reads NVS
    // (backend.*) internally and touches no shared Application state.
    const bool released = SystemReset::ReleaseCloudOwnership();

    // Marshal the result back onto the Application task (OQ1) and clear the
    // single-flight guard there.
    self->Schedule([self, released]() {
        self->cloud_release_inflight_.store(false);
        if (!released) {
            ESP_LOGW(TAG, "Deferred cloud ownership release failed; will retry on next refresh");
            return;
        }
        Settings backend_settings("backend", true);
        backend_settings.SetInt("release_pending", 0);
        backend_settings.SetString("device_secret", "");
        ESP_LOGI(TAG, "Deferred cloud ownership released; robot is free for a new parent to claim");
    });

    vTaskDelete(nullptr);
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
    // Low priority (tskIDLE_PRIORITY+1) and NOT pinned to core 0 so the worker
    // simply WAITS on the network at low priority while the wake-word AFE
    // fetch/feed pipeline keeps the CPU (same pattern as ClaimFetchTask).
    // Internal DRAM stack: TLS/HTTP + any Settings/NVS from this path need a
    // cache-safe stack (SPIRAM stacks panic when flash cache is disabled).
    if (xTaskCreateWithCaps(&Application::HeartbeatTask, "heartbeat_http", 8192, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "heartbeat task create failed; retrying next tick");
        delete ctx;
        heartbeat_inflight_.store(false);
    }
}

void Application::HeartbeatTask(void* arg) {
    auto* ctx = static_cast<HeartbeatContext*>(arg);
    Application* self = ctx->app;
    const std::string url = ctx->url;
    const std::string device_secret = ctx->device_secret;
    std::string body = std::move(ctx->body);
    delete ctx;

    // The ONLY work on this worker: the blocking ~5s HTTP/TLS POST.
    int status_code = self->SendDeviceHeartbeat(url, device_secret, std::move(body));

    // Clear the single-flight guard and marshal any auth-failure handling back
    // onto the Application task (OQ1): HandleHeartbeatAuthFailure mutates claim /
    // BLE / NVS / audio-channel state, which must stay on the owning task.
    self->Schedule([self, status_code]() {
        self->heartbeat_inflight_.store(false);
        if (status_code == 401 || status_code == 403) {
            self->HandleHeartbeatAuthFailure(status_code);
        }
    });

    vTaskDelete(nullptr);
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

    // Unclaimed + saved Wi-Fi keeps BLE advertising open for phone setup.
    // Running OTA HTTPS (CheckNewVersion) at the same time exhausts internal
    // heap (TLS + BluFi) and freezes the UI on "Loading setup..." with no
    // further logs. Unclaimed robots cannot finish cloud activation without a
    // parent claim, so skip network bootstrap and exit to Idle claim-standby.
    if (!IsDeviceClaimed()) {
        ESP_LOGW(TAG,
                 "Unclaimed device: skip OTA/bootstrap HTTPS while BLE stays up "
                 "(avoids Loading-setup hang; claim path remains via BLE)");
        CheckAssetsVersion();
        SystemInfo::PrintHeapCheckpoint("activation.complete");
        xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
        return;
    }

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

    // Initialize the protocol
    SystemInfo::StartHeapPhaseMonitor();
    InitializeProtocol();
    SystemInfo::PrintHeapCheckpoint("protocol_init.complete");
    SystemInfo::StopHeapPhaseMonitor();

    // Signal completion to main loop
    SystemInfo::PrintHeapCheckpoint("activation.complete");
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
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

    // Cold-boot first-wake latency fix: prewarm the AFE wake-word pipeline here,
    // on the prio-2 activation task, so the expensive one-time AFE create + the
    // audio_detection fetch-task spawn overlap the OTA/protocol network waits that
    // still run before Idle. Without this the AFE is built lazily on the FIRST
    // EnableWakeWordDetection(true) at the prio-10 Idle transition, and the very
    // first "Hi ESP" spoken right at Idle races AFE init and is dropped (1-2
    // tries). Strictly gated on IsDeviceClaimed() so an UNCLAIMED robot never
    // builds/runs the mic (BLE+AFE-FEED contention gate). Prewarm does NOT Start()
    // the mic — the locked Idle gate still owns enabling it — so the FEED ring
    // stays empty until then and OQ1 / the contention fix are untouched.
    if (IsDeviceClaimed()) {
        SystemInfo::StartHeapPhaseMonitor();
        audio_service_.PrewarmWakeWord();
        SystemInfo::PrintHeapCheckpoint("afe_prewarm.complete");
        SystemInfo::StopHeapPhaseMonitor();
    }

    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        auto current_state = GetDeviceState();
        if (current_state == kDeviceStateWifiConfiguring ||
            current_state == kDeviceStateAudioTesting) {
            ESP_LOGI(TAG, "Skipping OTA version check because WiFi config mode is active");
            return;
        }
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                char error_message[128];
                snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
                char buffer[256];
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
                Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d), code=%d, url=%s",
                     retry_delay, retry_count, MAX_RETRY, err, ota_->GetCheckVersionUrl().c_str());
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                auto delayed_state = GetDeviceState();
                if (delayed_state == kDeviceStateWifiConfiguring ||
                    delayed_state == kDeviceStateAudioTesting) {
                    ESP_LOGI(TAG, "Aborting OTA retry because WiFi config mode is active");
                    return;
                }
                if (delayed_state == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

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

void Application::InitializeProtocol() {
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
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
        is_websocket_protocol = true;
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    if (is_websocket_protocol && lesson_message_queue_ == nullptr &&
        lesson_message_task_handle_ == nullptr) {
        lesson_message_queue_ = xQueueCreate(kLessonMessageQueueDepth, sizeof(char*));
        if (lesson_message_queue_ == nullptr) {
            ESP_LOGE(TAG, "lesson_worker queue create failed");
        } else if (xTaskCreateWithCaps(&Application::LessonMessageTask, "lesson_worker",
                                       kLessonMessageWorkerStackBytes, this,
                                       tskIDLE_PRIORITY + 2, &lesson_message_task_handle_,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "lesson_worker task create failed");
            vQueueDelete(lesson_message_queue_);
            lesson_message_queue_ = nullptr;
            lesson_message_task_handle_ = nullptr;
        }
    }

    protocol_->OnConnected([this]() {
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

    protocol_->OnNetworkError([this](const std::string& message) {
        backend_offline_.store(true);   // -> OFFLINE_RETRY copy via the mapper
        // H2: the session is down -> stop the heartbeat so it cannot keep POSTing
        // (and blocking the main task) against a dead backend. It restarts only
        // from OnConnected/OnAudioChannelOpened once the session is healthy again.
        StopHeartbeat();
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking || tts_audio_accepting_.load()) {
            last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
            // Stamp the active response generation so a frame that slips in just
            // as the response is cancelled is gen-gated out at dequeue.
            packet->generation = speaking_generation_.load();
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        // User-driven listen/wake sessions own reconnect intent. Passive lesson
        // preconnect only makes the device reachable for server lesson pull/nudge;
        // it must not later reconnect into Listening without a wake/button action.
        if (passive_ws_intent_.load()) {
            ESP_LOGI(TAG, "passive_lesson_websocket_opened_without_heartbeat");
            StopHeartbeat();
            if (!lesson_runtime_active_.load()) {
                audio_service_.EnableWakeWordDetection(true);
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
        // Once the realtime WS is up, STOP the blocking claim-config HTTP/TLS poll.
        // It runs on the main task and was starving the Opus codec task (growing
        // encode_drop -> dropped mic uplink -> "I speak but no response"). The
        // device is functional over the WS; claim re-arms if it ever needs to.
        Schedule([this]() { StopClaimPoll(); });
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        tts_audio_accepting_.store(false);
        StopHeartbeat();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
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
                ESP_LOGW(TAG, "lesson passive ws dropped -> passive reconnect");
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
                StartPassiveLessonWebsocket();
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
                ESP_LOGW(TAG, "passive_lesson_ws_dropped_unexpected -> passive reconnect");
                StartPassiveLessonWebsocket();
                return;
            }
            // Sustained operation: an UNEXPECTED drop (server/tunnel closed the WS,
            // NOT a user/system close) leaves online_intent_ true -> auto-reconnect
            // with backoff so a 20-60 min conversation is not permanently cut off.
            if (online_intent_.load()) {
                if (lesson_runtime_active_.load()) {
                    ESP_LOGW(TAG, "lesson ws dropped unexpected -> suppress generic reconnect");
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
            }
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
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
        if (strcmp(type->valuestring, "tts") == 0) {
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
                tts_audio_accepting_.store(true);
                Schedule([this]() {
                    aborted_ = false;
                    auto current_generation = speaking_generation_.load();
                    last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
                    SetDeviceState(kDeviceStateSpeaking);
                    ESP_LOGI(TAG, "tts_start_received generation=%lu", (unsigned long)current_generation);
                    ArmSpeakingTimeout();
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                tts_audio_accepting_.store(false);
                int64_t t_recv = esp_timer_get_time() / 1000;
                ESP_LOGI(TAG, "tts_stop_received ts=%lld", t_recv);
                // Patch 3.4: the backend tags an interrupt-driven stop with
                // reason="interrupt" (barge-in) vs a normal end-of-turn stop.
                auto reason = cJSON_GetObjectItem(root, "reason");
                bool is_interrupt = cJSON_IsString(reason) &&
                                    strcmp(reason->valuestring, "interrupt") == 0;
                auto continue_listening = cJSON_GetObjectItem(root, "continue_listening");
                bool force_continue_listening = cJSON_IsTrue(continue_listening);
                auto listen_mode = cJSON_GetObjectItem(root, "listen_mode");
                bool force_realtime_listen = cJSON_IsString(listen_mode) &&
                                             strcmp(listen_mode->valuestring, "realtime") == 0;
                bool explicit_stop_listening =
                    cJSON_IsBool(continue_listening) && !cJSON_IsTrue(continue_listening) &&
                    cJSON_IsString(listen_mode) &&
                    strcmp(listen_mode->valuestring, "manual") == 0;
                if (is_interrupt) {
                    // Barge-in: cut NOW instead of draining. Bump+publish the
                    // generation so any in-flight frame is gen-gated (Patch 3.3),
                    // then clear the playback/decode queues. Idempotent if the
                    // local VAD path already aborted.
                    audio_service_.SetPlaybackGeneration(++speaking_generation_);
                    audio_service_.ResetDecoder();
                    ESP_LOGI(TAG, "tts_stop_interrupt_flush ts=%lld", t_recv);
                }
                // NOTE: for a NORMAL end-of-turn stop we deliberately do NOT
                // ResetDecoder — that cut the final 200-500ms of every response
                // because the server sends `tts state=stop` immediately after
                // audio_end while the playback queue still holds buffered frames.
                // User reported: "phản hồi không ổn định chưa trả lời hết cầu
                // chuyển sang đang lắng nghe". Normal stops rely on natural queue
                // drain; only the interrupt branch above cuts early.
                Schedule([this, force_continue_listening, force_realtime_listen, explicit_stop_listening]() {
                    ++speaking_generation_;
                    last_speaking_activity_ms_.store(0);
                    const bool lesson_interactive_turn =
                        lesson_interactive_listen_pending_.load() ||
                        lesson_interactive_listening_active_.load();
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
                    if (force_continue_listening && !lesson_interactive_turn) {
                        bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
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
                        ESP_LOGI(TAG, "mic_loop_resumed ts=%lld reason=tts_stop_continue_listening",
                                 esp_timer_get_time() / 1000);
                        return;
                    }
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            if (lesson_interactive_turn) {
                                bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
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
                            if (!playback_drained) {
                                ESP_LOGW(TAG,
                                         "tts_stop_playback_drain_timeout timeout_ms=%lu action=idle",
                                         static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                            }
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                            ESP_LOGI(TAG, "mic_loop_resumed ts=%lld",
                                     esp_timer_get_time() / 1000);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGD(TAG, "<< %s", text->valuestring);  // PRIV-1: transcript content debug-only (COPPA)
                    if (!lesson_runtime_active_.load()) {
                        Schedule([display, message = std::string(text->valuestring)]() {
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
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("user", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                if (!lesson_runtime_active_.load()) {
                    Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                        display->SetEmotion(emotion_str.c_str());
                        HandleEmotionGesture(emotion_str.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
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
                    Schedule([this]() {
                        Reboot();
                    });
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
            if (!HandleRobotActionMessage(root)) {
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
                if (HandleRobotActionMessage(payload)) {
                    return;
                }
                char* payload_str_raw = cJSON_PrintUnformatted(payload);
                std::string payload_str = (payload_str_raw != nullptr) ? std::string(payload_str_raw) : std::string();
                if (payload_str_raw != nullptr) {
                    cJSON_free(payload_str_raw);
                }
                if (!lesson_runtime_active_.load()) {
                    Schedule([this, display, payload_str = std::move(payload_str)]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else if (strncmp(type->valuestring, "lesson_", 7) == 0) {
            // US-006 Slice-01 (S10): additive lesson_* dispatch. Placed immediately
            // ABOVE the unknown-type no-op so un-upgraded firmware keeps dropping
            // lesson_* silently (backward-compat). Queue it so HTTP/TLS image fetch
            // and decode never run on the WebSocket receive callback / lwIP stack.
            EnqueueLessonMessage(root);
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    // WebSocket Start() opens the realtime audio channel. Unclaimed devices keep
    // it closed until wake/button so BLE claim and local wake-word setup own the
    // radio. Claimed devices open a PASSIVE channel so ESP-server connect-time
    // lesson pull and backend lesson nudges have a route without entering
    // Listening. MQTT Start() is a control-channel connect, so it still runs here.
    if (is_websocket_protocol) {
        if (IsDeviceClaimed()) {
            ESP_LOGI(TAG, "Claimed device: opening passive WebSocket for lesson/nudge");
            StartPassiveLessonWebsocket();
        } else {
            ESP_LOGI(TAG, "WebSocket audio starts on wake word or explicit listen");
        }
    } else {
        protocol_->Start();
    }
}

bool Application::HandleRobotActionMessage(const cJSON* root) {
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
        Schedule([this, target_angle]() {
            SendHeadSetAngle(target_angle);
        });
        return true;
    }

    auto schedule_percent_action = [this, root](bool (Application::*method)(int), int default_percent) {
        auto percent = cJSON_GetObjectItem(root, "percent");
        int target_percent = cJSON_IsNumber(percent) ? percent->valueint : default_percent;
        Schedule([this, method, target_percent]() {
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
            Schedule([this, method = handler.handler]() {
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
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmRaise();
}

bool Application::SendRightArmRaise() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmRaise();
}

bool Application::SendLeftArmLower() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmLower();
}

bool Application::SendRightArmLower() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmLower();
}

bool Application::SendBothArmsRaise() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsRaise();
}

bool Application::SendBothArmsLower() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsLower();
}

bool Application::SendLeftArmSetPercent(int percent) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmSetPercent(percent);
}

bool Application::SendRightArmSetPercent(int percent) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmSetPercent(percent);
}

bool Application::SendBothArmsSetPercent(int percent) {
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
        audio_service_.PlaySound(sound);
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
    lesson_runtime_active_.store(active);
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
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
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
    auto state = GetDeviceState();

    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson toggle ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
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
    auto created = xTaskCreateWithCaps(&Application::OpenChannelTask, "lesson_ws", 8192, ctx,
                                       tskIDLE_PRIORITY + 3, nullptr,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        delete ctx;
        connect_in_flight_.store(false);
        passive_ws_intent_.store(false);
        CancelConnectWatchdog();
        ESP_LOGE(TAG, "lesson_ws task create failed");
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
    // up to ~20s). Run it on a short-lived worker so the app task keeps draining
    // audio/VAD/abort/UI. connect_generation_ invalidates a stale result; the
    // connect watchdog (SM-3) recovers a wedged/black-hole connect.
    reconnect_mode_ = mode;
    online_intent_.store(true);  // we want an open channel -> reconnect on unexpected drop
    uint32_t gen = ++connect_generation_;
    connect_in_flight_.store(true);
    connect_attempt_active_.store(true);  // WSS-8: suppress per-attempt error banner until terminal
    ArmConnectWatchdog();
    passive_ws_intent_.store(false);
    auto* ctx = new ConnectContext{this, mode, gen, std::string(), false, false};
    if (xTaskCreateWithCaps(&Application::OpenChannelTask, "ws_open", 8192, ctx,
                            tskIDLE_PRIORITY + 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete ctx;
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);  // WSS-8: no worker -> cycle ended
        CancelConnectWatchdog();
        ESP_LOGE(TAG, "ws_open task create failed -> idle");
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::OpenChannelTask(void* arg) {
    auto* ctx = static_cast<ConnectContext*>(arg);
    Application* self = ctx->app;
    ListeningMode mode = ctx->mode;
    uint32_t gen = ctx->generation;
    std::string wake_word = ctx->wake_word;
    bool wake_word_invoke = ctx->wake_word_invoke;
    bool passive_preconnect = ctx->passive_preconnect;
    delete ctx;

    // The ONLY blocking call, now off the app task.
    bool ok = false;
    int max_attempts = wake_word_invoke ? kWakeWordAudioChannelOpenMaxAttempts : 1;
    for (int attempt = 1;
         self->protocol_ && !self->protocol_->IsAudioChannelOpened() && attempt <= max_attempts;
         ++attempt) {
        ok = self->protocol_->OpenAudioChannel();
        if (ok) {
            break;
        }
        if (wake_word_invoke) {
            ESP_LOGW(TAG, "wake_audio_channel_open_failed attempt=%d max=%d",
                     attempt, kWakeWordAudioChannelOpenMaxAttempts);
        }
        if (wake_word_invoke && attempt < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(kWakeWordAudioChannelRetryDelayMs));
        }
    }
    if (self->protocol_ && self->protocol_->IsAudioChannelOpened()) {
        ok = true;
    }
    self->connect_in_flight_.store(false);  // worker is done using protocol_

    self->Schedule([self, ok, mode, gen, wake_word, wake_word_invoke, passive_preconnect]() {
        self->CancelConnectWatchdog();
        // A ResetProtocol arrived while we were mid-connect and deferred the
        // actual reset to us (now safe: the worker no longer touches protocol_).
        if (self->reset_pending_.exchange(false)) {
            self->DoResetProtocol();
            return;
        }
        if (gen != self->connect_generation_.load()) {
            return;  // superseded by a newer connect or the watchdog
        }
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
            self->connect_attempt_active_.store(false);  // WSS-8: connect cycle resolved (success)
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
                } else if (!self->lesson_runtime_active_.load()) {
                    const std::string deferred_wake_word = self->deferred_wake_word_;
                    self->deferred_wake_word_.clear();
                    if (!deferred_wake_word.empty()) {
                        ESP_LOGI(TAG, "passive_lesson_deferred_wake_resumed");
                        self->FinishWakeWordInvoke(deferred_wake_word);
                    } else {
                        self->audio_service_.EnableWakeWordDetection(true);
                        ESP_LOGI(TAG, "passive_lesson_wake_word_rearmed running=%d",
                                 self->audio_service_.IsWakeWordRunning() ? 1 : 0);
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
            const bool lesson_answer_turn =
                self->lesson_interactive_listen_pending_.load() ||
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
                self->ScheduleReconnect(mode, self->reconnect_resume_listening_.load());   // WSS-4: long-horizon retry
            } else if (wake_word_invoke) {
                // WSS-8: the wake open-loop exhausted all attempts -> terminal.
                // Per-attempt errors were suppressed (connect_attempt_active_),
                // so surface the offline banner exactly once now.
                self->connect_attempt_active_.store(false);
                xEventGroupSetBits(self->event_group_, MAIN_EVENT_ERROR);
            }
        }
    });
    vTaskDelete(nullptr);
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
    // Invalidate the still-running worker's eventual result, recover to Idle and
    // schedule a backoff retry. connect_in_flight_ may still be true (worker
    // blocked); HandleReconnectTick re-defers until it clears.
    ++connect_generation_;
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
        SchedulePassiveLessonReconnect();
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGW(TAG, "lesson connect watchdog timeout -> suppress generic reconnect");
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
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000ULL);
}

void Application::SchedulePassiveLessonReconnect() {
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
    if (protocol_ == nullptr) {
        reconnect_attempt_ = 0;
        passive_reconnect_attempt_ = 0;
        reconnect_passive_.store(false);
        connect_attempt_active_.store(false);
        return;
    }
    if (reconnect_passive_.exchange(false)) {
        if (protocol_->IsAudioChannelOpened()) {
            passive_reconnect_attempt_ = 0;
            return;
        }
        if (connect_in_flight_.load()) {
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
    if (connect_in_flight_.load()) {
        ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());  // previous worker still finishing; retry later
        return;
    }
    ESP_LOGI(TAG, "reconnect_tick attempt=%d", reconnect_attempt_);
    SetDeviceState(kDeviceStateConnecting);
    ContinueOpenAudioChannel(reconnect_mode_);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
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
        audio_service_.EnableVoiceProcessing(true);
    }
}

void Application::HandleStopListeningEvent() {
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
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

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
        if (xTaskCreateWithCaps(&Application::OpenChannelTask, "wake_ws_open", 8192, ctx,
                                tskIDLE_PRIORITY + 3, nullptr,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            delete ctx;
            connect_in_flight_.store(false);
            connect_attempt_active_.store(false);  // WSS-8: no worker -> cycle ended
            CancelConnectWatchdog();
            ESP_LOGE(TAG, "wake_ws_open task create failed -> idle");
            audio_service_.EnableWakeWordDetection(true);
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    FinishWakeWordInvoke(wake_word);
}

void Application::FinishWakeWordInvoke(const std::string& wake_word) {
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
    // Send buffered wake audio only after the state event has sent listen/start.
    Schedule([this]() {
        if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
            return;
        }
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
    });
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
        default:
            // No localized key for this state -> use the contract copy directly
            // (e.g. BOOT "Starting", WIFI_CONNECTING, BOOTSTRAP_FETCHING,
            // ERROR_RECOVERABLE "Hold button 5s to retry").
            return spec->screen_text;
    }
}

void Application::HandleListeningWatchdogTick() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Realtime mode intentionally keeps a long-lived audio stream open. The
    // watchdog is for finite AutoStop/manual turns that should not sit in
    // Listening forever after missed VAD/STT/server-stop events.
    if (listening_mode_ == kListeningModeRealtime) {
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t started_ms = listening_started_ms_.load();
    int64_t last_activity_ms = last_listening_activity_ms_.load();
    if (started_ms <= 0 || last_activity_ms <= 0) {
        listening_started_ms_.store(now_ms);
        last_listening_activity_ms_.store(now_ms);
        return;
    }

    const int64_t idle_ms = now_ms - last_activity_ms;
    const int64_t turn_ms = now_ms - started_ms;
    const uint32_t idle_limit_ms = listening_mode_ == kListeningModeAutoStop
        ? kListeningNoSpeechTimeoutMs
        : kListeningMaxTurnMs;
    const uint32_t turn_limit_ms = listening_mode_ == kListeningModeAutoStop
        ? kListeningAutoStopMaxTurnMs
        : kListeningMaxTurnMs;
    if (idle_ms < idle_limit_ms && turn_ms < turn_limit_ms) {
        return;
    }

    uint32_t decode_q = 0, send_q = 0, playback_q = 0;
    audio_service_.GetQueueDepths(decode_q, send_q, playback_q);
    auto audio_stats = audio_service_.GetDebugStatistics();
    ESP_LOGW(TAG,
             "listening_watchdog_timeout mode=%d idle_ms=%lld turn_ms=%lld decode_q=%lu send_q=%lu playback_q=%lu decode_drop=%lu encode_drop=%lu reconnects=%lu",
             static_cast<int>(listening_mode_),
             idle_ms,
             turn_ms,
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

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

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
                audio_service_.EnableWakeWordDetection(false);
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
            if (IsDeviceClaimed() && !connect_in_flight_.load()) {
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

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
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
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
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
            StopHeartbeat();
            StopClaimPoll();
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
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

    ESP_LOGW(TAG, "speaking_timeout generation=%lu idle_ms=%lld",
             (unsigned long)generation,
             last_activity_ms > 0 ? now_ms - last_activity_ms : -1);
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
        ESP_LOGI(TAG, "mic_loop_resumed ts=%lld reason=speaking_timeout",
                 esp_timer_get_time() / 1000);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
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

void Application::Reboot() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson reboot ignored");
        return;
    }
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        CloseAudioChannelByIntent();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
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
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

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
        audio_service_.Start(); // Restart audio service
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
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson direct wake ignored state=%d", static_cast<int>(state));
        return;
    }
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

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

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
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
    audio_service_.PlaySound(sound);
}

void Application::CloseAudioChannelByIntent() {
    // User/system-initiated close: we no longer want an open channel, so
    // OnAudioChannelClosed must NOT auto-reconnect. Cancel any pending retry.
    deferred_wake_word_.clear();
    passive_ws_intent_.store(false);
    reconnect_passive_.store(false);
    online_intent_.store(false);
    reconnect_attempt_ = 0;
    passive_reconnect_attempt_ = 0;
    connect_attempt_active_.store(false);
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    if (protocol_) {
        protocol_->CloseAudioChannel();
    }
}

void Application::DoResetProtocol() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        CloseAudioChannelByIntent();
    }
    protocol_.reset();
}

void Application::ResetProtocol() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "ResetProtocol ignored during lesson");
        return;
    }
    Schedule([this]() {
        ++connect_generation_;  // invalidate any in-flight connect's result
        if (connect_in_flight_.load()) {
            // A worker is blocked in OpenAudioChannel() using protocol_; resetting
            // now would be a use-after-free. Defer to the worker's completion,
            // which honors reset_pending_ back on the app task.
            reset_pending_.store(true);
            ESP_LOGW(TAG, "ResetProtocol deferred: connect worker in flight");
            return;
        }
        DoResetProtocol();
    });
}
