from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_codec_uses_bounded_refill_before_mic_encode():
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    body = source.split("void AudioService::OpusCodecTask()", 1)[1]
    assert "AudioPlaybackRefillPolicy refill_policy;" in body
    guard = body.index("refill_policy.DeferEncode(")
    assert body.index("debug_statistics_.decode_count++") < guard
    assert guard < body.index("/* Encode the audio to send queue */")
    assert "decoded_for_playback = true;" in body


def test_stale_skip_preserves_outstanding_refill_budget():
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    body = source.split("void AudioService::OpusCodecTask()", 1)[1]
    stale = body.split("if (packet->generation != playback_generation_.load())", 1)[1]
    stale = stale.split("uint32_t encoded_size", 1)[0]
    assert "continue;" in stale
    assert "refill_policy =" not in stale
    assert "refill_policy.DeferEncode" not in stale


def test_refill_policy_native(tmp_path):
    compiler = shutil.which("clang++") or shutil.which("c++")
    assert compiler, "C++ compiler required"
    executable = tmp_path / "refill-test"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/audio_playback_refill_test.cc"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
