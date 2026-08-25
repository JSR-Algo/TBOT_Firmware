"""Periodic system metrics expose null-safe audio task stack high-water marks."""

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


def enclosing_scoped_block(text: str, marker: str) -> str:
    marker_index = text.index(marker)
    stack = []
    for index, char in enumerate(text[:marker_index]):
        if char == "{":
            stack.append(index)
        elif char == "}":
            stack.pop()
    assert stack, f"marker is not inside a brace scope: {marker}"

    block_start = stack[-1]
    depth = 0
    for index in range(block_start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[block_start : index + 1]
    raise AssertionError(f"unterminated scope containing {marker}")


def test_audio_service_exposes_null_safe_stack_high_water_snapshot():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")
    getter = function_body(source, "AudioTaskStackHighWaterMarks AudioService::GetTaskStackHighWaterMarks")

    assert "struct AudioTaskStackHighWaterMarks" in header
    for field in (
        "audio_input = -1",
        "audio_output = -1",
        "opus_codec = -1",
        "afe_detection = -1",
    ):
        assert field in header
    assert "AudioTaskStackHighWaterMarks GetTaskStackHighWaterMarks();" in header
    assert "std::lock_guard<std::mutex> lock(task_handle_mutex_)" in getter
    assert "handle == nullptr ? -1" in getter
    assert getter.count("uxTaskGetStackHighWaterMark(handle)") == 1
    assert "audio_input_task_handle_" in getter
    assert "audio_output_task_handle_" in getter
    assert "opus_codec_task_handle_" in getter
    assert "GetDetectionTaskStackHighWaterMark()" in getter


def test_audio_tasks_clear_handles_before_self_delete_and_afe_is_optional():
    source = read("main/audio/audio_service.cc")
    wake_header = read("main/audio/wake_word.h")
    afe_header = read("main/audio/wake_words/afe_wake_word.h")
    afe_source = read("main/audio/wake_words/afe_wake_word.cc")

    for handle in (
        "audio_input_task_handle_",
        "audio_output_task_handle_",
        "opus_codec_task_handle_",
    ):
        clear = source.index(f"{handle} = nullptr")
        delete = source.index("vTaskDelete(NULL)", clear)
        assert clear < delete
    assert "virtual int32_t GetDetectionTaskStackHighWaterMark() const { return -1; }" in wake_header
    assert "int32_t GetDetectionTaskStackHighWaterMark() const override;" in afe_header
    afe_getter = function_body(
        afe_source, "int32_t AfeWakeWord::GetDetectionTaskStackHighWaterMark"
    )
    getter_guard = afe_getter.index(
        "auto lifecycle_lock = run_synchronization_.AcquireTransition()"
    )
    getter_load = afe_getter.index(
        "audio_detection_task_handle_.load(std::memory_order_acquire)"
    )
    getter_watermark = afe_getter.index("uxTaskGetStackHighWaterMark(task_handle)")
    assert getter_guard < getter_load < getter_watermark
    getter_guard_scope = enclosing_scoped_block(
        afe_getter, "auto lifecycle_lock = run_synchronization_.AcquireTransition()"
    )
    assert "audio_detection_task_handle_.load(std::memory_order_acquire)" in getter_guard_scope
    assert "uxTaskGetStackHighWaterMark(task_handle)" in getter_guard_scope
    assert "audio_detection_task_handle_.load(std::memory_order_acquire)" in afe_getter
    assert "task_handle == nullptr" in afe_getter
    assert "uxTaskGetStackHighWaterMark(task_handle)" in afe_getter
    assert afe_getter.index("task_handle == nullptr") < afe_getter.index(
        "uxTaskGetStackHighWaterMark(task_handle)"
    )

    detection_task = afe_source[
        afe_source.index("const BaseType_t detection_created") :
        afe_source.index('"audio_detection"')
    ]
    exit_guard = detection_task.index(
        "auto lifecycle_lock = this_->run_synchronization_.AcquireTransition()"
    )
    clear = detection_task.index(
        "audio_detection_task_handle_.store(nullptr, std::memory_order_release)"
    )
    exit_ack = detection_task.index("xEventGroupSetBits(exit_events, DETECTION_EXITED_EVENT)")
    assert exit_guard < clear < exit_ack
    exit_guard_scope = enclosing_scoped_block(
        detection_task,
        "auto lifecycle_lock = this_->run_synchronization_.AcquireTransition()",
    )
    assert "audio_detection_task_handle_.store(nullptr, std::memory_order_release)" in exit_guard_scope
    assert "xEventGroupSetBits(exit_events, DETECTION_EXITED_EVENT)" not in exit_guard_scope


def test_periodic_sys_metrics_emits_stable_audio_stack_fields():
    application = read("main/application.cc")
    assert "auto stack_hwm = audio_service_.GetTaskStackHighWaterMarks();" in application
    log_start = application.index('ESP_LOGI(TAG, "sys_metrics stack_main_min=')
    log_end = application.index(");", log_start)
    log = application[log_start:log_end]
    for field in (
        "stack_main_min=%u",
        "stack_audio_input_min=%ld",
        "stack_audio_output_min=%ld",
        "stack_opus_codec_min=%ld",
        "stack_afe_detection_min=%ld",
        "psram_free_b=%u",
    ):
        assert field in log


def test_opus_worker_stack_uses_the_measured_named_budget():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")

    assert "former 24 KiB allocation reported 16.9 KiB unused" in header
    assert "kOpusCodecTaskStackBytes = 12 * 1024" in header
    assert '"opus_codec", kOpusCodecTaskStackBytes' in source
    assert "2048 * 12" not in source
