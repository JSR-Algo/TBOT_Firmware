from pathlib import Path
import subprocess
import os
import pytest
from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("sanitize", ["none", "address"] + (["thread"] if os.environ.get("CHAT_OUTBOUND_TSAN") == "1" else []))
def test_actual_application_outbound_adapter(tmp_path, sanitize):
    source = (ROOT / "main/application.cc").read_text()
    signatures = [
        "bool Application::InitializeChatOutboundWorker",
        "ChatOutboundMailbox::Result Application::ActivateChatOutbound",
        "ChatOutboundMailbox::Result Application::SubmitChatOutbound",
        "void Application::RetireChatOutbound",
        "bool Application::PollChatOutbound",
        "bool Application::IsChatOutboundCompletionCurrent",
        "void Application::NotifyChatOutbound",
        "void Application::ChatOutboundTask",
        "void Application::PollChatOutboundEvents",
        "void Application::RequestInitializeProtocol",
        "void Application::DoResetProtocol",
        "bool Application::CompletePendingProtocolWork",
        "void Application::Reboot",
        "void Application::CompleteReboot",
        "void Application::ResetProtocol",
        "void Application::CloseAudioChannelByIntent",
        "void Application::HandleConnectWatchdog",
        "bool Application::StartOpenChannelWorker",
        "void Application::HeartbeatTask",
        "void Application::ScheduleDeferredProtocolClose",
    ]
    for signature in signatures:
        assert signature in source, f"Missing actual outbound adapter: {signature}"
    fixture = (ROOT / "tests/native/chat_outbound_worker_test.cc").read_text()
    cpp = tmp_path / "adapter.cc"
    cpp.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(method(source, s) for s in signatures)))
    binary = tmp_path / "worker"
    for name in ["chat_outbound_worker.h", "chat_outbound_mailbox.h"]:
        (tmp_path / name).write_text((ROOT / "main" / name).read_text().replace("private:", "public:"))
    worker = tmp_path / "chat_outbound_worker.cc"
    worker.write_text((ROOT / "main/chat_outbound_worker.cc").read_text())
    flags = [] if sanitize == "none" else [f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}", "-fno-omit-frame-pointer"]
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(tmp_path), "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/native_stubs"),
                    "-I", str(Path.home() / "esp/esp-idf/components/json/cJSON"),
                    str(cpp), str(worker), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
