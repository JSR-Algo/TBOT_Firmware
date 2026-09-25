import subprocess
import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index(name)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_protocol_work_lifetime(tmp_path):
    binary = tmp_path / "protocol_work_lifetime"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"), str(ROOT / "tests/native/protocol_work_lifetime_test.cc"), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30)


@pytest.mark.parametrize("sanitize", [False, True])
def test_application_lifetime_adapters(tmp_path, sanitize):
    source = (ROOT / "main/application.cc").read_text()
    methods = [
        "bool Application::StartOpenChannelWorker",
        "void Application::DoResetProtocol",
        "bool Application::CompletePendingProtocolWork",
        "void Application::RequestInitializeProtocol",
        "void Application::CompleteProtocolActivation",
        "void Application::ScheduleDeferredProtocolClose",
        "void Application::Reboot",
        "void Application::ResetProtocol",
        "void Application::OpenChannelTask",
        "void Application::StartProtocolWorker",
        "void Application::CloseAudioChannelByIntent",
        "bool Application::IsConnectSuccessPublicationSuppressed",
        "void Application::CompleteClaimProtocolActivation",
        "void Application::CompleteReboot",
        "void Application::HandleConnectWatchdog",
        "void Application::ContinueOpenAudioChannel",
        "void Application::StartPassiveLessonWebsocket",
        "void Application::HandleReconnectTick",
    ]
    adapter = (ROOT / "tests/native/protocol_application_lifetime_adapter.cc").read_text()
    adapter = adapter.replace("// PRODUCTION_OPENED_CALLBACK", method(source, "protocol_->OnAudioChannelOpened(") + ");")
    cpp = tmp_path / "application.cc"
    cpp.write_text(adapter.replace("// PRODUCTION_METHODS", "\n".join(method(source, name) for name in methods)))
    binary = tmp_path / "application"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []
    subprocess.run(["c++", "-std=c++17", "-pthread", *flags, "-I", str(ROOT / "main"), str(cpp), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30)


def test_reset_adapter_preserves_reserved_protocol_after_watchdog(tmp_path):
    source = (ROOT / "main/application.cc").read_text()
    adapter = r'''
#include "protocol_work_lifetime.h"
#include <atomic>
#include <memory>
#include <cassert>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct Protocol { bool IsAudioChannelOpened() { return false; } };
struct Application {
 bool chat_cleanup_enabled_ = false;
 bool PollChatProtocolCleanup() { assert(false); return false; }
 std::unique_ptr<Protocol> protocol_{new Protocol};
 ProtocolWorkLifetime protocol_work_lifetime_;
 std::atomic<bool> connect_in_flight_{false}, reset_pending_{false};
 std::atomic<uint64_t> protocol_generation_{1};
 void RequestLessonStorageAbandonment() {}
 void CancelLessonRobotEntranceOnDisplay() {}
 void CloseAudioChannelByIntent() {}
 void RetireChatOutbound() {}
 void DoResetProtocol();
};
'''
    adapter += method(source, "void Application::DoResetProtocol()")
    adapter += r'''
int main() {
 Application app;
 auto token = app.protocol_work_lifetime_.Reserve();
 app.connect_in_flight_.store(false); // Watchdog expired, actual worker still owns it.
 app.DoResetProtocol();
 assert(app.protocol_ != nullptr);
 assert(app.protocol_generation_ == 1);
 app.protocol_work_lifetime_.Release(token);
 app.DoResetProtocol();
 assert(app.protocol_ == nullptr);
 assert(app.protocol_generation_ == 2);
}
'''
    cpp = tmp_path / "adapter.cc"
    cpp.write_text(adapter)
    binary = tmp_path / "adapter"
    subprocess.run(["c++", "-std=c++17", "-I", str(ROOT / "main"), str(cpp), "-o", str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30)
