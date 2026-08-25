from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def enclosing_scoped_blocks(text: str, marker: str):
    marker_index = text.index(marker)
    stack = []
    for index, char in enumerate(text[:marker_index]):
        if char == "{":
            stack.append(index)
        elif char == "}":
            stack.pop()
    assert stack, f"marker is not inside a brace scope: {marker}"

    for block_start in reversed(stack):
        depth = 0
        for index in range(block_start, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    yield text[block_start : index + 1]
                    break
        else:
            raise AssertionError(f"unterminated scope containing {marker}")


def guard_declaration_scope(text: str) -> str:
    return next(enclosing_scoped_blocks(text, "OpusEncoderSerialization::Acquire()"))


def test_all_esp_opus_encoder_calls_share_one_serialization_boundary():
    helper_header = read("main/audio/opus_encoder_serialization.h")
    helper_source = read("main/audio/opus_encoder_serialization.cc")
    cmake = read("main/CMakeLists.txt")

    assert "class OpusEncoderSerialization" in helper_header
    assert "using Lease = std::unique_lock<std::mutex>;" in helper_header
    assert "static Lease Acquire();" in helper_header
    assert "std::mutex OpusEncoderSerialization::mutex_;" in helper_source
    assert '"audio/opus_encoder_serialization.cc"' in cmake

    expected_calls = {
        "main/audio/audio_service.cc": 4,
        "main/audio/wake_words/afe_wake_word.cc": 4,
        "main/audio/wake_words/custom_wake_word.cc": 4,
    }
    encoder_calls = (
        "esp_opus_enc_open(",
        "esp_opus_enc_get_frame_size(",
        "esp_opus_enc_process(",
        "esp_opus_enc_close(",
    )
    discovered = {}
    for path in (ROOT / "main").rglob("*.[ch]*"):
        source = path.read_text(encoding="utf-8")
        count = sum(source.count(call) for call in encoder_calls)
        if count:
            discovered[str(path.relative_to(ROOT))] = count
    assert discovered == expected_calls


def test_main_encoder_guards_only_library_calls_not_queue_or_network_work():
    source = read("main/audio/audio_service.cc")

    destructor = function_body(source, "AudioService::~AudioService()")
    close_scope = guard_declaration_scope(destructor)
    assert close_scope.index("OpusEncoderSerialization::Acquire()") < close_scope.index(
        "esp_opus_enc_close(opus_encoder_)"
    )

    initialize = function_body(source, "void AudioService::Initialize(AudioCodec* codec)")
    init_scope = guard_declaration_scope(initialize)
    lease = init_scope.index("OpusEncoderSerialization::Acquire()")
    opened = init_scope.index("esp_opus_enc_open(", lease)
    sized = init_scope.index("esp_opus_enc_get_frame_size(", opened)
    assert lease < opened < sized

    codec_task = function_body(source, "void AudioService::OpusCodecTask()")
    process = codec_task.index("esp_opus_enc_process(")
    process_scope = guard_declaration_scope(codec_task)
    lease = process_scope.index("OpusEncoderSerialization::Acquire()")
    guarded_process = process_scope.index("esp_opus_enc_process(", lease)
    payload = codec_task.index("packet->payload.assign", process)
    callback = codec_task.index("callbacks_.on_send_queue_available", payload)
    assert lease < guarded_process
    assert "packet->payload.assign" not in process_scope
    assert "callbacks_.on_send_queue_available" not in process_scope
    assert process < payload < callback


def test_temporary_wake_encoders_hold_one_lease_from_open_through_close():
    for path, signature in (
        ("main/audio/wake_words/afe_wake_word.cc", "void AfeWakeWord::EncodeWakeWordData()"),
        ("main/audio/wake_words/custom_wake_word.cc", "void CustomWakeWord::EncodeWakeWordData()"),
    ):
        body = function_body(read(path), signature)
        call_markers = (
            "esp_opus_enc_open(",
            "esp_opus_enc_get_frame_size(",
            "esp_opus_enc_process(",
            "esp_opus_enc_close(",
        )
        lifetime_scope = guard_declaration_scope(body)
        lease = lifetime_scope.index("OpusEncoderSerialization::Acquire()")
        opened = lifetime_scope.index(call_markers[0], lease)
        sized = lifetime_scope.index(call_markers[1], opened)
        processed = lifetime_scope.index(call_markers[2], sized)
        closed = lifetime_scope.index(call_markers[3], processed)
        assert lease < opened < sized < processed < closed
        assert ".unlock()" not in lifetime_scope[lease:closed]


def test_native_lease_exclusion_runs_in_existing_host_gate():
    script = read("scripts/run_host_native_wake_word_lifecycle_test.sh")
    workflow = read(".github/workflows/build.yml")
    assert "tests/native/opus_encoder_serialization_test.cc" in script
    assert "main/audio/opus_encoder_serialization.cc" in script
    assert "tests/test_opus_encoder_serialization_contract.py" in workflow
