"""Contract tests for robot-originated cloud factory reset.

These source-scrape checks lock the ownership-release path that matters for
physical re-pairing: a claimed robot must call the backend with its stored
device_secret before local NVS is erased, otherwise the backend stays assigned
to the old household and the next phone claim fails with DEVICE_ALREADY_OWNED.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def test_factory_reset_releases_cloud_ownership_before_erasing_local_credentials():
    source = read("main/boards/common/system_reset.cc")
    body = function_body(source, "void SystemReset::CheckButtons")

    assert "ReleaseCloudOwnership()" in body
    assert "ResetNvsFlash();" in body
    assert body.index("ReleaseCloudOwnership()") < body.index("ResetNvsFlash();")
    assert "if (ReleaseCloudOwnership())" in body
    assert "Cloud ownership release failed; keeping NVS credentials" in body


def test_factory_reset_posts_device_token_to_backend_release_route():
    source = read("main/boards/common/system_reset.cc")
    body = function_body(source, "bool SystemReset::ReleaseCloudOwnership")

    assert 'backend_settings.GetString("api_url")' in body
    assert 'backend_settings.GetString("device_secret")' in body
    assert 'Board::GetInstance().GetUuid()' in body
    assert 'http->SetHeader("X-Device-Token", device_secret)' in body
    assert 'http->SetHeader("X-Device-Id", device_id)' in body
    assert 'http->Open("POST", url)' in body
    assert 'http->SetTimeout(5000);' in body
    assert body.index('http->SetHeader("X-Device-Token", device_secret)') < body.index('http->Open("POST", url)')

    url_body = function_body(source, "static std::string BuildFactoryResetUrl")
    assert '"/devices/" + UrlEncodeQueryParam(device_id) + "/factory-reset"' in url_body
