#pragma once
// Host stub for main/application.h (lesson subset). Declares the Application surface
// lesson_handler.cc's Application::HandleLessonMessage implementation requires.
//
// Schedule() runs the callback INLINE so the LVGL-draw lambda bodies (the back-to-front
// SetLessonBackground/Object/RobotOverlay + SetEmotion/SetChatMessage draws and the
// interactive-listen Schedule) actually execute against the recording fakes — proving
// the renderer scheduled the right draw with the right image pointer. On the real
// device Schedule() marshals onto the LVGL/app task; the pixel pipeline that runs there
// is ESP32-S3 hardware (CP-7) and is out of host scope.
#include "protocol.h"
#include "robot_uart.h"
#include "lesson_handler.h"

#include <cJSON.h>

#include <functional>
#include <memory>
#include <cstdint>
#include <string_view>
#include <vector>

enum DeviceState {
    kDeviceStateIdle,
    kDeviceStateSpeaking
};

class Application {
public:
    static Application& GetInstance() {
        static Application inst;
        return inst;
    }

    // Reset all per-test observable state (does NOT reset lesson_handler.cc's file-local
    // g_session — that is reset by sending a fresh lesson_prepare, exactly as on device).
    void HostReset() {
        protocol_ = std::make_unique<Protocol>();
        prepare_listen_calls = 0;
        cancel_listen_calls = 0;
        abort_speaking_calls = 0;
        last_abort_reason = kAbortReasonNone;
        device_state = kDeviceStateIdle;
        lesson_runtime_active = false;
        lesson_terminal_audio_quiet = false;
        lesson_network_render_quiet = 0;
        schedule_calls = 0;
        defer_scheduled_callbacks = false;
        schedule_wait_succeeds = true;
        schedule_wait_starts_before_timeout = false;
        deferred_callbacks.clear();
        lesson_visual_queue.clear();
        lesson_interactive_listen_generation = 0;
        play_sound_calls = 0;
        last_sound = "";
        robot_uart_.Reset();
    }

    // ---- members the renderer touches ----
    // FW-LESSON-03 leak guard: a test can protocol_.reset() to exercise the
    // `if (protocol_)` false branch (frame built + consumed, send skipped).
    std::unique_ptr<Protocol> protocol_;

    int prepare_listen_calls = 0;
    int cancel_listen_calls = 0;
    int abort_speaking_calls = 0;
    AbortReason last_abort_reason = kAbortReasonNone;
    DeviceState device_state = kDeviceStateIdle;
    bool lesson_runtime_active = false;
    bool lesson_terminal_audio_quiet = false;
    int lesson_network_render_quiet = 0;
    int schedule_calls = 0;
    bool defer_scheduled_callbacks = false;
    bool schedule_wait_succeeds = true;
    bool schedule_wait_starts_before_timeout = false;
    std::vector<std::function<void()>> deferred_callbacks;
    std::vector<LessonQueueItem> lesson_visual_queue;
    uint32_t lesson_interactive_listen_generation = 0;
    int play_sound_calls = 0;
    std::string last_sound;
    RobotUart robot_uart_;

    void Schedule(std::function<void()>&& cb) {
        schedule_calls++;
        if (defer_scheduled_callbacks) {
            deferred_callbacks.push_back(std::move(cb));
            return;
        }
        if (cb) cb();  // run inline so the draw-lambda body executes against fakes
    }
    bool ScheduleAndWait(std::function<bool()>&& cb, int) {
        schedule_calls++;
        if (!schedule_wait_succeeds && !schedule_wait_starts_before_timeout) return false;
        return cb ? cb() : false;
    }
    void FlushScheduledCallbacks() {
        auto callbacks = std::move(deferred_callbacks);
        deferred_callbacks.clear();
        for (auto& cb : callbacks) {
            if (cb) cb();
        }
    }
    void EnqueueLessonVisualCompletion(
        LessonQueueItemKind kind,
        std::uint64_t transport_epoch,
        std::uint64_t visual_generation,
        std::int64_t server_sequence,
        const char* assignment_id,
        const char* session_id,
        const char* step_id,
        LessonVisualCompletionResult result,
        const char* degraded_reason = nullptr,
        std::uint64_t visual_nonce = 0) {
        lesson_visual_queue.push_back(MakeLessonVisualQueueItem(
            kind, transport_epoch, visual_generation, server_sequence,
            assignment_id, session_id, step_id, result, degraded_reason, visual_nonce));
    }
    void DrainLessonVisualQueue() {
        auto items = std::move(lesson_visual_queue);
        lesson_visual_queue.clear();
        for (const auto& item : items) {
            DispatchLessonVisualCompletion(item, protocol_.get(), &robot_uart_);
        }
    }
    uint32_t BeginLessonInteractiveListeningRequest() { return ++lesson_interactive_listen_generation; }
    void PrepareLessonInteractiveListening() { prepare_listen_calls++; }
    void PrepareLessonInteractiveListening(uint32_t generation) {
        if (generation == lesson_interactive_listen_generation) prepare_listen_calls++;
    }
    void CancelLessonInteractiveListening() {
        lesson_interactive_listen_generation++;
        cancel_listen_calls++;
    }
    DeviceState GetDeviceState() const { return device_state; }
    void AbortSpeaking(AbortReason reason) {
        abort_speaking_calls++;
        last_abort_reason = reason;
        device_state = kDeviceStateIdle;
    }
    void BeginLessonTerminalAudioQuiet() { lesson_terminal_audio_quiet = true; }
    void SetLessonRuntimeActive(bool active) {
        lesson_runtime_active = active;
        if (active) lesson_terminal_audio_quiet = false;
    }
    void PlaySound(const std::string_view& sound) {
        play_sound_calls++;
        last_sound = std::string(sound);
    }
    void BeginLessonNetworkRenderQuiet() { lesson_network_render_quiet++; }
    void EndLessonNetworkRenderQuiet() {
        if (lesson_network_render_quiet > 0) lesson_network_render_quiet--;
    }
    bool IsLessonNetworkRenderQuiet() const { return lesson_network_render_quiet > 0; }

    // Defined in lesson_handler.cc (the unit under test).
    void HandleLessonMessage(const cJSON* root);
    bool AbandonLessonStorageSession();
};
