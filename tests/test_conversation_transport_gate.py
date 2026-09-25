from pathlib import Path
import shutil
import subprocess
import os
import json
import re
import shlex
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_transport_gate_native(tmp_path):
    header = (ROOT / "main/protocols/connection_inbound_gate.h").read_text()
    assert "HealthyEpoch()" in header, "Missing atomic healthy connection publication"
    compiler = shutil.which("clang++") or shutil.which("c++")
    executable = tmp_path / "transport-gate"
    subprocess.run([
        compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/conversation_transport_gate_test.cc"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_target_healthy_epoch_getter_is_inline_load(tmp_path):
    commands = ROOT / "build/compile_commands.json"
    if not commands.exists():
        pytest.skip("Target compilation database required")
    entry = next(item for item in json.loads(commands.read_text())
                 if item["file"].endswith("/protocols/websocket_protocol.cc"))
    args = shlex.split(entry["command"])
    compiler = Path(args[0])
    if not compiler.exists() or "xtensa" not in compiler.name:
        pytest.skip("Configured Xtensa toolchain required")
    source = tmp_path / "epoch_probe.cc"
    source.write_text('#include "protocols/connection_inbound_gate.h"\n'
                      'extern "C" uint32_t ReadHealthyEpoch(const ConnectionInboundGate* gate) { return gate->HealthyEpoch(); }\n')
    obj = tmp_path / "epoch_probe.o"
    args[args.index("-o") + 1] = str(obj)
    args[args.index("-c") + 1] = str(source)
    args.extend(["-I", str(ROOT / "main")])
    subprocess.run(args, cwd=entry["directory"], check=True, timeout=60)
    objdump = str(compiler).replace("g++", "objdump")
    assembly = subprocess.check_output([objdump, "-d", "--disassemble=ReadHealthyEpoch", str(obj)], text=True)
    instructions = re.findall(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(\S+)", assembly, re.MULTILINE)
    assert instructions, assembly
    assert not any(op.startswith(("call", "j", "b", "loop")) for op in instructions), assembly
    assert any(op.startswith("l32i") for op in instructions), assembly


def test_conditional_ack_boundary():
    from test_lesson_passive_websocket_contract import function_body
    protocol = (ROOT / "main/protocols/protocol.h").read_text()
    source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    assert "CurrentConnectionEpoch()" in protocol
    assert "SendConversationTtsDrainAckIfCurrent" in protocol
    assert "return 0;" in function_body(protocol, "virtual uint32_t CurrentConnectionEpoch")
    assert "ConversationTtsAckResult::Failed" in function_body(protocol, "virtual ConversationTtsAckResult SendConversationTtsDrainAckIfCurrent")
    websocket_header = (ROOT / "main/protocols/websocket_protocol.h").read_text()
    assert "return inbound_gate_.HealthyEpoch();" in function_body(websocket_header, "uint32_t CurrentConnectionEpoch")
    body = function_body(source, "ConversationTtsAckResult WebsocketProtocol::SendConversationTtsDrainAckIfCurrent")
    assert body.index("TryAcquire(expected_connection_epoch)") < body.index("cJSON_CreateObject()") < body.index("SendText(encoded)")
    for field in ('"type", "tts_ack"', '"state", "stop"', '"drainId", drain_id.c_str()', '"session_id", session_id_.c_str()'):
        assert f"!cJSON_AddStringToObject(root, {field})" in body
    for validation in ('drain_id.size() <= 5', 'drain_id.size() > 128', 'drain_id.compare(0, 5, "chat:")', "drain_id.find('\\0')", "session_id_.find('\\0')"):
        assert validation in body
    assert "cJSON_free(encoded)" in body and "cJSON_Delete(root)" in body
    assert "ConversationTtsAckResult::Busy" in body
    assert "ConversationTtsAckResult::Stale" in body
    send = function_body(source, "bool WebsocketProtocol::SendText")
    assert "websocket_->Send(text)" in send and "SetError(" in send
    assert "inbound_gate_.FailCurrent()" in function_body(source, "void WebsocketProtocol::SetError")


def test_production_conditional_sender_native(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    idf = Path(os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf")))
    cjson = Path(os.environ.get("CJSON_DIR", str(idf / "components/json/cJSON")))
    assert (cjson / "cJSON.c").exists(), "Set CJSON_DIR to an ESP-IDF cJSON checkout"
    source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    signature = "ConversationTtsAckResult WebsocketProtocol::SendConversationTtsDrainAckIfCurrent"
    start = source.index(signature)
    declaration = source[start:source.index("{", start)]
    body = function_body(source, signature)
    fixture = (ROOT / "tests/native/conversation_conditional_ack_test.cc").read_text()
    generated = tmp_path / "conditional_ack.cc"
    generated.write_text(fixture.replace("// PRODUCTION_SENDER", declaration + body))
    flags = ["-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
    obj = tmp_path / "cjson.o"
    subprocess.run([shutil.which("clang") or "cc", *flags, "-Wno-deprecated-declarations", "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    executable = tmp_path / "conditional-ack"
    subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread", *flags,
                    "-I", str(cjson), "-I", str(ROOT / "main"), str(generated), str(obj), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_disconnect_invalidates_before_notification(tmp_path):
    from test_lesson_passive_websocket_contract import websocket_callback_body
    source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    body = websocket_callback_body(source, "OnDisconnected")
    assert "inbound_gate_.FailCurrent();" in body, "Current disconnect must invalidate connection publication"
    body = body[body.index("{"):body.rindex("}") + 1]
    generated = tmp_path / "disconnect.cc"
    generated.write_text('''
#include "protocols/connection_inbound_gate.h"
#include "protocols/connection_source.h"
#include <cassert>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct Socket { int GetLastError() { return 0; } };
struct Fixture {
    ConnectionInboundGate inbound_gate_;
    ConnectionSource current_source_{1,1};
    int notifications = 0;
    bool IsTimeout() { return false; }
    void NotifyAudioChannelClosedOnce(ConnectionSource source) {
        assert(source.source_id==current_source_.source_id);
        assert(inbound_gate_.HealthyEpoch() == 0);
        ++notifications;
    }
    void Disconnect(uint32_t connection_epoch, Socket* candidate_websocket, ConnectionSource source={1,1})
''' + body + '''
};
int main() {
    Fixture fixture;
    Socket socket;
    const auto old = fixture.inbound_gate_.BeginConnection();
    fixture.Disconnect(old, &socket);
    assert(fixture.notifications == 1);
    assert(fixture.inbound_gate_.HealthyEpoch() == 0);
    assert(fixture.inbound_gate_.CurrentEpoch() == old);
    assert(!fixture.inbound_gate_.TryAcquire(old));
    const auto next = fixture.inbound_gate_.BeginConnection();
    fixture.Disconnect(old, &socket);
    assert(fixture.inbound_gate_.HealthyEpoch() == next);
    assert(fixture.notifications == 1);
}
''')
    executable = tmp_path / "disconnect"
    subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"), str(generated), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_production_socket_publication_and_detach(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    opening = function_body(source, "bool WebsocketProtocol::OpenAudioChannel")
    publication = "{\n" + opening[opening.index("    std::unique_ptr<WebSocket> retired_websocket;"):]
    publication = publication.replace("{\n", "{\n    const ConnectionSource source{connection_epoch,connection_epoch};\n    struct Hello {uint64_t deadline_us=250100;std::atomic<bool> published{false};};\n    auto hello_signal=std::make_shared<Hello>();\n",1)
    detach = "void WebsocketProtocol::DetachAndResetWebsocket(uint32_t expected_epoch, bool notify, ConnectionSource source)" + function_body(source, "void WebsocketProtocol::DetachAndResetWebsocket")
    fixture = (ROOT / "tests/native/conversation_socket_binding_test.cc").read_text()
    generated = tmp_path / "socket_binding.cc"
    generated.write_text(fixture.replace("// PRODUCTION_PUBLICATION", publication).replace("// PRODUCTION_DETACH", detach))
    executable = tmp_path / "socket-binding"
    subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"), str(generated), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_open_failures_hold_matching_lease(tmp_path):
    source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    opening = source[source.index("bool WebsocketProtocol::OpenAudioChannel"):]
    branches = []
    for error in ("SERVER_NOT_CONNECTED", "SERVER_ERROR", "SERVER_TIMEOUT"):
        end = opening.index(f"SetError(Lang::Strings::{error}, source);") + len(f"SetError(Lang::Strings::{error}, source);")
        start = opening.rfind("        auto failure_lease", 0, end)
        assert start >= 0, "Handshake failures require a matching lease across SetError"
        branches.append(opening[start:end])
    generated = tmp_path / "open_failures.cc"
    prefix = '''
#include "protocols/connection_inbound_gate.h"
#include "protocols/connection_source.h"
#include <cassert>
#include <chrono>
#include <future>
using namespace std::chrono_literals;
namespace Lang { namespace Strings {
const char* SERVER_NOT_CONNECTED = "connect";
const char* SERVER_ERROR = "send";
const char* SERVER_TIMEOUT = "timeout";
}}
struct Fixture {
    ConnectionInboundGate inbound_gate_;
    int errors = 0;
    void SetError(const char*,ConnectionSource source) {
        assert(source.connection_epoch==inbound_gate_.CurrentEpoch());
        assert(inbound_gate_.CurrentThreadHasLease());
        inbound_gate_.FailCurrent();
        ++errors;
    }
'''
    for index, body in enumerate(branches):
        prefix += f"bool Fail{index}(uint32_t connection_epoch) {{\nconst ConnectionSource source{{1,connection_epoch}};\n{body}\n return false; }}\n"
    prefix += '''};
int main() {
    for (auto fail : {&Fixture::Fail0, &Fixture::Fail1, &Fixture::Fail2}) {
        Fixture fixture;
        const auto old = fixture.inbound_gate_.BeginConnection();
        (fixture.*fail)(old);
        assert(fixture.errors == 1 && fixture.inbound_gate_.HealthyEpoch() == 0);
        assert(fixture.inbound_gate_.CurrentEpoch() == old);
        const auto next = fixture.inbound_gate_.BeginConnection();
        std::future<bool> stale;
        std::promise<void> attempting;
        {
            auto lease = fixture.inbound_gate_.Acquire(next);
            stale = std::async(std::launch::async, [&] {
                attempting.set_value();
                return (fixture.*fail)(old);
            });
            attempting.get_future().wait();
            assert(stale.wait_for(30ms) == std::future_status::timeout);
        }
        stale.get();
        assert(fixture.errors == 1 && fixture.inbound_gate_.HealthyEpoch() == next);
    }
}
'''
    generated.write_text(prefix)
    executable = tmp_path / "open-failures"
    subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"), str(generated), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
