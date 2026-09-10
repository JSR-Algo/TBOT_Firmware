from pathlib import Path
import os
import subprocess
import pytest
from test_protocol_work_lifetime import method
from test_lesson_passive_websocket_contract import function_body

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_chat_playback_reset(tmp_path, sanitize):
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    signatures = ["uint32_t AudioService::RequestChatPlaybackReset", "bool AudioService::ResetChatDecoder",
                  "bool AudioService::PushChatPacketToDecodeQueue", "bool AudioService::PushPacketToDecodeQueue",
                  "bool AudioService::TryGetPlaybackResetEpoch"]
    for signature in signatures:
        assert signature in source, f"Missing chat playback boundary: {signature}"
    fixture = (ROOT / "tests/native/chat_playback_reset_test.cc").read_text()
    methods = []
    for signature in signatures:
        body = method(source, signature)
        if signature == "bool AudioService::ResetChatDecoder":
            needle = "std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);"
            assert body.count(needle) == 1
            body = body.replace(needle, "if (reset_entry_hook) reset_entry_hook();\n" + needle)
        if signature == "bool AudioService::PushPacketToDecodeQueue":
            needle = "std::unique_lock<std::mutex> lock(audio_queue_mutex_);"
            body = body.replace(needle, "if (queue_entry_hook) queue_entry_hook();\n" + needle)
        methods.append(body)
    generated = tmp_path / "reset.cc"
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(methods)))
    binary = tmp_path / "reset"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_codec_reset_fence(tmp_path, sanitize):
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    codec = method(source, "void AudioService::OpusCodecTask")
    assert "chat_playback_reset_.AllowsDecode(packet->chat_reset_token)" in codec, "Codec lacks chat reset admission fence"
    fixture = (ROOT / "tests/native/chat_playback_codec_test.cc").read_text()
    predicate = function_body(codec, "audio_queue_cv_.wait(lock, [this]()")
    fixture = fixture.replace("// PRODUCTION_WAIT_PREDICATE", predicate[1:-1])
    signature = "if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE"
    block = function_body(codec, signature)
    condition_start = codec.index(signature)
    condition_end = codec.index("{", condition_start)
    condition = codec[condition_start:condition_end]
    watermark = "const auto reset_watermark = chat_playback_reset_.Requested();"
    assert codec.index(watermark) < condition_start
    condition = watermark + "\n" + condition
    dequeue = "audio_decode_queue_.pop_front();"
    assert block.count(dequeue) == 1
    block = block.replace(dequeue, dequeue + "\nif (decode_dequeued_hook) decode_dequeued_hook();")
    needle = "std::unique_lock<std::mutex> transition(chat_decode_transition_mutex_);"
    assert block.count(needle) == 1
    block = block.replace(needle, "if (decode_claim_hook) decode_claim_hook();\n" + needle)
    generated = tmp_path / "codec.cc"
    reset_method = method(source, "bool AudioService::ResetChatDecoder")
    reset_method = reset_method.replace(needle, "if (reset_claim_hook) reset_claim_hook();\n" + needle)
    fixture = fixture.replace("// PRODUCTION_METHODS",
        method(source, "void AudioService::SetDecodeSampleRate") + "\n" + reset_method)
    generated.write_text(fixture.replace("// PRODUCTION_DECODE", condition + block))
    binary = tmp_path / "codec"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
@pytest.mark.parametrize("server_aec", [0, 1])
def test_actual_output_reset_fence(tmp_path, sanitize, server_aec):
    source = (ROOT / "main/audio/audio_service.cc").read_text()
    output = method(source, "void AudioService::AudioOutputTask")
    assert "chat_playback_reset_.AllowsDecode(task->chat_reset_token)" in output, "Missing pre-output reset fence"
    fixture = (ROOT / "tests/native/chat_playback_output_test.cc").read_text()
    generated = tmp_path / "output.cc"
    generated.write_text(fixture.replace("// PRODUCTION_OUTPUT", output))
    binary = tmp_path / "output"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    f"-DCONFIG_USE_SERVER_AEC={server_aec}",
                    "-I", str(ROOT / "main"), str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_reset_saturates(tmp_path):
    source = (ROOT / "main/audio/chat_playback_reset.h").read_text()
    assert "requested_{0}" in source
    (tmp_path / "saturated.h").write_text(source.replace("requested_{0}", "requested_{UINT32_MAX - 2}"))
    generated = tmp_path / "saturated.cc"
    generated.write_text('#include "saturated.h"\n#include <cassert>\n'
                         'int main() { ChatPlaybackReset r; auto t=r.Request(); '
                         'assert(t==UINT32_MAX-1 && r.Pending()); assert(r.Complete(t)); '
                         'assert(!r.Pending() && r.AllowsDecode(t)); '
                         'assert(r.Request()==UINT32_MAX && r.Pending()); '
                         'assert(!r.Complete(UINT32_MAX) && !r.Current(UINT32_MAX)); '
                         'assert(!r.AllowsDecode(t)); assert(r.Request()==UINT32_MAX && r.Pending()); }')
    binary = tmp_path / "saturated"
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
