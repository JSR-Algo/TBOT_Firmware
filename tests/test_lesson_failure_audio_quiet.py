"""The actual lesson failure lambda must stop queued, not-yet-audible audio."""

from pathlib import Path
import subprocess

from test_protocol_work_lifetime import method


ROOT = Path(__file__).resolve().parents[1]


def test_actual_failure_cleanup_fences_queued_audio_before_speaking(tmp_path):
    source = (ROOT / "main/lesson_handler.cc").read_text()
    cleanup = method(source, "auto end_lesson_after_failure =")
    fixture = r'''
#include <cassert>
struct Session { unsigned current_transport_epoch = 7; bool running=true, paused=false, prepared=true; } g_session;
struct Layers { void ClearAll() {} } g_layer_state;
void InvalidateLessonVisualCompletionState(unsigned) {}
void CancelAndRestoreActiveLessonEmbodiedAction() {}
void ClearTerminalLessonCursor() {}
struct Application {
    bool terminal=false, lesson_runtime=true, queued_audio=true, intake=true;
    static Application& GetInstance() { static Application app; return app; }
    void BeginLessonTerminalAudioQuiet() { terminal=true; queued_audio=false; intake=false; }
    void CancelLessonInteractiveListening() {}
    void SetLessonRuntimeActive(bool active) {
        assert(!active);
        assert(terminal && !queued_audio && !intake);
        lesson_runtime=active;
    }
    void Exercise() {
        bool display=false, assets=false;
        auto show_lesson_failure_display = [&] { display=true; };
        // Before the first output frame the device is Idle, so this production
        // state-gated helper does not call AbortSpeaking.
        auto abort_speaking_if_needed = [] {};
        auto end_lesson_asset_session = [&] { assets=true; };
'''+cleanup+r''';
        end_lesson_after_failure();
        assert(display && assets && !lesson_runtime && !g_session.running && !g_session.prepared);
    }
};
int main() { Application::GetInstance().Exercise(); }
'''
    generated = tmp_path / "failure.cc"
    generated.write_text(fixture)
    binary = tmp_path / "failure"
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
