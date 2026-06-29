from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# Current production OTA/bootstrap seed. This is a temporary Cloudflare tunnel
# until the ESP host is stabilized; the WebSocket host is intentionally not
# derived from this because tunnels are split by role.
OTA_URL = "https://carefully-freelance-improving-numerical.trycloudflare.com/tbot/ota/"
PROVISIONING_STATUS_URL = "https://tbot-backend-8wmh.onrender.com/v1/device/provisioning/status"
WS_URL = "wss://freebsd-concern-noon-cement.trycloudflare.com/tbot/v1/"


def test_firmware_defaults_point_to_current_tbot_endpoints():
    kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
    local_defaults = (ROOT / "sdkconfig.defaults.local").read_text(encoding="utf-8")

    assert f'default "{OTA_URL}"' in kconfig
    assert f'CONFIG_OTA_URL="{OTA_URL}"' in local_defaults
    assert f'CONFIG_PROVISIONING_STATUS_URL="{PROVISIONING_STATUS_URL}"' in local_defaults
    assert f'default "{WS_URL}"' in kconfig
    assert f'CONFIG_WEBSOCKET_URL="{WS_URL}"' in local_defaults


def test_local_firmware_configs_do_not_override_current_ota_seed():
    for sdkconfig_name in ("sdkconfig", "sdkconfig.blufi"):
        contents = (ROOT / sdkconfig_name).read_text(encoding="utf-8")
        assert f'CONFIG_OTA_URL="{OTA_URL}"' in contents, sdkconfig_name
        assert f'CONFIG_WEBSOCKET_URL="{WS_URL}"' in contents, sdkconfig_name


def test_websocket_protocol_uses_compile_time_fallback_when_nvs_missing():
    websocket_protocol = (
        ROOT / "main" / "protocols" / "websocket_protocol.cc"
    ).read_text(encoding="utf-8")

    assert 'settings.GetString("url", CONFIG_WEBSOCKET_URL)' in websocket_protocol
