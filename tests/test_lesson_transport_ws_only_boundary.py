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
  2. dispatches lesson frames ONLY through WebSocket, while the persistent lesson
     worker itself is preallocated for the application lifetime.

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


def test_lesson_protocol_dispatch_remains_websocket_only_with_persistent_worker():
    """The worker is lifecycle infrastructure, while lesson dispatch remains WS-only."""
    app = read("main/application.cc")
    init = app[app.index("void Application::InitializeProtocol()"): app.index("protocol_->OnConnected")]
    assert "lesson_worker" not in init
    assert "lesson_message_queue_" not in init
    # is_websocket_protocol is only true for the WebsocketProtocol branch.
    assert "is_websocket_protocol = true;" in app
    # And MQTT is selected when HasMqttConfig() -> never flips is_websocket_protocol.
    assert "protocol_ = std::make_unique<MqttProtocol>();" in app
    assert "auto websocket_protocol = std::make_unique<WebsocketProtocol>();" in app
    assert "protocol_ = std::move(websocket_protocol);" in app

    incoming_start = app.index("protocol_->OnIncomingJson(")
    incoming = app[incoming_start:app.index("// WebSocket Start()", incoming_start)]
    lesson_start = incoming.index('strncmp(type->valuestring, "lesson_", 7) == 0')
    lesson_branch = incoming[lesson_start:incoming.index("#endif", lesson_start)]

    assert "[this, display, is_websocket_protocol]" in incoming
    assert "if (!is_websocket_protocol)" in lesson_branch
    assert lesson_branch.index("if (!is_websocket_protocol)") < lesson_branch.index(
        "EnqueueLessonMessage(root, callback_transport_epoch);"
    )
    assert 'ESP_LOGW(TAG, "lesson_* ignored on non-WebSocket transport");' in lesson_branch
    assert lesson_branch.index("return;") < lesson_branch.index(
        "EnqueueLessonMessage(root, callback_transport_epoch);"
    )


def test_lesson_worker_uses_static_internal_storage_for_full_application_lifetime():
    app = read("main/application.cc")
    constructor = app[app.index("Application::Application()"): app.index("Application::~Application()")]
    destructor = app[app.index("Application::~Application()"): app.index("void Application::EnqueueLessonVisualCompletion")]

    assert "DRAM_ATTR StaticQueue_t lesson_message_queue_buffer;" in app
    assert "DRAM_ATTR uint8_t lesson_message_queue_storage[" in app
    assert "DRAM_ATTR StaticTask_t lesson_message_task_buffer;" in app
    assert "DRAM_ATTR StackType_t lesson_message_task_stack[" in app
    assert "lesson_message_task_stack[kLessonMessageWorkerStackDepth]" in app
    assert "sizeof(lesson_message_task_stack) == kLessonMessageWorkerStackDepth" in app
    assert "lesson_message_queue_ = xQueueCreateStatic(" in constructor
    assert "lesson_message_task_handle_ = xTaskCreateStatic(" in constructor
    assert "xQueueCreate(" not in constructor
    assert "xTaskCreateWithCaps" not in constructor
    assert "vQueueDelete(lesson_message_queue_)" not in destructor
