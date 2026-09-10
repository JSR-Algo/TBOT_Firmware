from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_decode_fence_is_wired_through_completion_and_reset():
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    worker = source.split("void AudioService::OpusCodecTask()", 1)[1].split(
        "void AudioService::SetDecodeSampleRate", 1)[0]
    assert "const auto decode_epoch = audio_decode_fence_.Begin();" in worker
    begin = worker.index("audio_decode_fence_.Begin()")
    assert begin < worker.index("lock.unlock();")
    completion = worker.index("audio_decode_fence_.Complete(")
    assert worker.index("decoder_lock.unlock();") < completion
    assert worker.rfind("lock.lock();", 0, completion) > worker.index("decoder_lock.unlock();")
    assert "decode_epoch, packet->generation, playback_generation_.load()" in worker
    assert completion < worker.index("audio_playback_queue_.push_back")
    assert "if (decode_succeeded && decode_current)" in worker
    after_completion = worker[completion:worker.index("/* Encode the audio to send queue */")]
    assert after_completion.index("audio_queue_cv_.notify_all();") < after_completion.index("refill_policy.DeferEncode(")
    assert "continue;" not in after_completion.split("refill_policy.DeferEncode(", 1)[0]
    reset = source.split("void AudioService::ResetDecoder()", 1)[1].split(
        "void AudioService::GetQueueDepths", 1)[0]
    assert reset.index("lock(audio_queue_mutex_)") < reset.index("audio_decode_fence_.Reset();")
    assert reset.index("audio_decode_fence_.Reset();") < reset.index("decoder_lock(decoder_mutex_)")
    assert "Complete(" not in reset
    wait = source.split("bool AudioService::WaitForPlaybackQueueEmpty", 1)[1].split(
        "void AudioService::ResetDecoder", 1)[0]
    assert "!audio_decode_fence_.InFlight()" in wait


def test_decode_fence_native(tmp_path):
    compiler = shutil.which("clang++") or shutil.which("c++")
    assert compiler, "C++ compiler required"
    assert (ROOT / "main/audio/audio_decode_fence.h").exists(), "Production decode fence missing"
    executable = tmp_path / "decode-fence-test"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/audio_decode_fence_test.cc"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
