from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_websocket_hello_advertises_lesson_audio_drain_ack_additively():
    websocket = read("main/protocols/websocket_protocol.cc")
    mqtt = read("main/protocols/mqtt_protocol.cc")

    assert 'cJSON_AddBoolToObject(features, "lessonAudioDrainAck", true);' in websocket
    assert '"lessonAudioDrainAck"' not in mqtt


def test_normal_stop_waits_for_playback_then_sends_matching_ack():
    application = read("main/application.cc")
    protocol_header = read("main/protocols/protocol.h")
    protocol_source = read("main/protocols/protocol.cc")

    parse = application.index('cJSON_GetObjectItem(root, "drainId")')
    wait = application.index("WaitForPlaybackQueueEmpty", parse)
    send = application.index("SendTtsDrainAck", wait)
    assert parse < wait < send
    assert "if (!is_interrupt && !tts_drain_id.empty())" in application
    assert "void SendTtsDrainAck(const std::string& drain_id);" in protocol_header
    assert 'cJSON_AddStringToObject(root, "type", "tts_ack")' in protocol_source
    assert 'cJSON_AddStringToObject(root, "state", "stop")' in protocol_source
    assert 'cJSON_AddStringToObject(root, "drainId", drain_id.c_str())' in protocol_source


def test_interrupt_stop_does_not_wait_or_ack_and_terminal_quarantine_remains():
    application = read("main/application.cc")

    assert "if (!is_interrupt && !tts_drain_id.empty())" in application
    assert "tts_stop_interrupt_flush" in application
    assert "terminal lesson tts stop matched generation_hi=" in application
    assert "stale terminal lesson tts stop ignored" in application


def test_playback_drain_waits_for_the_in_flight_codec_write_to_finish():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")

    assert "bool audio_playback_in_flight_ = false;" in header
    output = source.index("void AudioService::AudioOutputTask()")
    claimed = source.index("audio_playback_in_flight_ = true;", output)
    write = source.index("codec_->OutputData(task->pcm);", claimed)
    completed = source.index("audio_playback_in_flight_ = false;", write)
    assert claimed < write < completed
    wait = source.index("bool AudioService::WaitForPlaybackQueueEmpty")
    assert "!audio_playback_in_flight_" in source[wait : wait + 500]
