from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

OTA_URL = "https://tbot-backend-8wmh.onrender.com/tbot/ota/"
PROVISIONING_STATUS_URL = "https://tbot-backend-8wmh.onrender.com/v1/device/provisioning/status"


def assert_no_ephemeral_endpoint(contents: str, source: str) -> None:
    assert "trycloudflare.com" not in contents, source
    assert "ngrok" not in contents.lower(), source
    assert "your-ip-or-domain" not in contents, source


def test_firmware_defaults_use_stable_ota_seed_and_no_ephemeral_ws_seed():
    kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
    local_defaults = (ROOT / "sdkconfig.defaults.local").read_text(encoding="utf-8")
    flash_instructions = (ROOT / "FLASH_INSTRUCTIONS.md").read_text(encoding="utf-8")
    ota_probe = (ROOT / "tools" / "probe_ota_activation.py").read_text(encoding="utf-8")

    assert_no_ephemeral_endpoint(kconfig, "main/Kconfig.projbuild")
    assert_no_ephemeral_endpoint(local_defaults, "sdkconfig.defaults.local")
    assert_no_ephemeral_endpoint(flash_instructions, "FLASH_INSTRUCTIONS.md")
    assert_no_ephemeral_endpoint(ota_probe, "tools/probe_ota_activation.py")
    assert f'default "{OTA_URL}"' in kconfig
    assert f'CONFIG_OTA_URL="{OTA_URL}"' in local_defaults
    assert f'CONFIG_PROVISIONING_STATUS_URL="{PROVISIONING_STATUS_URL}"' in local_defaults
    assert 'default ""' in kconfig
    assert 'CONFIG_WEBSOCKET_URL=""' in local_defaults


def test_local_firmware_configs_do_not_override_stable_seed_with_ephemeral_urls():
    for sdkconfig_name in ("sdkconfig.defaults.local",):
        contents = (ROOT / sdkconfig_name).read_text(encoding="utf-8")
        assert_no_ephemeral_endpoint(contents, sdkconfig_name)
        assert f'CONFIG_OTA_URL="{OTA_URL}"' in contents, sdkconfig_name
        assert 'CONFIG_WEBSOCKET_URL=""' in contents, sdkconfig_name


def test_websocket_protocol_uses_compile_time_fallback_when_nvs_missing():
    websocket_protocol = (
        ROOT / "main" / "protocols" / "websocket_protocol.cc"
    ).read_text(encoding="utf-8")

    assert 'settings.GetString("url", CONFIG_WEBSOCKET_URL)' in websocket_protocol
