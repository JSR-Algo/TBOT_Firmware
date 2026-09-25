from pathlib import Path
import subprocess
import os
import pytest

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def test_selected_stop_entry_does_not_send_on_application(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    fixture='''
#include <atomic>
#include <cassert>
#include <cstdint>
#define ESP_LOGI(...) ((void)0)
enum {kDeviceStateListening=1,kDeviceStateIdle=2,kDeviceStateAudioTesting=3,kDeviceStateWifiConfiguring=4};
struct Protocol { unsigned sends=0;void SendStopListening(){++sends;} };
struct Audio { void EnableAudioTesting(bool){assert(false);} };
struct Application {
    Protocol transport;Protocol* protocol_=&transport;Audio audio_service_;
    std::atomic<bool> lesson_interactive_listen_pending_{false},lesson_interactive_listening_active_{false},lesson_runtime_active_{false};
    std::atomic<uint32_t> lesson_interactive_listen_generation_{0};
    std::atomic<int64_t> listening_started_ms_{0},last_listening_activity_ms_{0};
    int state=kDeviceStateListening;int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}
    void HandleStopListeningEvent();
    bool HandleChatStopListening() { state=kDeviceStateIdle;return true; }
};
'''
    fixture+=method(source,"void Application::HandleStopListeningEvent")
    fixture+='\nint main(){Application app;app.HandleStopListeningEvent();assert(app.transport.sends==0);}\n'
    generated=tmp_path / "stop.cc";generated.write_text(fixture)
    binary=tmp_path / "stop"
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined",str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_chat_cleanup_requests_reset_before_new_generation():
    source = (ROOT / "main/application.cc").read_text()
    request = method(source, "uint32_t Application::RequestChatAudioCleanup")
    assert "RequestChatPlaybackReset()" in request
    assert request.index("RequestChatPlaybackReset()") < request.index("SetPlaybackGeneration(")
    worker = method(source, "void Application::RunChatAudioCleanup")
    assert "ResetChatDecoder(chat_audio_work_.reset_serial)" in worker
    assert "ResetDecoder()" not in worker


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
@pytest.mark.parametrize("listening_wake", [False, True])
def test_actual_deferred_audio_cleanup(tmp_path, sanitize, listening_wake):
    app = (ROOT / "main/application.cc").read_text()
    audio = (ROOT / "main/audio/audio_service.cc").read_text()
    signatures = [
        "bool Application::RequestChatCue",
        "uint32_t Application::RequestChatAudioCleanup",
        "uint32_t Application::RequestChatPlaybackCleanup",
        "void Application::PollChatAudioCleanup",
        "void Application::RunChatAudioCleanup",
        "void Application::RetryChatAudioCleanup",
        "void Application::BeginChatRebootAudioCleanup",
        "void Application::PollChatReboot",
    ]
    for signature in signatures:
        assert signature in app, f"Missing deferred Application boundary: {signature}"
    generated = tmp_path / "application.cc"
    fixture = (ROOT / "tests/native/chat_outbound_application_test.cc").read_text()
    fixture = fixture.split("// TRANSPORT_FIXTURE")[0]
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(
        [method(app, signature) for signature in signatures]
        + [method(audio, "bool AudioService::PrepareChatAudioTransition"),
           method(audio, "bool AudioService::PrepareChatUplink"),
           method(audio, "uint32_t AudioService::RequestChatPlaybackReset"),
           method(audio, "bool AudioService::ResetChatDecoder").replace(
               "std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);",
               "if (reset_entry_hook) reset_entry_hook();\nstd::unique_lock<std::mutex> decoder_lock(decoder_mutex_);")]
    )))
    binary = tmp_path / "application"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    *(["-DCONFIG_WAKE_WORD_DETECTION_IN_LISTENING=1"] if listening_wake else []),
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}", "-I", str(ROOT / "main"),
                    str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_deferred_transport_cleanup(tmp_path, sanitize):
    source = (ROOT / "main/application.cc").read_text()
    signatures = ["bool Application::PollChatProtocolCleanup",
                  "void Application::RunChatProtocolCleanup",
                  "bool Application::CompletePendingProtocolWork"]
    for signature in signatures:
        assert signature in source, f"Missing retained transport cleanup: {signature}"
    fixture = (ROOT / "tests/native/chat_outbound_application_test.cc").read_text()
    fixture = fixture[fixture.index("// TRANSPORT_FIXTURE") + len("// TRANSPORT_FIXTURE"):]
    fixture = fixture.split("// SIGNAL_FIXTURE")[0]
    generated = tmp_path / "transport.cc"
    generated.write_text(fixture.replace("// PRODUCTION_TRANSPORT_METHODS", "\n".join(
        method(source, signature) for signature in signatures)))
    binary = tmp_path / "transport"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}", "-I", str(ROOT / "main"),
                    str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_protocol_callback_signal_fence(tmp_path, sanitize):
    assert (ROOT / "main/chat_protocol_signals.h").exists(), "Missing bounded callback signal fence"
    fixture = (ROOT / "tests/native/chat_outbound_application_test.cc").read_text()
    generated = tmp_path / "signals.cc"
    generated.write_text(fixture.split("// SIGNAL_FIXTURE")[1])
    binary = tmp_path / "signals"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_actual_afe_initialization_failures(tmp_path):
    source = (ROOT / "main/audio/processors/afe_audio_processor.cc").read_text()
    header = (ROOT / "main/audio/processors/afe_audio_processor.h").read_text()
    fixture = (ROOT / "tests/native/chat_audio_initialization_test.cc").read_text()
    fixture = fixture.replace("// PRODUCTION_READINESS", method(header, "bool IsCaptureReady() const").replace(" override", ""))
    signatures = ["void AfeAudioProcessor::Initialize", "AfeAudioProcessor::~AfeAudioProcessor",
                  "void AfeAudioProcessor::Start", "void AfeAudioProcessor::Stop", "bool AfeAudioProcessor::IsRunning"]
    generated = tmp_path / "afe.cc"
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(method(source, s) for s in signatures)))
    binary = tmp_path / "afe"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(generated), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_error_callback_ownership(tmp_path, sanitize):
    source = (ROOT / "main/application.cc").read_text()
    fixture = (ROOT / "tests/native/chat_protocol_callback_test.cc").read_text()
    fixture = fixture.replace("// PRODUCTION_ERROR_CALLBACK", method(source, "protocol_->OnNetworkError(") + ");")
    fixture = fixture.replace("// PRODUCTION_CLOSED_CALLBACK", method(source, "protocol_->OnAudioChannelClosed(") + ");")
    fixture = fixture.replace("// PRODUCTION_METHODS", "\n".join(method(source,s) for s in
        ["void Application::PollChatProtocolSignals", "Protocol::SourceCallbacks Application::MakeChatSourceCallbacks",
         "bool Application::SelectChatProtocolSource"]))
    generated = tmp_path / "callback.cc"
    signal = (ROOT / "main/chat_protocol_signals.h").read_text()
    signal = signal.replace("class ChatProtocolSignals", "extern std::function<void()> source_publish_hook;\nclass ChatProtocolSignals")
    signature = "bool PublishConnectionFault(ConnectionSource source, uint32_t connect_generation, uint32_t flags) {"
    signal = signal.replace(signature, signature + "\nif (source_publish_hook) source_publish_hook();")
    (tmp_path / "chat_protocol_signals.h").write_text(signal)
    generated.write_text(fixture)
    binary = tmp_path / "callback"
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    obj=tmp_path / "cjson.o"
    subprocess.run(["cc", "-Wno-deprecated-declarations", "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), "-I", str(cjson), str(generated), str(obj), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=20)
