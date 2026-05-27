from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

OTA_URL = "https://luggage-spears-louisville-psychology.trycloudflare.com/tbot/ota/"
WS_URL = "wss://perform-elvis-specifically-nominated.trycloudflare.com/tbot/v1/"


def test_firmware_defaults_point_to_current_tbot_endpoints():
    kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
    local_defaults = (ROOT / "sdkconfig.defaults.local").read_text(encoding="utf-8")

    assert f'default "{OTA_URL}"' in kconfig
    assert f'CONFIG_OTA_URL="{OTA_URL}"' in local_defaults
    assert f'default "{WS_URL}"' in kconfig
    assert f'CONFIG_WEBSOCKET_URL="{WS_URL}"' in local_defaults


def test_websocket_protocol_uses_compile_time_fallback_when_nvs_missing():
    websocket_protocol = (
        ROOT / "main" / "protocols" / "websocket_protocol.cc"
    ).read_text(encoding="utf-8")

    assert 'settings.GetString("url", CONFIG_WEBSOCKET_URL)' in websocket_protocol
