from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_production_chat_conditional_transport(tmp_path):
    from test_lesson_passive_websocket_contract import function_body
    source = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    signatures = [
        "ChatOutboundMailbox::Result WebsocketProtocol::SendChatFullTextIfCurrent",
        "int WebsocketProtocol::ObserveChatPassiveLiveness",
        "ChatOutboundMailbox::Result WebsocketProtocol::SendChatControlIfCurrent",
        "ChatOutboundMailbox::Result WebsocketProtocol::SendChatAudioIfCurrent",
        "bool WebsocketProtocol::SendText",
        "bool WebsocketProtocol::SendTextForSource",
    ]
    methods = []
    for signature in signatures:
        assert signature in source, f"Missing production conditional transport: {signature}"
        start = source.index(signature)
        methods.append(source[start:source.index("{", start)] + function_body(source, signature))
    generated = tmp_path / "transport.cc"
    generated.write_text((ROOT / "tests/native/chat_conditional_transport_test.cc").read_text().replace(
        "// PRODUCTION_METHODS", "\n".join(methods)))
    idf = Path(os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf")))
    cjson = Path(os.environ.get("CJSON_DIR", str(idf / "components/json/cJSON")))
    flags = ["-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
    obj = tmp_path / "cjson.o"
    subprocess.run([shutil.which("clang") or "cc", *flags, "-Wno-deprecated-declarations",
                    "-I", str(cjson), "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    executable = tmp_path / "transport"
    subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread", *flags,
                    "-I", str(cjson), "-I", str(ROOT / "main"), str(generated), str(obj),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=15)
