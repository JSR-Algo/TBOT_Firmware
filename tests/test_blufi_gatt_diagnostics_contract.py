"""Contracts for the temporary BluFi GATT diagnostic build overlay."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "sdkconfig.defaults.blufi-gatt-diagnostics"


def test_diagnostic_overlay_enables_only_required_debug_trace_layers():
    config = OVERLAY.read_text(encoding="utf-8")

    required = {
        "CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y",
        "CONFIG_LOG_MAXIMUM_LEVEL=4",
        "CONFIG_BT_LOG_GATT_TRACE_LEVEL_DEBUG=y",
        "CONFIG_BT_LOG_GATT_TRACE_LEVEL=5",
        "CONFIG_BT_LOG_BTC_TRACE_LEVEL_DEBUG=y",
        "CONFIG_BT_LOG_BTC_TRACE_LEVEL=5",
        "CONFIG_BT_LOG_BLUFI_TRACE_LEVEL_DEBUG=y",
        "CONFIG_BT_LOG_BLUFI_TRACE_LEVEL=5",
    }
    assert required <= set(config.splitlines())


def test_diagnostic_overlay_is_not_part_of_the_default_production_build():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "sdkconfig.defaults.blufi-gatt-diagnostics" not in cmake


def test_diagnostic_overlay_contains_no_credential_or_network_values():
    config = OVERLAY.read_text(encoding="utf-8").lower()
    for forbidden in ("ssid", "password", "passwd", "token", "secret"):
        assert forbidden not in config
