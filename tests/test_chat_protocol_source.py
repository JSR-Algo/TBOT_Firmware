from pathlib import Path
import subprocess
import pytest
import os
from test_protocol_work_lifetime import method
from test_lesson_passive_websocket_contract import function_body

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_source_signals(tmp_path, sanitize):
    assert (ROOT / "main/protocols/connection_source.h").exists(), "Missing immutable socket source"
    binary = tmp_path / "source"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), str(ROOT / "tests/native/chat_protocol_source_test.cc"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)


def test_source_callback_contract():
    header = (ROOT / "main/protocols/protocol.h").read_text()
    assert "struct SourceCallbacks" in header, "Missing additive source callbacks"
    ws = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    assert "source_sequence_.Next(connection_epoch)" in ws
    assert "DeliverIncomingJson(root, callback_transport_epoch, source, receipt)" in ws
    assert "DeliverAudioChannelOpened(source, hello_signal->deadline_us)" in ws
    assert "NotifyAudioChannelClosedOnce(source)" in ws
    assert "SetError(Lang::Strings::SERVER_TIMEOUT, source)" in ws


def test_application_source_adapter_contract():
    app = (ROOT / "main/application.cc").read_text()
    assert "Protocol::SourceCallbacks Application::MakeChatSourceCallbacks" in app
    assert "bool Application::SelectChatProtocolSource" in app


def test_actual_success_callbacks(tmp_path):
    app = (ROOT / "main/application.cc").read_text()
    fixture = (ROOT / "tests/native/chat_success_callback_test.cc").read_text()
    fixture = fixture.replace("// PRODUCTION_CALLBACKS", "\n".join(method(app,"protocol_->"+s)+");" for s in
        ["OnConnected", "OnAudioChannelOpened"]))
    generated = tmp_path / "success.cc"
    generated.write_text(fixture)
    binary = tmp_path / "success"
    subprocess.run(["c++","-std=c++17","-pthread","-Wall","-Wextra","-Werror","-Wno-unused-variable",
                    "-fsanitize=address,undefined","-I",str(ROOT / "main"),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)


def test_actual_mqtt_connected_legacy(tmp_path):
    mqtt = (ROOT / "main/protocols/mqtt_protocol.cc").read_text()
    connected = method(mqtt,"mqtt_->OnConnected") + ");"
    source = '''
#include <functional>
#include <cassert>
unsigned timers=0;
void esp_timer_stop(int) { ++timers; }
struct Mqtt { std::function<void()> callback; void OnConnected(std::function<void()> f) { callback=f; } };
struct Fixture {
    Mqtt mqtt;
    Mqtt* mqtt_=&mqtt;
    int reconnect_timer_=0;
    std::function<void()> on_connected_;
    void Install() { CALLBACK }
};
int main() { Fixture f; unsigned connections=0;
    f.on_connected_=[&]{++connections;}; f.Install(); f.mqtt.callback();
    assert(connections==1 && timers==1);
}
'''.replace("CALLBACK",connected)
    generated = tmp_path / "mqtt.cc"
    generated.write_text(source)
    binary = tmp_path / "mqtt"
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined",str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    assert "on_incoming_json_(root, IncomingJsonTransportEpoch());" in mqtt
    assert "DeliverIncomingJson" not in mqtt and "SetSourceCallbacks" not in mqtt


def source_transport_fixture():
    ws = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    header = (ROOT / "main/protocols/protocol.h").read_text()
    fixture = (ROOT / "tests/native/chat_source_transport_test.cc").read_text()
    fixture = fixture.replace("// PRODUCTION_ADOPTED_TEST", 'early.source_callbacks_.adopted=[&](ConnectionSource){return adopted;};' if 'adopted;' in header else '')
    fixture = fixture.replace("// PRODUCTION_RECEIPT_TEST", 'early.source_callbacks_.json=[&](ConnectionSource,const cJSON*,uint64_t,ConnectionReceipt receipt){++delivered;received=receipt.received_us;admission_deadline=receipt.admission_deadline_us;};' if 'ConnectionReceipt' in header else 'early.source_callbacks_.json=[&](ConnectionSource,const cJSON*,uint64_t){++delivered;};')
    fixture = fixture.replace("// PRODUCTION_BINARY", "\n".join(method(header,"struct "+s)+" __attribute__((packed));" for s in
        ["BinaryProtocol2", "BinaryProtocol3"]))
    fixture = fixture.replace("// PRODUCTION_SOURCE_CALLBACKS", method(header, "struct SourceCallbacks") + ";")
    fixture = fixture.replace("// PRODUCTION_DELIVERY", "\n".join(method(header, "void " + s) for s in
        ["DeliverIncomingAudio", "DeliverIncomingJson", "DeliverAudioChannelOpened", "DeliverAudioChannelClosed", "DeliverNetworkError"]))
    callbacks = "\n".join(method(ws, "candidate_websocket->" + s) + ");" for s in ["OnData", "OnDisconnected"])
    fixture = fixture.replace("// PRODUCTION_CALLBACKS", callbacks)
    opening = method(ws, "bool WebsocketProtocol::OpenAudioChannel")
    publication=opening[opening.index("    std::unique_ptr<WebSocket> retired_websocket;"):]
    fixture=fixture.replace("// PRODUCTION_PUBLICATION", publication)
    start = opening.index("    {\n        auto connection_mutation")
    end = opening.index("\n\n    WebSocket* candidate_websocket",start)
    fixture = fixture.replace("// PRODUCTION_CONNECTION", opening[start:end].replace("return false;", "return {};"))
    signatures = ["void WebsocketProtocol::NotifyAudioChannelClosedOnce", "void WebsocketProtocol::CompleteCloseAndNotify",
                  "void WebsocketProtocol::DetachAndResetWebsocket"]
    source_error = ws[ws.index("void WebsocketProtocol::SetError(const std::string& message, ConnectionSource source)"):]
    fixture = fixture.replace("// PRODUCTION_METHODS", "\n".join(method(ws,s) for s in signatures) + "\n" + method(source_error,"void WebsocketProtocol::SetError"))
    return fixture


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_socket_source_delivery(tmp_path, sanitize):
    fixture = source_transport_fixture()
    generated = tmp_path / "transport.cc"
    generated.write_text(fixture)
    cjson = Path(os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf"))) / "components/json/cJSON"
    flags = [f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}"]
    obj = tmp_path / "cjson.o"
    subprocess.run(["cc", *flags, "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(obj)],check=True)
    binary = tmp_path / "transport"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "main"), "-I", str(cjson), str(generated), str(obj), "-o", str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
