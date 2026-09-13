from pathlib import Path
import subprocess
import re

ROOT = Path(__file__).resolve().parents[1]


def test_actual_playout_identity_and_drain(tmp_path):
    source = ROOT / "tests/native/lesson_audio_playout_test.cc"
    binary = tmp_path / "playout"
    subprocess.run(["clang++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)


def test_actual_output_fences_cancelled_lesson_pcm(tmp_path):
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    expression = re.search(r"const bool current_output =\s*(.*?);", source, re.S).group(1)
    generated = tmp_path / "output.cc"
    generated.write_text('''
#include <atomic>
#include <cassert>
#include <cstdint>
struct Task { uint32_t chat_reset_token=0, response_generation=1; bool conversation_audio=true; };
struct Reset { bool AllowsDecode(uint32_t token) { return token==1; } };
int main() {
    Task storage; auto* task=&storage;
    Reset chat_playback_reset_; std::atomic<uint32_t> playback_generation_{2};
    auto current=[&]() { return ''' + expression + '''; };
    assert(!current()); // Cancelled lesson PCM must not reach OutputData.
    task->response_generation=2; assert(current());
    task->conversation_audio=false; task->response_generation=0; assert(current());
    task->conversation_audio=true; task->chat_reset_token=1; assert(!current());
    task->response_generation=2; assert(current());
    task->chat_reset_token=2; assert(!current());
}
''')
    binary = tmp_path / "output"
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
