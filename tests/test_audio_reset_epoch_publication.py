from pathlib import Path
import shutil
import subprocess
import json
import re
import shlex
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_target_publication_has_no_atomic_helpers_or_retry_loops(tmp_path):
    commands = ROOT / "build/compile_commands.json"
    if not commands.exists():
        pytest.skip("Target compilation database required")
    entry = next(item for item in json.loads(commands.read_text())
                 if item["file"].endswith("/audio/audio_service.cc"))
    args = shlex.split(entry["command"])
    compiler = Path(args[0])
    if not compiler.exists() or "xtensa" not in compiler.name:
        pytest.skip("Configured Xtensa toolchain required")
    source = tmp_path / "epoch_probe.cc"
    source.write_text('#include "audio/audio_reset_epoch_publication.h"\n'
                      'extern "C" bool ReadEpoch(const AudioResetEpochPublication* p, uint64_t& out) { return p->TryRead(out); }\n'
                      'extern "C" void BeginEpoch(AudioResetEpochPublication* p) { p->BeginReset(); }\n'
                      'extern "C" void PublishEpoch(AudioResetEpochPublication* p, uint64_t epoch) { p->Publish(epoch); }\n')
    obj = tmp_path / "epoch_probe.o"
    args[args.index("-o") + 1] = str(obj)
    args[args.index("-c") + 1] = str(source)
    args.extend(["-I", str(ROOT / "main")])
    subprocess.run(args, cwd=entry["directory"], check=True, timeout=60)
    objdump = str(compiler).replace("g++", "objdump")
    for symbol in ("ReadEpoch", "BeginEpoch", "PublishEpoch"):
        assembly = subprocess.check_output([objdump, "-d", "--disassemble=" + symbol, str(obj)], text=True)
        instructions = re.findall(r"^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]+[ \t]+(\S+)[ \t]*([^\n]*)", assembly, re.MULTILINE)
        assert instructions, assembly
        assert not any(op.startswith(("call", "loop", "s32c1i")) for _, op, _ in instructions), assembly
        edges = {}
        for index, (address, op, operands) in enumerate(instructions):
            successors = []
            if op.startswith(("b", "j")):
                target = re.search(r"(?:^|[,\s])([0-9a-f]+)\s*<", operands)
                assert target, assembly
                successors.append(int(target[1], 16))
            if not op.startswith(("ret", "j")) and index + 1 < len(instructions):
                successors.append(int(instructions[index + 1][0], 16))
            edges[int(address, 16)] = successors
        def check_acyclic(address, path):
            assert address not in path, assembly
            for successor in edges[address]:
                check_acyclic(successor, path | {address})
        check_acyclic(int(instructions[0][0], 16), set())
        assert any(op.startswith("l32i") for _, op, _ in instructions), assembly
        assert any(op == "memw" for _, op, _ in instructions), assembly
        if symbol != "ReadEpoch":
            assert any(op.startswith("s32i") for _, op, _ in instructions), assembly


def test_publication_reset_brackets_fence_before_decoder_wait():
    from test_lesson_passive_websocket_contract import function_body
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    body = function_body(source, "void AudioService::ResetDecoder")
    operations = ["lock(audio_queue_mutex_)", "audio_reset_epoch_publication_.BeginReset()",
                  "audio_decode_fence_.Reset()",
                  "audio_reset_epoch_publication_.Publish(audio_decode_fence_.ResetEpoch())",
                  "decoder_lock(decoder_mutex_)", "esp_opus_dec_reset(opus_decoder_)"]
    offsets = [body.index(operation) for operation in operations]
    assert offsets == sorted(offsets)
    assert source.count("audio_decode_fence_.Reset()") == 2
    chat = function_body(source, "bool AudioService::ResetChatDecoder")
    assert chat.index("transition.unlock()") < chat.index("lock(audio_queue_mutex_)")
    assert chat.index("lock(audio_queue_mutex_)") < chat.index("audio_decode_fence_.Reset()")


def test_publication_intermediate_states(tmp_path):
    header = ROOT / "main/audio/audio_reset_epoch_publication.h"
    assert header.exists(), "Missing bounded reset epoch publication"
    source = header.read_text().replace("#pragma once", "")
    # Instrument the copied implementation, never expose production test APIs.
    needles = ["low_.store(static_cast<uint32_t>(epoch), std::memory_order_seq_cst);",
               "high_.store(static_cast<uint32_t>(epoch >> 32), std::memory_order_seq_cst);",
               "const uint32_t low = low_.load(std::memory_order_seq_cst);"]
    for index, needle in enumerate(needles):
        assert source.count(needle) == 1
        source = source.replace(needle, needle + f"\n EpochPublicationHook({index});")
    # Start near sequence exhaustion to exercise the permanent-busy boundary.
    saturated = source.replace("AudioResetEpochPublication", "SaturatingPublication").replace(
        "sequence_{0}", "sequence_{UINT32_MAX - 3}")
    fixture = (ROOT / "tests/native/audio_reset_epoch_interleaving_test.cc").read_text()
    generated = tmp_path / "interleaving.cc"
    generated.write_text(fixture.replace("// INSTRUMENTED_PUBLICATIONS", source + saturated))
    executable = tmp_path / "interleaving"
    subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(generated), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_reset_epoch_publication_native(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    header = ROOT / "main/audio/audio_reset_epoch_publication.h"
    assert header.exists(), "Missing bounded reset epoch publication"
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    service_header = (ROOT / "main/audio/audio_service.h").read_text()
    assert "bool TryGetPlaybackResetEpoch(uint64_t& out) const;" in service_header
    methods = []
    for signature in ("bool AudioService::TryGetPlaybackResetEpoch", "void AudioService::ResetDecoder"):
        assert signature in source, f"Missing production {signature}"
        start = source.index(signature)
        methods.append(source[start:source.index("{", start)] + function_body(source, signature))
    fixture = (ROOT / "tests/native/audio_reset_epoch_publication_test.cc").read_text()
    generated = tmp_path / "epoch.cc"
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(methods)))
    executable = tmp_path / "epoch"
    subprocess.run([
        shutil.which("clang++") or "c++", "-std=c++17", "-pthread",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(ROOT / "main"), str(generated), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)
