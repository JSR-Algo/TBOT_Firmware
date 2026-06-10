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
#include "tbot_connect_mapper.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "boards/common/blufi.h"
#endif

#include <ctime>
#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

static constexpr uint32_t kListenPlaybackDrainTimeoutMs = 650;
static constexpr uint32_t kSpeakingTimeoutMs = 12000;
static constexpr int kWakeWordAudioChannelOpenMaxAttempts = 3;
static constexpr uint32_t kWakeWordAudioChannelRetryDelayMs = 700;

// TBOT claim poll (C4): cadence 10s. The backend's 5-minute cap applies only
// after a pending claim exists; unclaimed standby must keep polling so a late
// phone scan can still find and claim the robot.
static constexpr uint64_t kClaimPollIntervalUs = 10ULL * 1000000ULL;      // 10s (was 4s: blocking HTTP/TLS poll was hammering main task + flaky backend)
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
    vEventGroupDelete(event_group_);
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
            last_vad_speech_ms_ = esp_timer_get_time() / 1000;
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
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
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
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
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
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
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
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
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

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
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

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
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
                ESP_LOGI(TAG, "audio_metrics decode_q=%lu send_q=%lu playback_q=%lu decode_drop=%lu encode_drop=%lu stale_frames=%lu interrupts=%lu reconnects=%lu",
                         (unsigned long)decode_q, (unsigned long)send_q, (unsigned long)playback_q,
                         (unsigned long)audio_stats.decode_drop_count,
                         (unsigned long)audio_stats.encode_drop_count,
                         (unsigned long)audio_stats.stale_frame_count,
                         (unsigned long)interrupt_count_.load(),
                         (unsigned long)reconnect_count_.load());
                // MEM-1: main-task stack high-water + PSRAM free (SRAM heap printed above).
                ESP_LOGI(TAG, "sys_metrics stack_main_min=%u psram_free_b=%u",
                         (unsigned)uxTaskGetStackHighWaterMark(nullptr),
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
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
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
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        CloseAudioChannelByIntent();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        ESP_LOGI(TAG, "Activation done ignored because WiFi config mode is active");
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

    // Once the realtime WS is up the device is fully functional. The claim-config
    // backend (onrender) is a SEPARATE, flaky service; each poll is a blocking
    // ~3s HTTP/TLS round-trip on the app task. Re-armed every ~10s it starves the
    // Opus codec (encode/decode drops -> choppy speech, dropped questions),
    // delays the wake-word event ("Hi ESP" needs several tries) and stalls state
    // transitions ("văng lỗi" mid-talk). The giveup-on-failures guard never fires
    // because fetches SUCCEED (device just unclaimed) so failures reset to 0.
    // Stop the blocking claim poll while claimed+online. Keep the normal online
    // audio path stable by also tearing down standby BLE; explicit BOOT
    // Wi-Fi-config mode reopens BluFi for phone reconnect when needed.
    if (online_intent_.load() && IsDeviceClaimed()) {
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
        if (!pending_tbot_claim_.active &&
            (ble_state == Blufi::BleState::kAdvertising ||
             ble_state == Blufi::BleState::kConnected)) {
            ESP_LOGI(TAG, "Skipping claim config fetch while BLE is active; waiting for BluFi token handoff");
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            StartClaimPoll();
            return;
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

    PendingTbotClaim pending_claim;
    const bool fetched = FetchPendingTbotClaimFromDeviceConfig(api_url, token, pending_claim);
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
    // window in practice). ConfirmPendingTbotClaim() runs on this (App) task — the
    // same task that just performed the blocking config fetch — so its blocking
    // POST /claim/confirm is safe here. It persists the claimed flag + websocket
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

    // TBOT BLE+audio contention fix — bring audio up NOW that we are claimed.
    // While UNCLAIMED we deliberately skipped the realtime audio WS preconnect
    // (InitializeProtocol) and kept the AFE/mic input OFF in Idle
    // (HandleStateChangedEvent). A successful confirm has just persisted the
    // claimed flag (Settings("tbot_claim").confirmed = 1, so IsDeviceClaimed()
    // is now true) AND the freshly-claimed websocket URL + backend device
    // secret (PersistTbotClaimConfirmationResponse). BLE advertising was torn down
    // above, so the BLE+audio contention that overflowed the AFE FEED ringbuffer
    // is gone and it is safe to start audio. We do this explicitly here so a
    // just-claimed robot gets working audio WITHOUT requiring a reboot.
    if (IsDeviceClaimed()) {
        // 1) Re-arm the AFE/mic input by enabling wake-word detection (cheap,
        //    non-blocking) so "Hi ESP" works immediately. This mirrors the
        //    claimed Idle path; the device is already in Idle here.
        audio_service_.EnableWakeWordDetection(true);

        // 2) Warm the realtime audio WS after claim persistence so the first
        //    wake word does not pay the cold TLS/WebSocket handshake. For the
        //    WebSocket protocol Protocol::Start() IS the audio preconnect, but it
        //    BLOCKS on the TLS + server-hello handshake (up to ~20s). We are on
        //    the priority-10 main Application task here, so we must NOT call it
        //    inline (it would freeze the event loop and trip the 10s task
        //    watchdog). Run it on a short-lived detached worker, exactly like the
        //    boot preconnect runs inside ActivationTask. Guard on "channel not
        //    already open" so we never disturb an already-live channel (e.g. a
        //    claimed MQTT control channel started at boot). If the preconnect
        //    fails it is non-fatal: the first wake word reopens it on demand
        //    (ContinueWakeWordInvoke retries OpenAudioChannel).
        if (protocol_ && !protocol_->IsAudioChannelOpened()) {
            ESP_LOGI(TAG, "Claim confirmed: warming realtime audio WS (off-task)");
            xTaskCreate([](void* arg) {
                Application* app = static_cast<Application*>(arg);
                if (app->protocol_ && !app->protocol_->IsAudioChannelOpened()) {
                    app->protocol_->Start();
                }
                vTaskDelete(nullptr);
            }, "claim_ws_warm", 4096, this, tskIDLE_PRIORITY + 3, nullptr);
        }
    }

    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTED, "link", Lang::Sounds::OGG_SUCCESS);
    return true;
}

void Application::SchedulePendingTbotClaimRefresh() {
    Schedule([this]() {
        RefreshPendingTbotClaim();
    });
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
// Bounded claim poll (C4)
// ---------------------------------------------------------------------------

void Application::StartClaimPoll() {
    if (online_intent_.load() && IsDeviceClaimed()) {
        return;  // Claimed + online; never re-arm the blocking claim backend poll.
    }
    if (claim_poll_active_) {
        return;  // Already polling this window.
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
    esp_timer_start_periodic(claim_poll_timer_, kClaimPollIntervalUs);
    ESP_LOGI(TAG, "Claim poll started (every %llus, %llds cap)",
             kClaimPollIntervalUs / 1000000ULL, kClaimPollWindowMs / 1000);
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
    // Authoritative claimed-signal: a DEDICATED flag written ONLY by a successful
    // physical-claim confirm (PersistTbotClaimConfirmationResponse). We must NOT
    // key off websocket "token": the OTA CheckVersion response also writes that
    // key (a realtime-WS auth token) on every boot (observed: "Received websocket
    // token: empty=0"), so an unclaimed-but-online robot would falsely look
    // claimed and silently stop advertising/polling for pairing.
    Settings claim_state("tbot_claim", false);
    return claim_state.GetInt("confirmed", 0) != 0;
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

static std::string BuildTbotHeartbeatBody(const std::string& status_json) {
    cJSON* status_root = cJSON_Parse(status_json.c_str());
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "device_id", Board::GetInstance().GetUuid().c_str());
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
                app->Schedule([app]() { app->SendDeviceHeartbeat(); });
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

bool Application::SendDeviceHeartbeat() {
    // H2: gate on a LIVE online DeviceState (Idle/Listening/Speaking), not merely
    // on token presence. A claimed device sitting in WifiConfiguring/Activating/
    // Connecting/Upgrading/Error has no healthy session to report and must not
    // fire heartbeats (which would also block the main task). The timer can still
    // be stopped late, so this is the authoritative runtime gate.
    const DeviceState device_state = GetDeviceState();
    if (device_state != kDeviceStateIdle &&
        device_state != kDeviceStateListening &&
        device_state != kDeviceStateSpeaking) {
        return false;
    }

    // Gate: only claimed/online. Backend API auth uses the backend device
    // secret from claim/confirm; websocket.token is reserved for realtime WS
    // HMAC auth and must not be reused here.
    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    if (api_url.empty()) {
        return false;
    }
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (device_secret.empty()) {
        return false;  // Not claimed yet -> do not heartbeat.
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
    const std::string url = base + "/device/heartbeat";

    const std::string status_json = Board::GetInstance().GetDeviceStatusJson();
    std::string body = BuildTbotHeartbeatBody(status_json);

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for heartbeat");
        return false;
    }
    // B1: a heartbeat must never stall audio. SendDeviceHeartbeat() runs on the
    // priority-10 main Application task, so cap the blocking Open() at 5s instead
    // of the default 30s so a slow/unreachable backend can never freeze playback.
    http->SetTimeout(5000);
    http->SetHeader("X-Device-Token", device_secret);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "Heartbeat HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return false;
    }
    const int status_code = http->GetStatusCode();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Heartbeat failed (HTTP %d)", status_code);
        return false;
    }
    ESP_LOGI(TAG, "Heartbeat accepted (HTTP %d)", status_code);
    return true;
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

    // Rollback is useful when a new image cannot boot, but waiting until the
    // network OTA check completes makes a healthy image vulnerable to rollback
    // during tunnel/backend outages. At this point the app, display, audio, and
    // activation task have started, so mark the running image valid before doing
    // any network-bound version or assets work.
    ota_->MarkCurrentVersionValid();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
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

    protocol_->OnConnected([this]() {
        backend_offline_.store(false);  // healthy session -> ONLINE, not retry
        DismissAlert();
        // Device session is up -> begin periodic heartbeat (C5). The sender is
        // self-gated: it only POSTs once claim backend credentials are in NVS.
        StartHeartbeat();
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
        // Channel is up (including the boot-time preconnect, which bypasses
        // ContinueOpenAudioChannel) -> mark intent so an unexpected drop while
        // idle auto-reconnects instead of stranding the device offline.
        online_intent_.store(true);
        backend_offline_.store(false);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        StartHeartbeat();
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
            display->SetChatMessage("system", "");
            if (GetDeviceState() == kDeviceStateWifiConfiguring ||
                GetDeviceState() == kDeviceStateAudioTesting) {
                // The close was caused by explicit setup entry. Keep the setup
                // state/screen; do not fall back to ONLINE/Idle or schedule a
                // websocket reconnect while Wi-Fi provisioning owns the radio.
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            // WSS-7: drop stale mic backlog so a future session does not replay
            // seconds-old uplink audio after the channel reopens.
            while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
            // Sustained operation: an UNEXPECTED drop (server/tunnel closed the WS,
            // NOT a user/system close) leaves online_intent_ true -> auto-reconnect
            // with backoff so a 20-60 min conversation is not permanently cut off.
            if (online_intent_.load()) {
                ESP_LOGW(TAG, "ws_dropped_unexpected -> auto-reconnect (online_intent)");
                ScheduleReconnect(GetDefaultListeningMode());
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
            if (strcmp(state->valuestring, "start") == 0) {
                audio_service_.ResetDecoder();
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
                Schedule([this]() {
                    ++speaking_generation_;
                    last_speaking_activity_ms_.store(0);
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
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
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGD(TAG, ">> %s", text->valuestring);  // PRIV-1: transcript content debug-only (COPPA)
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                    HandleEmotionGesture(emotion_str.c_str());
                });
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
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
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
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                if (HandleRobotActionMessage(payload)) {
                    return;
                }
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else if (strncmp(type->valuestring, "lesson_", 7) == 0) {
            // US-006 Slice-01 (S10): additive lesson_* dispatch. Placed immediately
            // ABOVE the unknown-type no-op so un-upgraded firmware keeps dropping
            // lesson_* silently (backward-compat). Do NOT move the no-op below it.
            HandleLessonMessage(root);
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    // TBOT BLE+audio contention fix: while the device is UNCLAIMED it has no
    // lessons and needs no audio. On real hardware, running the realtime audio
    // WS preconnect (Wi-Fi audio traffic) at the same time as BLE advertising
    // (claimable standby) + the AFE mic pipeline overloads the chip: the AFE
    // FEED ringbuffer starves and floods the log with "Ringbuffer of AFE(FEED)
    // is full", then the robot errors. So for the realtime WebSocket protocol we
    // SKIP the boot-time audio preconnect until the device is claimed. This
    // removes the Wi-Fi audio traffic that fights BLE during pairing. The
    // protocol object itself is fully constructed (callbacks wired), so
    // IsAudioChannelOpened()/SendAudio()/etc. stay valid; we just do not open the
    // channel yet. A fresh claim confirm brings audio up explicitly (see
    // ConfirmPendingTbotClaim). MQTT (control channel) is never gated here.
    if (is_websocket_protocol && !IsDeviceClaimed()) {
        ESP_LOGI(TAG, "Unclaimed: skipping realtime audio WS preconnect (BLE+audio "
                      "contention); audio starts on claim confirm");
    } else {
        protocol_->Start();
    }
}

bool Application::HandleRobotActionMessage(const cJSON* root) {
    auto action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) {
        return false;
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
    return robot_uart_.SendLeftArmRaise();
}

bool Application::SendRightArmRaise() {
    return robot_uart_.SendRightArmRaise();
}

bool Application::SendLeftArmLower() {
    return robot_uart_.SendLeftArmLower();
}

bool Application::SendRightArmLower() {
    return robot_uart_.SendRightArmLower();
}

bool Application::SendBothArmsRaise() {
    return robot_uart_.SendBothArmsRaise();
}

bool Application::SendBothArmsLower() {
    return robot_uart_.SendBothArmsLower();
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
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
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

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
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
};
}  // namespace

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
    ArmConnectWatchdog();
    auto* ctx = new ConnectContext{this, mode, gen};
    if (xTaskCreate(&Application::OpenChannelTask, "ws_open", 4096, ctx,
                    tskIDLE_PRIORITY + 3, nullptr) != pdPASS) {
        delete ctx;
        connect_in_flight_.store(false);
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
    delete ctx;

    // The ONLY blocking call, now off the app task.
    bool ok = self->protocol_ && self->protocol_->OpenAudioChannel();
    self->connect_in_flight_.store(false);  // worker is done using protocol_

    self->Schedule([self, ok, mode, gen]() {
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
        if (self->GetDeviceState() != kDeviceStateConnecting) {
            return;
        }
        if (ok) {
            self->backend_offline_.store(false);
            self->reconnect_attempt_ = 0;
            self->SetListeningMode(mode);
        } else {
            ESP_LOGW(TAG, "open_audio_channel_failed -> idle + backoff");
            self->backend_offline_.store(true);
            self->SetDeviceState(kDeviceStateIdle);
            self->ScheduleReconnect(mode);   // WSS-4: bounded retry
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
    esp_timer_start_once(connect_watchdog_timer_, 12000000);  // 12s > sum of dep waits
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
    if (GetDeviceState() == kDeviceStateConnecting) {
        ESP_LOGW(TAG, "connect_watchdog_timeout -> idle + backoff");
        backend_offline_.store(true);
        SetDeviceState(kDeviceStateIdle);
        ScheduleReconnect(reconnect_mode_);
    }
}

void Application::ScheduleReconnect(ListeningMode mode) {
    static constexpr int kMaxReconnectAttempts = 6;
    if (reconnect_attempt_ >= kMaxReconnectAttempts) {
        ESP_LOGW(TAG, "reconnect_giveup after %d attempts (stay idle)", reconnect_attempt_);
        reconnect_attempt_ = 0;
        return;
    }
    reconnect_mode_ = mode;
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
    // Exponential backoff 0.5s -> 8s cap, plus 0..50% jitter (anti fleet-sync).
    uint32_t base_ms = 500u << reconnect_attempt_;
    if (base_ms > 8000u) {
        base_ms = 8000u;
    }
    uint32_t jitter_ms = esp_random() % (base_ms / 2 + 1);
    uint32_t delay_ms = base_ms + jitter_ms;
    reconnect_attempt_++;
    reconnect_count_.fetch_add(1, std::memory_order_relaxed);  // OBS-2
    ESP_LOGW(TAG, "reconnect_scheduled attempt=%d delay_ms=%lu", reconnect_attempt_, (unsigned long)delay_ms);
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000ULL);
}

void Application::HandleReconnectTick() {
    if (protocol_ == nullptr) {
        reconnect_attempt_ = 0;
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        reconnect_attempt_ = 0;  // user moved on; abandon the retry chain
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        reconnect_attempt_ = 0;
        return;
    }
    if (connect_in_flight_.load()) {
        ScheduleReconnect(reconnect_mode_);  // previous worker still finishing; retry later
        return;
    }
    ESP_LOGI(TAG, "reconnect_tick attempt=%d", reconnect_attempt_);
    SetDeviceState(kDeviceStateConnecting);
    ContinueOpenAudioChannel(reconnect_mode_);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
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
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
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
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
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

    for (int attempt = 1;
         !protocol_->IsAudioChannelOpened() && attempt <= kWakeWordAudioChannelOpenMaxAttempts;
         ++attempt) {
        if (protocol_->OpenAudioChannel()) {
            break;
        }
        ESP_LOGW(TAG, "wake_audio_channel_open_failed attempt=%d max=%d",
                 attempt, kWakeWordAudioChannelOpenMaxAttempts);
        if (attempt < kWakeWordAudioChannelOpenMaxAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kWakeWordAudioChannelRetryDelayMs));
        }
    }

    if (!protocol_->IsAudioChannelOpened()) {
        audio_service_.EnableWakeWordDetection(true);
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
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
        case kDeviceStateIdle:
            // ONLINE (or OFFLINE_RETRY / a claim overlay) per the mapper.
            display->SetStatus(connect_copy);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
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
            if (IsDeviceClaimed()) {
                audio_service_.EnableWakeWordDetection(true);
            } else {
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateConnecting:
            // BACKEND_CONNECTING per the mapper ("Connecting...").
            display->SetStatus(connect_copy);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

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
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
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
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

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

void Application::SpeakingTimeoutTask(void* arg) {
    auto current_generation = *static_cast<uint32_t*>(arg);
    delete static_cast<uint32_t*>(arg);

    vTaskDelay(pdMS_TO_TICKS(kSpeakingTimeoutMs));
    Application::GetInstance().Schedule([current_generation]() {
        Application::GetInstance().HandleSpeakingTimeout(current_generation);
    });
    vTaskDelete(nullptr);
}

void Application::ArmSpeakingTimeout() {
    auto current_generation = speaking_generation_.load();
    auto task_arg = new uint32_t(current_generation);
    auto created = xTaskCreate(
        &Application::SpeakingTimeoutTask,
        "speaking_timeout",
        4096,
        task_arg,
        tskIDLE_PRIORITY + 1,
        nullptr);
    if (created != pdPASS) {
        delete task_arg;
        ESP_LOGW(TAG, "speaking_timeout_task_failed generation=%lu", (unsigned long)current_generation);
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
    if (listening_mode_ == kListeningModeManualStop) {
        SetDeviceState(kDeviceStateIdle);
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
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
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
    aec_mode_ = mode;
    Schedule([this]() {
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
    online_intent_.store(false);
    reconnect_attempt_ = 0;
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
