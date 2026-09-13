"""Execute production terminal cancellation with instrumented audio boundaries."""

from pathlib import Path
import subprocess

import pytest

from test_protocol_work_lifetime import method


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("state", ["Speaking", "Listening", "Connecting", "Idle"])
def test_terminal_quiet_fences_and_flushes_without_waiting_for_tts_stop(tmp_path, state):
    source = (ROOT / "main/application.cc").read_text()
    fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <future>
#include "lesson_audio_playout.h"
#define ESP_LOGI(...) ((void)0)
enum DeviceState { kDeviceStateSpeaking, kDeviceStateListening,
                   kDeviceStateConnecting, kDeviceStateIdle };
struct Application {
    std::vector<std::string> events;
    DeviceState state = INITIAL_STATE;
    std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};
    LessonAudioPlayout lesson_audio_playout_;
    std::mutex lesson_playout_mutex_;
    std::atomic<bool> lesson_playout_pending_{false};
    std::shared_ptr<std::atomic<bool>> lesson_playout_authorization_;
    std::atomic<uint32_t> speaking_generation_{41}, lesson_interactive_listen_generation_{7};
    std::atomic<bool> tts_audio_accepting_{true}, lesson_runtime_active_{true};
    std::atomic<bool> lesson_interactive_listen_pending_{true}, lesson_interactive_listening_active_{true};
    std::atomic<bool> lesson_idle_repaint_suppressed_{false}, connect_attempt_active_{true};
    std::atomic<bool> passive_ws_intent_{true}, online_intent_{true};
    std::atomic<int64_t> last_speaking_activity_ms_{123}, listening_started_ms_{123}, last_listening_activity_ms_{123};
    uint32_t connect_generation_ = 9;
    bool aborted_ = false;
    struct Audio {
        Application& app;
        uint32_t published_generation = 41;
        unsigned queued_frames = 10, resets = 0;
        bool voice_processing = true, wake_word = true;
        void SetPlaybackGeneration(uint32_t generation) {
            assert(!app.tts_audio_accepting_.load());
            published_generation = generation;
            app.events.emplace_back("fence");
        }
        void ResetDecoder() {
            assert(published_generation != 41);
            queued_frames = 0;
            ++resets;
            app.events.emplace_back("flush");
        }
        void EnableVoiceProcessing(bool enabled) { voice_processing = enabled; }
        void EnableWakeWordDetection(bool enabled) { wake_word = enabled; }
    } audio_service_{*this};
    struct Arm {
        unsigned cancellations = 0;
        void Cancel() { ++cancellations; }
    } speaking_arm_dispatch_;
    struct Protocol {
        unsigned stop_listening = 0;
        void SendStopListening() { ++stop_listening; }
    } protocol_storage;
    Protocol* protocol_ = &protocol_storage;
    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState next) {
        assert(next == kDeviceStateIdle);
        assert(lesson_idle_repaint_suppressed_);
        state = next;
    }
    void CancelConnectWatchdog() { events.emplace_back("cancel_connect"); }
    void CancelLessonInteractiveListening();
    void BeginLessonTerminalAudioQuiet();
};
'''.replace("INITIAL_STATE", f"kDeviceState{state}")
    fixture += method(source, "void Application::CancelLessonInteractiveListening")
    fixture += method(source, "void Application::BeginLessonTerminalAudioQuiet")
    fixture += r'''
int main() {
    Application app;
    const char* response = "0123456789abcdef0123456789abcdef";
    assert(app.lesson_audio_playout_.Begin(41, response, 1));
    app.lesson_audio_playout_.PublishOutput(41, true, 100);
    const auto initial_state = app.state;
    app.BeginLessonTerminalAudioQuiet();
    assert(!app.lesson_audio_playout_.TakeStart());
    assert(!app.lesson_audio_playout_.Current(response, 41));
    assert(!app.tts_audio_accepting_.load());
    assert(app.lesson_terminal_audio_generation_ == 42);
    assert(app.speaking_generation_ == 42);
    assert(app.audio_service_.published_generation == 42);
    assert(app.audio_service_.resets == 1 && app.audio_service_.queued_frames == 0);
    assert(app.last_speaking_activity_ms_ == 0);
    assert(app.speaking_arm_dispatch_.cancellations == 1);
    assert(!app.lesson_interactive_listen_pending_ && !app.lesson_interactive_listening_active_);
    assert(app.lesson_interactive_listen_generation_ == 8);
    assert(app.state == kDeviceStateIdle);
    assert(app.protocol_storage.stop_listening == (initial_state == kDeviceStateListening ? 1u : 0u));
    if (initial_state == kDeviceStateListening) {
        assert(!app.audio_service_.voice_processing && !app.audio_service_.wake_word);
        assert(app.listening_started_ms_ == 0 && app.last_listening_activity_ms_ == 0);
    }
    if (initial_state == kDeviceStateConnecting) {
        assert(app.connect_generation_ == 10);
        assert(!app.connect_attempt_active_ && !app.passive_ws_intent_ && !app.online_intent_);
    }
    app.lesson_runtime_active_ = false;
    std::unique_lock<std::mutex> lock(app.lesson_playout_mutex_);
    auto terminal = std::async(std::launch::async, [&] { app.BeginLessonTerminalAudioQuiet(); });
    assert(terminal.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    lock.unlock();
    terminal.get();
    assert(app.state == kDeviceStateIdle && !app.tts_audio_accepting_);
    assert(app.audio_service_.queued_frames == 0);
    assert(app.protocol_storage.stop_listening == (initial_state == kDeviceStateListening ? 1u : 0u));
}
'''
    generated = tmp_path / "terminal.cc"
    generated.write_text(fixture)
    binary = tmp_path / "terminal"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(generated), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
