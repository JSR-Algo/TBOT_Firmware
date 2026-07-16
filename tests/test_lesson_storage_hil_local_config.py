from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts/generate_lesson_storage_hil_local_config.py"


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


def test_local_config_generator_writes_exact_atomic_hil_overlay(tmp_path):
    result, output = run_generator(
        tmp_path,
        ota_url="http://192.168.100.209:8003/tbot/ota/",
        websocket_url="ws://192.168.100.209:8000/tbot/v1/",
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert output.read_text(encoding="utf-8") == (
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
        ("http://example.com/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://8.8.8.8/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "http://192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://user:pass@192.168.100.209:8000/tbot/v1/"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://192.168.100.209:8000/tbot/v1/?token=secret"),
        ("http://192.168.100.209:8003/tbot/ota/", "ws://example.com/tbot/v1/"),
    ],
)
def test_local_config_generator_rejects_unsafe_urls(tmp_path, ota_url, websocket_url):
    result, output = run_generator(tmp_path, ota_url=ota_url, websocket_url=websocket_url)

    assert result.returncode != 0
    assert not output.exists()
    assert "user:pass" not in result.stdout + result.stderr
    assert "token=secret" not in result.stdout + result.stderr


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
        ota_url="https://10.0.0.5:8443/tbot/ota/",
        websocket_url="wss://172.16.4.8:8443/tbot/v1/",
        output=output,
    )

    assert result.returncode == 0, result.stderr
    assert output.read_text(encoding="utf-8").splitlines() == [
        'CONFIG_OTA_URL="https://10.0.0.5:8443/tbot/ota/"',
        'CONFIG_WEBSOCKET_URL="wss://172.16.4.8:8443/tbot/v1/"',
    ]
    assert output.stat().st_mode & 0o777 == 0o600
