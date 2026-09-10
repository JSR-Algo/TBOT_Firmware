from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_production_playback_drain_snapshot_native(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    signature = "bool AudioService::TryGetPlaybackDrainSnapshot"
    assert signature in source, "Missing nonblocking playback drain snapshot"
    header = (ROOT / "main/audio/audio_service.h").read_text()
    assert "bool TryGetPlaybackDrainSnapshot(PlaybackDrainSnapshot& out);" in header
    start = source.index(signature)
    declaration = source[start:source.index("{", start)]
    body = function_body(source, signature)
    fixture = (ROOT / "tests/native/audio_playback_drain_snapshot_test.cc").read_text()
    generated = tmp_path / "snapshot.cc"
    generated.write_text(fixture.replace("// PRODUCTION_SNAPSHOT", declaration + body))
    executable = tmp_path / "snapshot"
    subprocess.run([
        shutil.which("clang++") or "c++", "-std=c++17", "-pthread",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(ROOT / "main"), str(generated), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
