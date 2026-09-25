from pathlib import Path
import json
import re
import shlex
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("probe", ["uplink", "playback_reset", "playout_intake", "start_handoff"])
def test_target_authorization_is_bounded(tmp_path, probe):
    commands = ROOT / "build/compile_commands.json"
    if not commands.exists():
        pytest.skip("Configured target compilation database required")
    entry = next(item for item in json.loads(commands.read_text())
                 if item["file"].endswith("/audio/audio_service.cc"))
    args = shlex.split(entry["command"])
    compiler = Path(args[0])
    if not compiler.exists() or "xtensa" not in compiler.name:
        pytest.skip("Configured Xtensa toolchain required")
    source = tmp_path / "authorization_probe.cc"
    source.write_text('#include "audio/chat_uplink_authorization.h"\n'
                      'extern "C" uint32_t Revoke(ChatUplinkAuthorization* p) { return p->Revoke(); }\n'
                      'extern "C" bool Arm(ChatUplinkAuthorization* p, uint32_t r, bool chat) { return p->Arm(r, chat); }\n'
                      'extern "C" bool Current(const ChatUplinkAuthorization* p, ChatCaptureTag t) { return p->IsCurrentChat(t); }\n')
    symbols = ("Revoke", "Arm", "Current")
    if probe == "playback_reset":
        source.write_text('#include "audio/chat_playback_reset.h"\n'
                          'extern "C" uint32_t Request(ChatPlaybackReset* p) { return p->Request(); }\n'
                          'extern "C" bool Current(const ChatPlaybackReset* p, uint32_t t) { return p->Current(t); }\n'
                          'extern "C" bool Pending(const ChatPlaybackReset* p) { return p->Pending(); }\n')
        symbols = ("Request", "Current", "Pending")
    if probe == "playout_intake":
        source.write_text('#include "chat_playout_intake.h"\n'
                          'extern "C" uint32_t Establish(ChatPlayoutIntake* p, const ChatPlayoutIntake::Response* r) { return p->Establish(*r); }\n'
                          'extern "C" bool Current(const ChatPlayoutIntake* p, uint32_t t) { return p->Current(t); }\n'
                          'extern "C" bool Capture(const ChatPlayoutIntake* p, ChatPlayoutIntake::Capture* c) { return p->TryCapture(*c); }\n')
        symbols = ("_ZN17ChatPlayoutIntake9EstablishERKNS_8ResponseE",
                   "_ZNK17ChatPlayoutIntake7CurrentEm", "_ZNK17ChatPlayoutIntake10TryCaptureERNS_7CaptureE")
    if probe == "start_handoff":
        source.write_text('#include "chat_start_handoff.h"\n'
                          'extern "C" bool Publish(ChatStartHandoff* p, ChatStartHandoff::Request* r) { return p->Publish(*r); }\n'
                          'extern "C" bool Admit(ChatStartHandoff* p, const ChatStartHandoff::Request* r, uint32_t g, uint32_t t, uint64_t n) { return p->Admit(*r,g,t,n); }\n'
                          'extern "C" void Expire(ChatStartHandoff* p, const ChatStartHandoff::Request* r) { p->Expire(*r); }\n'
                          'extern "C" bool Read(ChatStartHandoff* p, const ChatStartHandoff::Request* r, uint64_t n, ChatStartHandoff::Admission* a) { return p->TryAdmission(*r,n,*a); }\n'
                          'extern "C" bool Confirm(ChatStartHandoff* p, const ChatStartHandoff::Request* r, uint64_t n) { return p->Confirm(*r,n); }\n')
    obj = tmp_path / "authorization_probe.o"
    args[args.index("-o") + 1] = str(obj)
    args[args.index("-c") + 1] = str(source)
    args.extend(["-I", str(ROOT / "main")])
    subprocess.run(args, cwd=entry["directory"], check=True, timeout=60)
    no_afe_args = list(args)
    no_afe_args[no_afe_args.index("-c") + 1] = str(ROOT / "main/audio/processors/no_audio_processor.cc")
    no_afe_args[no_afe_args.index("-o") + 1] = str(tmp_path / "no_audio_processor.o")
    subprocess.run(no_afe_args, cwd=entry["directory"], check=True, timeout=60)
    objdump = str(compiler).replace("g++", "objdump")
    if probe == "start_handoff":
        symbol_table = subprocess.check_output([str(compiler).replace("g++", "nm"), "--defined-only", str(obj)], text=True)
        symbols = tuple(re.findall(r"^[0-9a-f]+ [TW] (\S+)$", symbol_table, re.MULTILINE))
        assert symbols
    if probe in ("playout_intake", "start_handoff"):
        relocated = subprocess.check_output([objdump, "-dr", str(obj)], text=True)
        targets = set(re.findall(r"R_XTENSA_ASM_EXPAND\s+(\S+)", relocated))
        assert targets <= set(symbols) | {"memcpy", "memset"}, relocated
        # The target emits fixed 36-byte capture copy/zero helpers, not atomic
        # runtime locks or RMW. All scalar helper control flow is checked below.
    for symbol in symbols:
        assembly = subprocess.check_output([objdump, "-d", "--disassemble=" + symbol, str(obj)], text=True)
        instructions = re.findall(r"^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]+[ \t]+(\S+)[ \t]*([^\n]*)", assembly, re.MULTILINE)
        assert instructions, assembly
        forbidden = ("loop", "s32c1i") if probe in ("playout_intake", "start_handoff") else ("call", "loop", "s32c1i")
        assert not any(op.startswith(forbidden) for _, op, _ in instructions), assembly
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
        def acyclic(address, path):
            assert address not in path, assembly
            for successor in edges[address]:
                acyclic(successor, path | {address})
        acyclic(int(instructions[0][0], 16), set())


