"""Lesson runtime is a WebSocket-only capability — assert that boundary in source.

BLOCKER #0 audit (2026-06-20): the US-006 test-coverage report flags that the
MQTT hello does NOT advertise ``features.lesson`` while the WebSocket hello does,
and worries the ESP server raises ``LESSON_VERSION_UNSUPPORTED`` on MQTT.

VERDICT: this is INTENTIONAL, not a bug. The TBOT ESP server (esp32-server
tbot-server) is WebSocket-only (``app.py`` -> ``WebSocketServer``); it never
speaks the firmware ``MqttProtocol`` transport, which connects to a native MQTT
broker and opens a SEPARATE AES-encrypted UDP channel for audio only. The lesson
worker, lesson_* dispatch, and lesson_step rendering all ride the WebSocket JSON
text channel (``on_incoming_json_``). The firmware deliberately:
  1. advertises ``features.lesson`` ONLY on the WebSocket hello, and
  2. creates the lesson worker queue ONLY when ``is_websocket_protocol`` is true.

These tests lock that boundary so a naive "just add lesson to MQTT too" change
(which would be wrong — MQTT cannot carry lesson_step frames the same way) is
caught here instead of silently shipping a broken transport.

Pure source-reader (no ESP-IDF / no hardware), consistent with the other
firmware contract tests.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def _features_block(cc_text):
    """Return the substring spanning the features object construction."""
    start = cc_text.index("cJSON* features = cJSON_CreateObject();")
    end = cc_text.index('cJSON_AddItemToObject(root, "features", features);', start)
    return cc_text[start:end]


def test_websocket_hello_advertises_lesson_capability():
    ws_features = _features_block(read("main/protocols/websocket_protocol.cc"))
    # mcp stays alongside lesson; lesson + renderer are advertised (D-CAP-FLAG).
    assert 'cJSON_AddBoolToObject(features, "mcp", true);' in ws_features
    assert 'cJSON_AddBoolToObject(features, "lesson", true);' in ws_features
    assert 'cJSON_AddStringToObject(features, "renderer", kLessonRendererName);' in ws_features


def test_mqtt_hello_intentionally_omits_lesson_capability():
    """MQTT is a control-channel + separate UDP-audio transport; it cannot carry
    lesson_step the way the WebSocket JSON channel does. The hello MUST NOT claim
    a lesson capability it cannot honor (advertising it would make the server send
    lesson_prepare into a transport with no lesson worker -> dropped frames)."""
    mqtt = read("main/protocols/mqtt_protocol.cc")
    mqtt_features = _features_block(mqtt)
    assert 'cJSON_AddBoolToObject(features, "mcp", true);' in mqtt_features
    # The boundary: NO lesson / renderer capability on the MQTT hello.
    assert '"lesson"' not in mqtt_features
    assert '"renderer"' not in mqtt_features
    # The MQTT hello negotiates a separate UDP audio channel, not a JSON lesson path.
    assert 'cJSON_AddStringToObject(root, "transport", "udp");' in mqtt


def test_lesson_worker_queue_is_gated_on_websocket_transport_only():
    """The lesson_* worker (HTTP/TLS asset fetch + decode off the receive stack)
    is created ONLY for the WebSocket protocol. MQTT never gets a lesson worker,
    so even a stray lesson_* frame has nowhere to dispatch — the WS-only contract."""
    app = read("main/application.cc")
    init = app[app.index("void Application::InitializeProtocol()"): app.index("protocol_->OnConnected")]
    # The queue/task creation is inside an is_websocket_protocol guard.
    guard = init.index("if (is_websocket_protocol && lesson_message_queue_ == nullptr")
    create = init.index('xTaskCreateWithCaps(&Application::LessonMessageTask, "lesson_worker"')
    assert guard < create, "lesson worker must be created under the is_websocket_protocol guard"
    # is_websocket_protocol is only true for the WebsocketProtocol branch.
    assert "is_websocket_protocol = true;" in app
    # And MQTT is selected when HasMqttConfig() -> never flips is_websocket_protocol.
    assert "protocol_ = std::make_unique<MqttProtocol>();" in app
    assert "protocol_ = std::make_unique<WebsocketProtocol>();" in app
