"""Regression locks for lesson HTTPS asset fetch network headroom.

The lesson renderer keeps an idle WebSocket open while fetching HTTPS poster and
teaching-object images. On ESP32-S3 this exercises the LWIP tcp/ip receive task
through TLS and image downloads, so the task needs more stack than the older
3072-byte default.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIN_TCPIP_STACK_BYTES = 6144
MIN_HEARTBEAT_STACK_BYTES = 8192
MIN_WEBSOCKET_OPEN_STACK_BYTES = 8192


def _read_config_values(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, raw_value = line.split("=", 1)
        try:
            values[key] = int(raw_value.strip().strip('"'))
        except ValueError:
            continue
    return values


def test_lesson_https_fetch_has_tcpip_task_stack_headroom():
    defaults = _read_config_values(ROOT / "sdkconfig.defaults.local")

    assert defaults["CONFIG_LWIP_TCPIP_TASK_STACK_SIZE"] >= MIN_TCPIP_STACK_BYTES
    assert defaults["CONFIG_TCPIP_TASK_STACK_SIZE"] >= MIN_TCPIP_STACK_BYTES


def test_heartbeat_https_worker_has_stack_headroom_next_to_passive_websocket():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")

    assert f'"heartbeat_http", {MIN_HEARTBEAT_STACK_BYTES}' in source

def test_websocket_open_worker_has_tls_stack_headroom():
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")

    assert f"kOpenChannelWorkerStackDepth = {MIN_WEBSOCKET_OPEN_STACK_BYTES}" in source
    assert "xTaskCreateWithCaps(" in source
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in source
    assert "open_channel_task_stack" not in source