def test_authorization_saturates(tmp_path):
    source = (ROOT / "main/audio/chat_uplink_authorization.h").read_text()
    assert "state_{0}" in source
    (tmp_path / "saturated.h").write_text(source.replace("state_{0}", "state_{UINT32_MAX - 9}"))
    generated = tmp_path / "saturated.cc"
    generated.write_text('#include "saturated.h"\n#include <cassert>\n'
                         'int main() { ChatUplinkAuthorization a; auto r=a.Revoke(); '
                         'a.AcknowledgePrepared(r,true); assert(a.Arm(r)); '
                         'assert(a.Revoke()==UINT32_MAX); a.AcknowledgePrepared(UINT32_MAX,true); '
                         'assert(!a.Arm(UINT32_MAX)); assert(a.Revoke()==UINT32_MAX); '
                         'assert(!a.Accepts(a.Capture())); }')
    binary = tmp_path / "saturated"
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_encode_completion_boundary(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    signature = "if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE)"
    body = function_body(source[source.index("/* Encode the audio to send queue */"):], signature)
    fixture = (ROOT / "tests/native/chat_encode_capture_test.cc").read_text()
    generated = tmp_path / "encode.cc"
    generated.write_text(fixture.replace("// PRODUCTION_ENCODE_BLOCK", body))
    binary = tmp_path / "encode"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_service_capture_boundaries(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    methods = []
    for signature in ["bool AudioService::ReadAudioData", "void AudioService::PushTaskToEncodeQueue",
                      "bool AudioService::PrepareChatUplink"]:
        assert signature in source, f"Missing capture boundary: {signature}"
        start = source.index(signature)
        method = source[start:source.index("{", start)] + function_body(source, signature)
        if signature == "void AudioService::PushTaskToEncodeQueue":
            needle = "std::unique_lock<std::mutex> lock(audio_queue_mutex_);"
            assert method.count(needle) == 1
            method = method.replace(needle, "if (queue_attempt_hook) queue_attempt_hook();\n" + needle)
            predicate = "return service_stopped_ || audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE;"
            assert method.count(predicate) == 1
            method = method.replace(predicate, "if (queue_wait_hook) queue_wait_hook();\n" + predicate)
        methods.append(method)
    input_branch = function_body(source, "if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING))")
    methods.append("void AudioService::InputOnce(int bits) { do " + input_branch + " while (false); }")
    assert "packet->capture_tag = task->capture_tag" in source
    assert "chat_uplink_authorization_.Accepts(packet->capture_tag)" in source
    fixture = (ROOT / "tests/native/chat_service_capture_test.cc").read_text()
    generated = tmp_path / "service.cc"
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(methods)))
    binary = tmp_path / "service"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_processor_capture_boundaries(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    methods = []
    for name, signatures in {
        "no_audio_processor": ["void NoAudioProcessor::Feed", "void NoAudioProcessor::Stop",
                               "void NoAudioProcessor::PrepareCapture"],
        "afe_audio_processor": ["void AfeAudioProcessor::Feed", "void AfeAudioProcessor::Stop",
                                "void AfeAudioProcessor::PrepareCapture",
                                "void AfeAudioProcessor::FetchAudio"],
    }.items():
        source = (ROOT / f"main/audio/processors/{name}.cc").read_text()
        for signature in signatures:
            assert signature in source, f"Missing capture-aware production method: {signature}"
            start = source.index(signature)
            methods.append(source[start:source.index("{", start)] + function_body(source, signature))
    fixture = (ROOT / "tests/native/chat_processor_capture_test.cc").read_text()
    generated = tmp_path / "processors.cc"
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(methods)))
    binary = tmp_path / "processors"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_authorization_native(tmp_path):
    header = ROOT / "main/audio/chat_uplink_authorization.h"
    assert header.exists(), "Capture authorization prerequisite is missing"
    binary = tmp_path / "authorization"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(ROOT / "tests/native/chat_uplink_authorization_test.cc"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
