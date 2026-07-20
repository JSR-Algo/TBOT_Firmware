"""Privacy-safe heap probes around the lesson queue ownership boundaries."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_lesson_enqueue_probes_bracket_serialization_and_reuse_exact_payload_size():
    source = read("main/application.cc")
    enqueue = function_body(source, "void Application::EnqueueLessonMessage")

    before = enqueue.index('LogLessonHeapBoundary("enqueue.before_serialize", 0);')
    serialize = enqueue.index("char* payload = cJSON_PrintUnformatted(root);", before)
    size = enqueue.index(
        "const size_t payload_bytes = payload != nullptr ? strlen(payload) : 0;",
        serialize,
    )
    after = enqueue.index(
        'LogLessonHeapBoundary("enqueue.after_serialize", payload_bytes);', size
    )
    null_guard = enqueue.index("if (payload == nullptr)", after)
    queue = enqueue.index("xQueueSend(lesson_message_queue_, &payload, 0)", null_guard)

    assert before < serialize < size < after < null_guard < queue
    assert "strlen(payload)" not in enqueue[queue:]


def test_lesson_worker_probes_parse_handler_and_delete_in_ownership_order():
    source = read("main/application.cc")
    worker = function_body(source, "void Application::LessonMessageTask")

    size = worker.index("const size_t payload_bytes = strlen(payload);")
    before_parse = worker.index(
        'LogLessonHeapBoundary("worker.before_parse", payload_bytes);', size
    )
    parse = worker.index("cJSON* root = cJSON_Parse(payload);", before_parse)
    after_parse = worker.index(
        'LogLessonHeapBoundary("worker.after_parse", payload_bytes);', parse
    )
    handle = worker.index("self->HandleLessonMessage(root);", after_parse)
    after_handle = worker.index(
        'LogLessonHeapBoundary("worker.after_handle", payload_bytes);', handle
    )
    delete = worker.index("cJSON_Delete(root);", after_handle)
    after_delete = worker.index(
        'LogLessonHeapBoundary("worker.after_delete", payload_bytes);', delete
    )
    free_payload = worker.index("cJSON_free(payload);", after_delete)
    clear_payload = worker.index("payload = nullptr;", free_payload)
    after_payload_free = worker.index(
        'LogLessonHeapBoundary("worker.after_payload_free", payload_bytes);',
        clear_payload,
    )
    parse_failure = worker.index('ESP_LOGW(TAG, "lesson_* dropped: worker parse failed");')

    assert (
        size
        < before_parse
        < parse
        < after_parse
        < handle
        < after_handle
        < delete
        < after_delete
        < free_payload
        < clear_payload
        < after_payload_free
    )
    assert parse_failure < free_payload < after_payload_free


def test_heap_probe_is_all_numeric_except_for_compile_time_phase_label():
    source = read("main/lesson_heap_probe.cc")
    body = function_body(source, "void LogLessonHeapBoundary")

    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in body
    assert "heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)" in body
    assert re.search(
        r'"lesson_heap phase=%s payload_bytes=%u internal_free=%u "\s*'
        r'"lifetime_min_internal=%u"',
        body,
    )
    assert "std::string" not in body
    assert "cJSON" not in body
    assert not re.search(r"\b(?:malloc|calloc|realloc)\s*\(", body)

    log_format = re.search(r'ESP_LOGI\([^;]+;', body, re.DOTALL)
    assert log_format is not None
    statement = log_format.group(0)
    assert statement.count("%s") == 1
    assert statement.count("%u") == 3
    for forbidden in (
        "assignment",
        "session",
        "transcript",
        "caption",
        "url",
        "payload.c_str",
        "root",
    ):
        assert forbidden not in statement.lower()


def test_heap_probe_call_sites_use_only_fixed_phase_labels_and_numeric_values():
    source = read("main/application.cc")
    calls = re.findall(r"LogLessonHeapBoundary\(([^;]+)\);", source)

    assert len(calls) == 7
    expected = {
        '"enqueue.before_serialize", 0',
        '"enqueue.after_serialize", payload_bytes',
        '"worker.before_parse", payload_bytes',
        '"worker.after_parse", payload_bytes',
        '"worker.after_handle", payload_bytes',
        '"worker.after_delete", payload_bytes',
        '"worker.after_payload_free", payload_bytes',
    }
    assert {re.sub(r"\s+", " ", call).strip() for call in calls} == expected


def test_heap_probe_is_registered_in_firmware_and_host_gate():
    cmake = read("main/CMakeLists.txt")
    script = read("scripts/run_host_native_lesson_heap_probe_test.sh")
    workflow = read(".github/workflows/build.yml")

    assert '"lesson_heap_probe.cc"' in cmake
    assert "tests/native/lesson_heap_probe_host_test.cc" in script
    assert "main/lesson_heap_probe.cc" in script
    assert "scripts/run_host_native_lesson_heap_probe_test.sh" in workflow
    assert "tests/test_lesson_heap_boundary_contract.py" in workflow
