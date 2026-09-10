from pathlib import Path
import shutil
import subprocess
import json
import shlex
import re
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_duplex_reads_and_writes_use_independent_lifetime_locks():
    source = (ROOT / "main/audio/codecs/es8311_audio_codec.cc").read_text()
    read = source.split("int Es8311AudioCodec::Read(", 1)[1].split("int Es8311AudioCodec::Write(", 1)[0]
    write = source.split("int Es8311AudioCodec::Write(", 1)[1]
    assert "lock(read_mutex_)" in read
    assert "data_if_mutex_" not in read
    assert "lock(data_if_mutex_)" in write
    assert "read_mutex_" not in write
    for method in ("~Es8311AudioCodec()", "EnableInput(bool enable)", "EnableOutput(bool enable)"):
        body = source.split("Es8311AudioCodec::" + method + " {", 1)[1].split("\n}", 1)[0]
        assert "std::scoped_lock lock(data_if_mutex_, read_mutex_)" in body


def test_queued_writes_publish_before_wait_and_keep_driver_timing_boundary():
    source = (ROOT / "main/audio/codecs/es8311_audio_codec.cc").read_text()
    write = source.split("int Es8311AudioCodec::Write(", 1)[1]
    assert write.index("output_drain_->BeginWrite();") < write.index("lock(data_if_mutex_)")
    assert write.index("samples <= 0") < write.index("stereo_data.resize")
    assert write.index("driver_end_us = esp_timer_get_time()") < write.index("output_drain_->FinishWrite(ret == ESP_OK)")


def test_open_failure_latches_drain_and_preserves_fail_fast_behavior():
    source = (ROOT / "main/audio/codecs/es8311_audio_codec.cc").read_text()
    opening = source.split("const auto open_result = esp_codec_dev_open(dev_, &fs);", 1)[1].split(
        "esp_codec_dev_set_in_gain", 1)[0]
    assert "if (open_result != ESP_OK) output_drain_->Fail();" in opening
    assert "ESP_ERROR_CHECK(open_result);" in opening
    assert "WITHOUT_ABORT" not in opening
    assert "return" not in opening


def test_hardware_drain_native(tmp_path):
    assert (ROOT / "main/audio/codecs/es8311_hardware_drain.h").exists(), "Hardware drain implementation missing"
    compiler = shutil.which("clang++") or shutil.which("c++")
    assert compiler
    executable = tmp_path / "hardware-drain"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/es8311_hardware_drain_test.cc"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_checked_adapter_native(tmp_path):
    assert (ROOT / "main/audio/codecs/es8311_data_adapter.h").exists(), "Checked adapter missing"
    compiler = shutil.which("clang++") or shutil.which("c++")
    executable = tmp_path / "checked-adapter"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        "-I", str(ROOT / "tests/native/codec_stubs"),
        "-I", str(ROOT / "managed_components/espressif__esp_codec_dev/interface"),
        "-I", str(ROOT / "managed_components/espressif__esp_codec_dev/include"),
        str(ROOT / "tests/native/es8311_data_adapter_test.cc"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_target_eof_callback_has_no_calls_or_retry_loop(tmp_path):
    commands = ROOT / "build/compile_commands.json"
    if not commands.exists():
        pytest.skip("Target compilation database required for ISR instruction check")
    entry = next(item for item in json.loads(commands.read_text())
                 if item["file"].endswith("/audio/codecs/es8311_audio_codec.cc"))
    args = shlex.split(entry["command"])
    compiler = Path(args[0])
    if not compiler.exists() or "xtensa" not in compiler.name:
        pytest.skip("Configured Xtensa toolchain required for ISR instruction check")
    obj = tmp_path / "es8311_audio_codec.o"
    args[args.index("-o") + 1] = str(obj)
    subprocess.run(args, cwd=entry["directory"], check=True, timeout=120)
    objdump = str(compiler).replace("g++", "objdump")
    symbols = subprocess.check_output([objdump, "-t", str(obj)], text=True)
    callback = next(line for line in symbols.splitlines() if "OnTxSent" in line)
    assert ".iram" in callback, callback
    symbol = callback.split()[-1]
    assembly = subprocess.check_output([objdump, "-d", "--disassemble=" + symbol, str(obj)], text=True)
    instructions = re.findall(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(\S+)", assembly, re.MULTILINE)
    assert instructions, assembly
    assert not any(op.startswith(("call", "j", "b", "loop")) for op in instructions), assembly
    assert any(op.startswith("l32i") for op in instructions), assembly
    assert any(op.startswith("s32i") for op in instructions), assembly
    assert any(op.startswith("retw") for op in instructions), assembly
