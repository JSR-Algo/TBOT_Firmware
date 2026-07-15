from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/boards/common/blufi.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "main/boards/common/blufi.h").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = SOURCE.index(signature)
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[brace:index + 1]
    raise AssertionError(signature)


def test_deinit_tracks_host_and_controller_cleanup_independently():
    body = function_body("esp_err_t Blufi::deinit()")
    assert "bool host_active_" in HEADER
    assert "bool controller_active_" in HEADER
    assert "if (host_active_)" in body
    assert "if (controller_active_)" in body
    assert "host_active_ = false" in body
    assert "controller_active_ = false" in body


def test_host_failure_is_not_overwritten_by_controller_success():
    body = function_body("esp_err_t Blufi::deinit()")
    host = body.index("_host_deinit()")
    controller = body.index("_controller_deinit()", host)
    returned = body.rindex("return first_error;")
    assert "CaptureFirstError(first_error" in body[host:controller]
    assert "CaptureFirstError(first_error" in body[controller:returned]


def test_partial_failure_remains_retryable_and_never_sets_stale_success_flag():
    body = function_body("esp_err_t Blufi::deinit()")
    mark = body.index("m_deinited = true")
    assert body.index("if (host_active_)") < mark
    assert body.index("if (controller_active_)") < mark
    completion_guard = body[body.rfind("if (", 0, mark):mark]
    assert "!host_active_" in completion_guard
    assert "!controller_active_" in completion_guard
    assert "first_error == ESP_OK" in completion_guard


def test_controller_teardown_preserves_disable_failure():
    body = function_body("esp_err_t Blufi::_controller_deinit()")
    assert "first_error" in body
    assert body.count("CaptureFirstError(first_error") >= 2
    assert "return first_error;" in body


def test_full_success_is_the_only_path_that_marks_deinitialized():
    body = function_body("esp_err_t Blufi::deinit()")
    mark = body.index("m_deinited = true")
    completion_guard = body[body.rfind("if (", 0, mark):mark]
    assert "first_error == ESP_OK" in completion_guard
    assert "!host_active_" in completion_guard
    assert "!controller_active_" in completion_guard
    assert body.index("inited_ = false", mark) > mark


def test_init_cannot_overwrite_an_unfinished_cleanup_transaction():
    body = function_body("esp_err_t Blufi::init()")
    guard = body.index("if (host_active_ || controller_active_)")
    reset = body.index("m_deinited = false")
    assert guard < reset
    assert "return ESP_ERR_INVALID_STATE;" in body[guard:reset]
