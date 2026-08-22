from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts/generate_lesson_storage_hil_local_config.py"
KCONFIG = ROOT / "main/Kconfig.projbuild"


def run_generator(tmp_path: Path, *, ota_url: str, websocket_url: str, output: Path | None = None):
    output = output or tmp_path / "sdkconfig.defaults.hil-local"
    result = subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--ota-url",
            ota_url,
            "--websocket-url",
            websocket_url,
            "--output",
            str(output),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    return result, output


def run_validator(*, ota_url: str, websocket_url: str):
    return subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--ota-url",
            ota_url,
            "--websocket-url",
            websocket_url,
            "--validate-only",
        ],
        text=True,
        capture_output=True,
        check=False,
    )


def test_course_mode_local_endpoint_kconfig_is_default_off():
    source = KCONFIG.read_text(encoding="utf-8")
    block = source[
        source.index("config TBOT_COURSE_MODE_LOCAL_ENDPOINT") :
        source.index("config TBOT_LESSON_ASSET_MAX_FILE_BYTES")
    ]

    assert 'bool "Enable Task 07 private-LAN endpoint mode"' in block
    assert "default n" in block
    assert "CONFIG_OTA_URL" in block
    assert "CONFIG_WEBSOCKET_URL" in block


def test_course_mode_build_configuration_reuses_strict_endpoint_validation():
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")

    assert "if(CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT)" in cmake
    assert "generate_lesson_storage_hil_local_config.py" in cmake
    assert "--validate-only" in cmake
    assert "message(FATAL_ERROR" in cmake

    valid = run_validator(
        ota_url="http://192.168.100.209:8003/tbot/ota/",
        websocket_url="ws://[fd12:3456::7]:8000/tbot/v1/",
    )
    invalid = run_validator(
        ota_url="https://192.168.100.209/tbot/ota/",
        websocket_url="ws://192.168.100.209/tbot/v1/",
    )
    assert valid.returncode == 0, valid.stderr
    assert valid.stdout == ""
    assert invalid.returncode != 0


def test_local_config_generator_writes_exact_atomic_hil_overlay(tmp_path):
    result, output = run_generator(
        tmp_path,
        ota_url="http://192.168.100.209:8003/tbot/ota/",
        websocket_url="ws://192.168.100.209:8000/tbot/v1/",
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert output.read_text(encoding="utf-8") == (
        "CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT=y\n"
        'CONFIG_OTA_URL="http://192.168.100.209:8003/tbot/ota/"\n'
        'CONFIG_WEBSOCKET_URL="ws://192.168.100.209:8000/tbot/v1/"\n'
    )
    assert not list(tmp_path.glob(f".{output.name}.*"))


@pytest.mark.parametrize(
    ("ota_url", "websocket_url"),
    [
        ("http://user:pass@192.168.100.209:8003/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/?token=secret", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/#frag", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot\\ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/\nleak", "ws://192.168.100.209:8000/tbot/v1/"),
        ("ftp://192.168.100.209/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("https://192.168.100.209/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://example.com/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://8.8.8.8/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://127.0.0.1/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://169.254.1.2/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:0/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "http://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "wss://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://user:pass@192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/?token=secret"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/#frag"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://example.com/tbot/v1/"),
        ("http://[fe80::1]/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
    ],
)
def test_local_config_generator_rejects_unsafe_urls(tmp_path, ota_url, websocket_url):
    result, output = run_generator(tmp_path, ota_url=ota_url, websocket_url=websocket_url)

    assert result.returncode != 0
    assert not output.exists()
    assert "user:pass" not in result.stdout + result.stderr
    assert "token=secret" not in result.stdout + result.stderr


@pytest.mark.parametrize("control", ["\u0080", "\u0085", "\u009f"])
@pytest.mark.parametrize("field", ["ota", "websocket"])
def test_local_config_generator_rejects_c1_controls_without_leaking_input(tmp_path, field, control):
    marker = "do-not-print-c1-input"
    ota_url = "http://192.168.100.209:8003/tbot/ota/"
    websocket_url = "ws://192.168.100.209:8000/tbot/v1/"
    if field == "ota":
        ota_url += control + marker
    else:
        websocket_url += control + marker

    result, output = run_generator(tmp_path, ota_url=ota_url, websocket_url=websocket_url)

    assert result.returncode != 0
    assert not output.exists()
    assert marker not in result.stdout + result.stderr


@pytest.mark.parametrize("character", ["\u202e", "é"])
@pytest.mark.parametrize("field", ["ota", "websocket"])
def test_local_config_generator_rejects_non_ascii_without_leaking_input(tmp_path, field, character):
    marker = "do-not-print-non-ascii-input"
    ota_url = "http://192.168.100.209:8003/tbot/ota/"
    websocket_url = "ws://192.168.100.209:8000/tbot/v1/"
    if field == "ota":
        ota_url += character + marker
    else:
        websocket_url += character + marker

    result, output = run_generator(tmp_path, ota_url=ota_url, websocket_url=websocket_url)

    assert result.returncode != 0
    assert not output.exists()
    assert marker not in result.stdout + result.stderr


def test_local_config_generator_rejects_invalid_port_without_traceback(tmp_path):
    result, output = run_generator(
        tmp_path,
        ota_url="http://192.168.100.209:99999/tbot/ota/",
        websocket_url="ws://192.168.100.209:8000/tbot/v1/",
    )

    assert result.returncode != 0
    assert "Traceback" not in result.stderr
    assert not output.exists()


def test_local_config_generator_preserves_existing_output_on_validation_failure(tmp_path):
    output = tmp_path / "sdkconfig.defaults.hil-local"
    output.write_text("existing\n", encoding="utf-8")

    result, _ = run_generator(
        tmp_path,
        ota_url="https://public.example/tbot/ota/",
        websocket_url="wss://public.example/tbot/v1/",
        output=output,
    )

    assert result.returncode != 0
    assert output.read_text(encoding="utf-8") == "existing\n"


def test_local_config_generator_atomically_replaces_existing_output(tmp_path):
    output = tmp_path / "sdkconfig.defaults.hil-local"
    output.write_text("old\n", encoding="utf-8")
    os.chmod(output, 0o600)

    result, _ = run_generator(
        tmp_path,
        ota_url="http://10.0.0.5:8443/tbot/ota/",
        websocket_url="ws://172.16.4.8:8443/tbot/v1/",
        output=output,
    )

    assert result.returncode == 0, result.stderr
    assert output.read_text(encoding="utf-8").splitlines() == [
        "CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT=y",
        'CONFIG_OTA_URL="http://10.0.0.5:8443/tbot/ota/"',
        'CONFIG_WEBSOCKET_URL="ws://172.16.4.8:8443/tbot/v1/"',
    ]
    assert output.stat().st_mode & 0o777 == 0o600


def test_local_config_generator_accepts_private_ipv4_and_ula_ipv6_literals(tmp_path):
    cases = [
        ("http://10.0.0.5/tbot/ota/", "ws://172.31.255.254/tbot/v1/"),
        ("http://192.168.1.5/tbot/ota/", "ws://[fd12:3456::7]:8000/tbot/v1/"),
    ]

    for index, (ota_url, websocket_url) in enumerate(cases):
        output = tmp_path / f"sdkconfig.defaults.{index}"
        result, _ = run_generator(
            tmp_path,
            ota_url=ota_url,
            websocket_url=websocket_url,
            output=output,
        )

        assert result.returncode == 0, result.stderr
        assert f'CONFIG_OTA_URL="{ota_url}"' in output.read_text(encoding="utf-8")
