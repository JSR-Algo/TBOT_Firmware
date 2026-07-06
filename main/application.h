#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <cstdint>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "robot_uart.h"
#include "claim_confirmation_reporter.h"
#include "tbot_connect_mapper.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();
    uint32_t BeginLessonInteractiveListeningRequest();
    void PrepareLessonInteractiveListening();
    void PrepareLessonInteractiveListening(uint32_t generation);
    void CancelLessonInteractiveListening();
    void SetLessonRuntimeActive(bool active);
    bool IsLessonRuntimeActive() const;
    void BeginLessonNetworkRenderQuiet();
    void EndLessonNetworkRenderQuiet();
    bool IsLessonNetworkRenderQuiet() const;

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    bool SendLeftArmRaise();
    bool SendRightArmRaise();
    bool SendLeftArmLower();
    bool SendRightArmLower();
    bool SendBothArmsRaise();
    bool SendBothArmsLower();
    bool SendLeftArmSetPercent(int percent);
    bool SendRightArmSetPercent(int percent);
    bool SendBothArmsSetPercent(int percent);
    bool SendHeadTurnLeft();
    bool SendHeadTurnRight();
    bool SendHeadCenter();
    bool SendHeadSetAngle(int angle);
    bool SendHeadSetPercent(int percent);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();
    void SchedulePendingTbotClaimRefresh();
    void EnsureBleAdvertisingForUnclaimedSavedWifi();
    // True once the device has been claimed by PersistTbotClaimConfirmationResponse.
    // Claimed robots suppress claim polling and keep normal online BLE off;
    // explicit BOOT Wi-Fi-config mode reopens BluFi for owner reconnect/setup.
    bool IsDeviceClaimed() const;

    // BOOT long-press "re-pair": forget the current claim/ownership and re-enter
    // BLE pairing standby so a (possibly different) parent phone can connect and
    // re-claim the robot. Thread-safe (marshals all work onto the Application task).
    // Does NOT block on Wi-Fi/cloud at press time — cloud ownership release is
    // deferred via backend.release_pending (see MaybeDispatchDeferredCloudRelease).
    void EnterRepairPairingMode();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::atomic<bool> lesson_runtime_active_{false};
    std::atomic<uint32_t> lesson_interactive_listen_generation_{0};
    std::atomic<bool> lesson_interactive_listen_pending_{false};
    std::atomic<bool> lesson_interactive_listening_active_{false};
    std::atomic<bool> lesson_idle_repaint_suppressed_{false};
    std::atomic<int> lesson_network_render_quiet_{0};
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    RobotUart robot_uart_;
    std::unique_ptr<Ota> ota_;
    // OQ1: main-task-only. pending_tbot_claim_ / claim_substate_ (and the
    // pending_tbot_claim_*_ companions below) are read and written ONLY from the
    // Application task — timer callbacks never touch them directly, they post
    // work back via Schedule(). That serialization is the lock; do not access
    // these from another task without re-routing through Schedule().
    PendingTbotClaim pending_tbot_claim_;
    std::string pending_tbot_claim_api_url_;
    std::string pending_tbot_claim_token_;

    // TBOT claim runtime FSM sub-state (drives the connect-state mapper).
    // main-task-only (see OQ1 note above).
    TbotClaimSubstate claim_substate_ = TbotClaimSubstate::None;

    // Claim poll (C4): re-fetch /device/config every few seconds while the robot
    // is unclaimed. The backend's 5-minute cap applies only after a pending
    // claim exists; standby keeps polling so late phone scans still work. Posts
    // work to the Application task via Schedule().
    esp_timer_handle_t claim_poll_timer_ = nullptr;       // periodic, ~4s
    int64_t claim_poll_started_ms_ = 0;                   // monotonic window start
    bool claim_poll_active_ = false;
    // Interval the periodic claim poll timer is currently armed at. StartClaimPoll
    // picks the 10s confirm cadence while offline/confirming and a backed-off 60s
    // cadence once the realtime WS is up, so a residual poll can never materially
    // starve live audio. Initialized in StartClaimPoll (the .cc owns the actual
    // kClaimPollInterval*Us values). main-task-only (see OQ1 note above).
    uint64_t claim_poll_interval_us_ = 0;
    // "Hi ESP needs many tries" fix: single-flight guard for the off-task claim
    // fetch worker. The 10s timer must never stack overlapping ClaimFetchTask
    // workers when the flaky backend is slow (leaked tasks/heap). Set true when a
    // worker is spawned, cleared when its continuation runs (or the spawn fails).
    std::atomic<bool> claim_poll_inflight_{false};
    // Single-flight guard for the deferred BOOT re-pair cloud-ownership release
    // worker (CloudReleaseTask), mirroring claim_poll_inflight_.
    std::atomic<bool> cloud_release_inflight_{false};
    // L2: consecutive /device/config fetch failures. After a couple of misses the
    // standby copy becomes "Server unavailable. Retrying..." instead of the
    // misleading "Ready to connect"; reset to 0 on any successful fetch.
    int claim_fetch_failures_ = 0;

    // Local claim-expiry timer (C4): fires when the parsed expires_at elapses
    // (or the poll window cap is reached) -> CLAIM_CONFIRM_TIMEOUT / "Setup
    // expired". Separate one-shot so the deadline is enforced even between polls.
    esp_timer_handle_t claim_expiry_timer_ = nullptr;     // one-shot

    // Heartbeat (C5): periodic POST /v1/device/heartbeat while claimed/online,
    // carrying backend DTO fields plus ble_state/ap_state/temp from board status.
    esp_timer_handle_t heartbeat_timer_ = nullptr;        // periodic, ~20s
    bool heartbeat_active_ = false;
    std::atomic<int> deferred_heartbeat_auth_failure_status_{0};
    // "Hi ESP needs many tries" fix: single-flight guard for the off-task
    // heartbeat worker, same pattern as claim_poll_inflight_. The 20s timer must
    // never stack overlapping HeartbeatTask workers when the backend is slow.
    std::atomic<bool> heartbeat_inflight_{false};

    // T1/SM-1: connect runs on a short-lived worker so the long blocking
    // OpenAudioChannel() (TCP+TLS handshake + server hello, up to ~20s) never
    // freezes the app task. connect_generation_ invalidates a stale result;
    // connect_in_flight_ gates double-workers and defers ResetProtocol;
    // reset_pending_ honors a reset that arrived mid-connect.
    std::atomic<uint32_t> connect_generation_{0};
    std::atomic<bool> connect_in_flight_{false};
    std::atomic<bool> reset_pending_{false};
    // WSS-8: true from the start of a wake/listen/reconnect connect cycle until it
    // succeeds or the user/system cancels the online intent. While true, a per-attempt SetError is a
    // RECOVERABLE transient (the wake loop / ScheduleReconnect backoff retries),
    // so MAIN_EVENT_ERROR keeps the calm "connecting/idle" view instead of flashing
    // "Server unavailable. Retrying...". Wake-open exhaustion still surfaces a
    // terminal error; listen-mode reconnect stays in long-horizon auto-reconnect.
    // Cleared on OnAudioChannelOpened (success).
    std::atomic<bool> connect_attempt_active_{false};
    esp_timer_handle_t connect_watchdog_timer_ = nullptr;   // one-shot (SM-3)
    // T2/WSS-4: long-horizon auto-reconnect with fast backoff, then slow periodic retry.
    esp_timer_handle_t reconnect_timer_ = nullptr;          // one-shot
    int reconnect_attempt_ = 0;
    int passive_reconnect_attempt_ = 0;
    ListeningMode reconnect_mode_ = kListeningModeAutoStop;
    std::atomic<bool> reconnect_passive_{false};
    std::atomic<bool> reconnect_resume_listening_{true};
    // Sustained operation: true while we WANT an open audio channel. Set on
    // OnAudioChannelOpened; cleared by CloseAudioChannelByIntent() for every
    // user/system-initiated close. An UNEXPECTED drop leaves it true ->
    // OnAudioChannelClosed auto-reconnects so long talks aren't cut off.
    std::atomic<bool> online_intent_{false};
    // Passive lesson/nudge WebSocket: keep a claimed robot reachable by ESP
    // server without entering Listening or scheduling listen-mode reconnects.
    std::atomic<bool> passive_ws_intent_{false};
    std::string deferred_wake_word_;
    QueueHandle_t lesson_message_queue_ = nullptr;
    TaskHandle_t lesson_message_task_handle_ = nullptr;

    // Set on a backend/ws error, cleared on (re)connect. Lets the connect mapper
    // tell ONLINE apart from OFFLINE_RETRY ("Server unavailable. Retrying...").
    std::atomic<bool> backend_offline_{false};

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    std::atomic<bool> tts_audio_accepting_{false};
    std::atomic<uint32_t> speaking_generation_{0};
    std::atomic<int64_t> last_speaking_activity_ms_{0};
    std::atomic<int64_t> listening_started_ms_{0};
    std::atomic<int64_t> last_listening_activity_ms_{0};
    std::atomic<uint32_t> interrupt_count_{0};   // OBS-2: barge-in / abort count
    std::atomic<uint32_t> reconnect_count_{0};   // OBS-2: WS reconnect attempts

    // Monotonic ms timestamp of last VAD speech-start. Used to gate wake-word
    // transitions: if no human-voice VAD trigger occurred recently, drop the
    // wake-word event as a likely false-positive on ambient noise.
    int64_t last_vad_speech_ms_ = 0;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void RunScheduledTasks();
    static void SpeakingTimeoutTask(void* arg);
    void ArmSpeakingTimeout();
    void HandleSpeakingTimeout(uint32_t generation);
    void HandleListeningWatchdogTick();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void StartPassiveLessonWebsocket();
    static void OpenChannelTask(void* arg);            // T1: blocking connect, off app task
    void DoResetProtocol();                            // T1: actual reset (worker-safe)
    void ArmConnectWatchdog();                         // SM-3: bound CONNECTING
    void CancelConnectWatchdog();
    void HandleConnectWatchdog(uint32_t generation);
    void ScheduleReconnect(ListeningMode mode, bool resume_listening = true);        // T2/WSS-4: long-horizon backoff+jitter
    void SchedulePassiveLessonReconnect();             // passive lesson/nudge socket retry
    void HandleReconnectTick();
    void CloseAudioChannelByIntent();                  // intentional close -> clears online_intent_
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void FinishWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void RefreshPendingTbotClaim();
    // BluFi reported STA-connected success after a (re-)provisioning. The BLE
    // build never leaves kDeviceStateWifiConfiguring on its own (no ConfigModeExit
    // and HandleNetworkConnectedEvent ignores Connected in that state), so the
    // claim FSM would otherwise dead-end in setup and the claim would only
    // complete after an extra manual power-cycle. This drives the FSM out of
    // WifiConfiguring via the proven normal-boot path (Activating -> ActivationTask
    // -> Idle -> RefreshPendingTbotClaim) on the genuine provisioning-success entry
    // point only; the stale-event guards on HandleNetworkConnectedEvent/
    // RefreshPendingTbotClaim are left untouched.
    void PromoteFromWifiConfigAfterProvisioning();
    bool ConfirmPendingTbotClaim(bool trust_backend_expiry = false);
    // "Hi ESP needs many tries" fix: the blocking ~3s /device/config HTTP/TLS
    // fetch is split out of RefreshPendingTbotClaim() so it can run on a
    // dedicated low-priority worker instead of the priority-10 Application task
    // (which starved the wake-word AFE fetch/feed pipeline during the
    // boot-to-first-WS window). DispatchPendingTbotClaimFetch() spawns the
    // worker (single-flight); ClaimFetchTask() does ONLY the blocking fetch and
    // Schedule()s ApplyPendingTbotClaimFetchResult() back onto the Application
    // task, where all claim_substate_/BLE/SetDeviceState mutation stays serialized.
    void DispatchPendingTbotClaimFetch(const std::string& api_url, const std::string& token);
    static void ClaimFetchTask(void* arg);
    void ApplyPendingTbotClaimFetchResult(const std::string& api_url,
                                          const std::string& token,
                                          const PendingTbotClaim& pending_claim,
                                          bool fetched, int device_config_status);

    // Deferred cloud-ownership release for the BOOT re-pair flow.
    // EnterRepairPairingMode() sets backend.release_pending and KEEPS the device
    // credentials; once the robot is online + unclaimed,
    // MaybeDispatchDeferredCloudRelease() (called from RefreshPendingTbotClaim)
    // spawns a low-prio worker that POSTs /v1/devices/:id/factory-reset off the
    // prio-10 task (same single-flight pattern as the claim fetch), then clears
    // release_pending + device_secret so a different parent account can re-claim.
    void MaybeDispatchDeferredCloudRelease();
    static void CloudReleaseTask(void* arg);

    // --- TBOT claim runtime FSM helpers (C4) ---
    // Render the screen copy for the current claim sub-state from the connect
    // mapper (single source of truth = kTbotConnectStateSpecs).
    void RenderClaimSubstate(TbotClaimSubstate substate);
    // Begin / stop the bounded /device/config poll for the claim window.
    void StartClaimPoll();
    void StopClaimPoll();
    void PollPendingTbotClaimTick();
    // Arm / cancel the local expires_at deadline for a pending claim.
    void ArmClaimExpiryTimer();
    void CancelClaimExpiryTimer();
    void HandleClaimConfirmTimeout();

    // --- BLE discoverability for unclaimed standby / explicit setup ---
    // The mobile app pairs BLE-first: it scans for the BluFi advertisement
    // "TBOT-<MAC>" to find the robot, then drives claim or Wi-Fi setup. Keep
    // advertising available for unclaimed standby. Claimed online uses the BOOT
    // Wi-Fi-config path to reopen BluFi so BLE does not contend with AFE audio.
    // No-ops in non-BluFi builds.
    void EnsureBleAdvertisingForStandby();
    void StopBleAdvertising();

    // --- Heartbeat (C5) ---
    void StartHeartbeat();
    void StopHeartbeat();
    void HandleHeartbeatAuthFailure(int status_code);
    // "Hi ESP needs many tries" fix: the heartbeat does a blocking ~5s HTTP/TLS
    // POST. Like the claim poll, run it OFF the priority-10 Application task so a
    // slow backend can never freeze the core-0 wake-word AFE feed/fetch pipeline.
    // DispatchDeviceHeartbeat() runs on the Application task (gating + body build),
    // HeartbeatTask() does ONLY the blocking POST, and any auth-failure handling is
    // Schedule()d back onto the Application task (OQ1: all state stays serialized).
    void DispatchDeviceHeartbeat();
    static void HeartbeatTask(void* arg);
    // Off-task worker body: the blocking POST. Returns the HTTP status code (0 on
    // transport failure); the caller Schedule()s auth-failure handling back.
    int SendDeviceHeartbeat(const std::string& url, const std::string& device_secret,
                            std::string body);

    // Current BLE setup sub-state for the connect mapper (Off in AP/other builds).
    TbotBleSubstate GetBleSubstate() const;
    bool HandleRobotActionMessage(const cJSON* root);
    void EnqueueLessonMessage(const cJSON* root);
    static void LessonMessageTask(void* arg);
    // US-006 Slice-01 (S10): additive lesson_* renderer entry — see lesson_handler.cc.
    // Reached only via the additive `lesson_` branch in OnIncomingJson, above the
    // unknown-type no-op. Never touches the 8 legacy types / voice / MCP arm tools.
    void HandleLessonMessage(const cJSON* root);
    void HandleEmotionGesture(const char* emotion);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
