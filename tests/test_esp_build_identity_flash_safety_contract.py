from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_build_identity_is_preloaded_before_psram_websocket_workers_start():
    header = read("main/esp_build_identity.h")
    identity = read("main/esp_build_identity.cc")
    application = read("main/application.cc")

    assert "PreloadRunningEspBuildIdentity" in header

    initialize = application[
        application.index("void Application::Initialize()") :
        application.index("void Application::Run()")
    ]
    assert initialize.index("PreloadRunningEspBuildIdentity") < initialize.index(
        "audio_service_.Start()"
    )

    read_identity = identity[identity.index("bool ReadRunningEspBuildIdentity") :]
    assert "esp_partition_get_sha256" not in read_identity
    assert "build_identity_not_preloaded" in read_identity


def test_course_mode_local_endpoint_identity_is_lab_only_and_precedes_production():
    identity = read("main/esp_build_identity.cc")
    local_flag = "CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT"
    local_profile = 'TBOT_EMBEDDED_PROFILE "course-mode-task07-local-endpoint"'

    assert f"defined({local_flag}) && {local_flag}" in identity
    assert local_profile in identity
    identity_selection = identity[identity.index(f"#if defined({local_flag})") :]
    assert identity_selection.index(local_flag) < identity_selection.index(
        "CONFIG_TBOT_HIL_STORAGE_FAULTS"
    )
    local_branch = identity_selection[: identity_selection.index("#else")]
    assert 'TBOT_EMBEDDED_PROFILE "production"' not in local_branch
