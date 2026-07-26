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
