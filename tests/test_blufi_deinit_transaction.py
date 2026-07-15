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


def function_body_after(signature: str, offset: int) -> str:
    start = SOURCE.index(signature, offset)
    return function_body_at(start)


def function_body_at(start: int) -> str:
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[brace:index + 1]
    raise AssertionError(start)


def test_deinit_tracks_host_and_controller_cleanup_independently():
    body = function_body("esp_err_t Blufi::deinit()")
    assert "bool host_active_" in HEADER
    assert "bool controller_active_" in HEADER
    for state in (
        "profile_active_",
        "host_enabled_",
        "host_initialized_",
        "controller_enabled_",
        "controller_initialized_",
    ):
        assert f"bool {state}" in HEADER
    assert "if (host_active_)" in body
    assert "if (controller_active_)" in body


def test_host_failure_prevents_controller_teardown():
    body = function_body("esp_err_t Blufi::deinit()")
    host = body.index("_host_deinit()")
    controller = body.index("_controller_deinit()", host)
    between = body[host:controller]
    assert "if (host_error != ESP_OK || host_active_)" in between
    assert "CaptureFirstError(first_error, ESP_ERR_INVALID_STATE)" in between
    assert "return first_error;" in between


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
    assert "if (controller_enabled_)" in body
    assert "controller_enabled_ = false" in body
    assert "if (controller_initialized_)" in body
    assert "controller_initialized_ = false" in body
    disable = body.index("esp_bt_controller_disable()")
    deinit = body.index("esp_bt_controller_deinit()")
    assert "return ret;" in body[disable:deinit]


def test_bluedroid_retry_skips_completed_host_substeps():
    body = function_body("esp_err_t Blufi::_host_deinit()")
    required = ("profile_active_", "host_enabled_", "host_initialized_")
    for state in required:
        assert f"if ({state})" in body
        assert f"{state} = false" in body
    profile = body.index("esp_blufi_profile_deinit()")
    disable = body.index("esp_bluedroid_disable()")
    deinit = body.index("esp_bluedroid_deinit()")
    assert profile < disable < deinit
    assert "return ret;" in body[profile:disable]
    assert "return ret;" in body[disable:deinit]


def test_nimble_retry_tracks_profile_stop_and_deinit_separately():
    nimble = SOURCE.index("#ifdef CONFIG_BT_NIMBLE_ENABLED")
    body = function_body_after("esp_err_t Blufi::_host_deinit(void)", nimble)
    profile = body.index("esp_blufi_profile_deinit()")
    stop = body.index("nimble_port_stop()")
    deinit = body.index("esp_nimble_deinit()")
    assert profile < stop < deinit
    assert "profile_active_ = false" in body
    assert "host_enabled_ = false" in body
    assert "host_initialized_ = false" in body
    assert "return ret;" in body[profile:stop]
    assert "return ret;" in body[stop:deinit]


def test_full_success_is_the_only_path_that_marks_deinitialized():
    body = function_body("esp_err_t Blufi::deinit()")
    mark = body.index("m_deinited = true")
    completion_guard = body[body.rfind("if (", 0, mark):mark]
    assert "first_error == ESP_OK" in completion_guard
    assert "!host_active_" in completion_guard
    assert "!controller_active_" in completion_guard
    assert body.index("inited_ = false", mark) > mark
    controller = body.index("_controller_deinit()")
    assert "CaptureFirstError(first_error, ESP_ERR_INVALID_STATE)" in body[controller:mark]


def test_init_cannot_overwrite_an_unfinished_cleanup_transaction():
    body = function_body("esp_err_t Blufi::init()")
    guard = body.index("if (host_active_ || controller_active_)")
    reset = body.index("m_deinited = false")
    assert guard < reset
    assert "return ESP_ERR_INVALID_STATE;" in body[guard:reset]
