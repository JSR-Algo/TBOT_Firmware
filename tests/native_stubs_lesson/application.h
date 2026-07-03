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

#include <cJSON.h>

#include <functional>
#include <memory>
#include <string_view>

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
        lesson_network_render_quiet = 0;
        schedule_calls = 0;
        play_sound_calls = 0;
        last_sound = "";
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
    int lesson_network_render_quiet = 0;
    int schedule_calls = 0;
    int play_sound_calls = 0;
    std::string last_sound;

    void Schedule(std::function<void()>&& cb) {
        schedule_calls++;
        if (cb) cb();  // run inline so the draw-lambda body executes against fakes
    }
    void PrepareLessonInteractiveListening() { prepare_listen_calls++; }
    void CancelLessonInteractiveListening() { cancel_listen_calls++; }
    DeviceState GetDeviceState() const { return device_state; }
    void AbortSpeaking(AbortReason reason) {
        abort_speaking_calls++;
        last_abort_reason = reason;
        device_state = kDeviceStateIdle;
    }
    void SetLessonRuntimeActive(bool active) { lesson_runtime_active = active; }
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
};
