import re
import json
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")

def lcdwiki_reference_sdkconfig() -> str:
    config = json.loads(read("main/boards/lcdwiki-es3c35p/config.json"))
    return "\n".join(
        [
            read("sdkconfig.defaults.esp32s3"),
            read("sdkconfig.defaults.local"),
            "\n".join(config["builds"][0]["sdkconfig_append"]),
        ]
    )

def has_define(text: str, name: str, value: str) -> bool:
    return re.search(rf"^#define\s+{re.escape(name)}\s+{re.escape(value)}\b", text, re.MULTILINE) is not None


def test_lcdwiki_es3c35p_board_is_selected_and_registered():
    kconfig = read("main/Kconfig.projbuild")
    cmake = read("main/CMakeLists.txt")
    local_defaults = read("sdkconfig.defaults.local")
    sdkconfig = lcdwiki_reference_sdkconfig()

    assert "config BOARD_TYPE_LCDWIKI_ES3C35P" in kconfig
    assert 'bool "LCDWiki ES3C35P ESP32-S3 3.5 LCD"' in kconfig
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P" in cmake
    assert 'set(BOARD_TYPE "lcdwiki-es3c35p")' in cmake
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y" in local_defaults
    assert "CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5=y" not in local_defaults
    assert "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" in sdkconfig
    assert "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set" in sdkconfig
    assert "CONFIG_ESP_CONSOLE_UART_NUM=-1" in sdkconfig


def test_lcdwiki_es3c35p_is_kconfig_fallback_board_for_plain_builds():
    kconfig = read("main/Kconfig.projbuild")
    board_choice = kconfig[kconfig.index("choice BOARD_TYPE") : kconfig.index("endchoice", kconfig.index("choice BOARD_TYPE"))]

    assert re.search(r"^\s*default BOARD_TYPE_LCDWIKI_ES3C35P$", board_choice, re.MULTILINE)
    assert not re.search(r"^\s*default BOARD_TYPE_BREAD_COMPACT_WIFI$", board_choice, re.MULTILINE)


def test_lcdwiki_es3c35p_fleet_build_script_loads_local_board_overlay_and_hard_gates_flash():
    root_cmake = read("CMakeLists.txt")
    script = read("build-lcdwiki.sh")
    flash_instructions = read("FLASH_INSTRUCTIONS.md")

    assert 'set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local")' in root_cmake
    assert 'export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local"' in script
    assert "rm -f sdkconfig" in script
    assert 'rm -rf "$PROJECT_DIR/build"' in script
    assert "idf.py set-target esp32s3" in script
    assert "idf.py -DIDF_TARGET=esp32s3 reconfigure" not in script
    assert "grep -q '^CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y$' sdkconfig" in script
    assert "BLACK-SCREEN" in script
    assert "idf.py -p \"$PORT\" flash" in script
    assert "./build-lcdwiki.sh" in flash_instructions
    assert "SDKCONFIG_DEFAULTS" in flash_instructions
    assert "idf.py build" in flash_instructions
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y" in flash_instructions
    assert 'IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"' in script
    assert 'esp-idf-v5.5.2/export.sh' not in script

def test_lcdwiki_es3c35p_fleet_build_script_removes_stale_build_dir_before_set_target():
    script = read("build-lcdwiki.sh")

    lock = script.index('mkdir "$BUILD_LOCK_DIR"')
    sdkconfig_clean = script.index("rm -f sdkconfig")
    build_clean = script.index('rm -rf "$PROJECT_DIR/build"')
    set_target = script.index("idf.py set-target esp32s3")

    assert lock < sdkconfig_clean < build_clean < set_target

def test_lcdwiki_es3c35p_fleet_build_script_does_not_retry_or_bypass_idf_build():
    script = read("build-lcdwiki.sh")

    assert "if ! idf.py build; then" not in script
    assert "retrying once" not in script
    assert 'ninja -C "$PROJECT_DIR/build" -j1 all' not in script
    assert "idf.py build" in script

def test_lcdwiki_es3c35p_ci_release_build_disables_hardware_aes_and_gates_config():
    config_json = read("main/boards/lcdwiki-es3c35p/config.json")
    release_py = read("scripts/release.py")
    gate_script = read("scripts/assert_lcdwiki_prod_config.py")

    assert "# CONFIG_MBEDTLS_HARDWARE_AES is not set" in config_json
    assert "assert_lcdwiki_prod_config.py" in release_py
    assert "lcdwiki-es3c35p" in release_py
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y" in gate_script
    assert "CONFIG_MBEDTLS_HARDWARE_AES=y" in gate_script
    assert "Hardware AES must stay disabled" in gate_script
    assert "CONFIG_FATFS_LFN_HEAP=y" in config_json
    assert "FATFS short-name-only mode must stay disabled" in gate_script

def test_lcdwiki_es3c35p_reference_sdkconfig_passes_prod_gate(tmp_path):
    sdkconfig = tmp_path / "sdkconfig.es3c35p"
    sdkconfig.write_text(lcdwiki_reference_sdkconfig(), encoding="utf-8")
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/assert_lcdwiki_prod_config.py"),
            str(sdkconfig),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr


def test_lcdwiki_enables_release_cinematic_evidence():
    sdkconfig = lcdwiki_reference_sdkconfig()

    assert "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y" in sdkconfig


def test_lcdwiki_direct_defaults_chain_enables_release_evidence_without_hil():
    base = read("sdkconfig.defaults")
    esp32s3 = read("sdkconfig.defaults.esp32s3")
    local = read("sdkconfig.defaults.local")
    direct_chain = "\n".join((base, esp32s3, local))
    kconfig = read("main/Kconfig.projbuild")

    assert local.count("CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y") == 1
    assert "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y" in direct_chain
    assert "CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=y" not in direct_chain
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS=y" not in direct_chain
    assert "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y" not in base
    assert "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y" not in esp32s3
    assert "depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LCDWIKI_ES3C35P" in kconfig


def test_lcdwiki_es3c35p_prod_gate_rejects_missing_release_cinematic_evidence(tmp_path):
    sdkconfig = tmp_path / "sdkconfig.es3c35p-no-release-evidence"
    sdkconfig.write_text(
        lcdwiki_reference_sdkconfig().replace(
            "CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y",
            "",
        ),
        encoding="utf-8",
    )
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/assert_lcdwiki_prod_config.py"),
            str(sdkconfig),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "Release cinematic evidence must be enabled" in result.stderr


def test_lcdwiki_es3c35p_prod_gate_rejects_hil_storage_profile(tmp_path):
    sdkconfig = tmp_path / "sdkconfig.es3c35p-hil"
    sdkconfig.write_text(
        lcdwiki_reference_sdkconfig() + "\nCONFIG_TBOT_HIL_STORAGE_FAULTS=y\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/assert_lcdwiki_prod_config.py"),
            str(sdkconfig),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "HIL storage faults must stay disabled" in result.stderr


def test_lcdwiki_es3c35p_prod_gate_rejects_cinematic_hil_telemetry(tmp_path):
    sdkconfig = tmp_path / "sdkconfig.es3c35p-cinematic-hil"
    sdkconfig.write_text(
        lcdwiki_reference_sdkconfig() + "\nCONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=y\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/assert_lcdwiki_prod_config.py"),
            str(sdkconfig),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "Cinematic HIL telemetry must stay disabled" in result.stderr

def test_release_sdkconfig_append_replaces_existing_values_for_ci_gate(tmp_path):
    spec = importlib.util.spec_from_file_location("release", ROOT / "scripts/release.py")
    release = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(release)

    sdkconfig = tmp_path / "sdkconfig"
    sdkconfig.write_text(
        "\n".join(
            [
                "CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y",
                "CONFIG_MBEDTLS_HARDWARE_AES=y",
                "CONFIG_OTHER_VALUE=y",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    release._rewrite_sdkconfig_with_appends(
        sdkconfig,
        [
            "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y",
            "# CONFIG_MBEDTLS_HARDWARE_AES is not set",
        ],
    )

    text = sdkconfig.read_text(encoding="utf-8")
    assert "CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI=y" not in text
    assert "CONFIG_MBEDTLS_HARDWARE_AES=y" not in text
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y" in text
    assert "# CONFIG_MBEDTLS_HARDWARE_AES is not set" in text
    assert "CONFIG_OTHER_VALUE=y" in text

def test_lcdwiki_es3c35p_local_defaults_keep_mobile_ble_discovery_enabled():
    local_defaults = read("sdkconfig.defaults.local")

    assert "# CONFIG_USE_HOTSPOT_WIFI_PROVISIONING is not set" in local_defaults
    assert "CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING=y" in local_defaults
    assert "CONFIG_BT_BLUEDROID_ENABLED=y" in local_defaults


def test_lcdwiki_es3c35p_bluedroid_uses_psram_backed_dynamic_heap():
    sdkconfig = lcdwiki_reference_sdkconfig()

    assert "CONFIG_SPIRAM=y" in sdkconfig
    assert "CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y" in sdkconfig
    assert "# CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST is not set" not in sdkconfig
    assert "CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y" in sdkconfig
    assert "# CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY is not set" not in sdkconfig


def test_lcdwiki_es3c35p_generated_language_matches_vietnamese_sdkconfig():
    sdkconfig = lcdwiki_reference_sdkconfig()
    vi = read("main/assets/locales/vi-VN/language.json")
    cmake = read("main/CMakeLists.txt")

    assert "CONFIG_LANGUAGE_VI_VN=y" in sdkconfig
    assert 'set(LANG_DIR "vi-VN")' in cmake
    assert '"WIFI_CONFIG_MODE": "Chế độ cấu hình Wi-Fi"' in vi
    assert "配网模式" not in vi


def test_lcdwiki_es3c35p_generation_uses_idf_python_interpreter():
    cmake = read("main/CMakeLists.txt")

    assert "COMMAND python " not in cmake
    assert "COMMAND ${PYTHON} ${PROJECT_DIR}/scripts/gen_lang.py" in cmake
    assert "COMMAND ${PYTHON} ${PROJECT_DIR}/scripts/build_default_assets.py" in cmake

def test_lcdwiki_es3c35p_uses_st77922_qspi_pins_and_touch_i2c():
    manifest = read("main/idf_component.yml")
    config = read("main/boards/lcdwiki-es3c35p/config.h")
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "espressif/esp_lcd_st77922" in manifest
    assert has_define(config, "DISPLAY_CS_PIN", "GPIO_NUM_10")
    assert has_define(config, "DISPLAY_CLK_PIN", "GPIO_NUM_12")
    assert has_define(config, "DISPLAY_DATA0_PIN", "GPIO_NUM_11")
    assert has_define(config, "DISPLAY_DATA1_PIN", "GPIO_NUM_13")
    assert has_define(config, "DISPLAY_DATA2_PIN", "GPIO_NUM_14")
    assert has_define(config, "DISPLAY_DATA3_PIN", "GPIO_NUM_9")
    assert has_define(config, "DISPLAY_BACKLIGHT_PIN", "GPIO_NUM_41")
    assert has_define(config, "TOUCH_SDA_PIN", "GPIO_NUM_38")
    assert has_define(config, "TOUCH_SCL_PIN", "GPIO_NUM_39")
    assert has_define(config, "TOUCH_RST_PIN", "GPIO_NUM_47")
    assert has_define(config, "TOUCH_INT_PIN", "GPIO_NUM_48")
    assert ".data0_io_num = DISPLAY_DATA0_PIN" in board
    assert ".data1_io_num = DISPLAY_DATA1_PIN" in board
    assert ".data2_io_num = DISPLAY_DATA2_PIN" in board
    assert ".data3_io_num = DISPLAY_DATA3_PIN" in board
    assert "ST77922_PANEL_IO_QSPI_CONFIG" in board
    assert "ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG" not in board
    assert "ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG" not in board
    assert "esp_lcd_new_panel_st77922" in board
    assert "St77922RounderCallback" in board


def test_lcdwiki_es3c35p_does_not_call_unsupported_st77922_hardware_swap_xy():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "landscape/sw-rotate" in board
    assert "esp_lcd_panel_swap_xy(panel" not in board


def test_lcdwiki_es3c35p_matches_lcdwiki_panel_power_sequence():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "DISPLAY_WIDTH * 80 * static_cast<int>(sizeof(uint16_t))" in board
    assert "kLcdQspiClockHz = 20 * 1000 * 1000" in board
    assert "io_config.pclk_hz = kLcdQspiClockHz" in board
    assert "class LcdWikiBacklight" in board
    assert ".mode = GPIO_MODE_INPUT_OUTPUT" in board
    assert "gpio_set_level(DISPLAY_BACKLIGHT_PIN, brightness > 0 ? 1 : 0)" in board
    assert "gpio_get_level(DISPLAY_BACKLIGHT_PIN)" in board
    assert "EnableBacklightForBoot" in board
    assert "Boot probe pattern drawn" in board
    assert "kHoldBootProbePattern = false" in board
    assert "LVGL/application display init skipped" not in board
    assert "Initialize LVGL library" in board
    assert board.index("ESP_LOGI(TAG, \"Turning display on\")") < board.index("DrawBootProbePattern")
    assert board.index("DrawBootProbePattern") < board.index("ESP_LOGI(TAG, \"Initialize LVGL library\")")


def test_lcdwiki_es3c35p_uses_lcdwiki_audio_and_uart_pins():
    config = read("main/boards/lcdwiki-es3c35p/config.h")
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_17" in config
    assert "#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_18" in config
    assert "#define AUDIO_I2S_GPIO_WS   GPIO_NUM_21" in config
    assert "#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_16" in config
    assert "#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_15" in config
    # 92 is the measured Live TTS sweet spot for this PA without clipping.
    assert "constexpr int kLcdWikiOutputVolume = 92" in board
    assert "class LcdWikiAudioCodec : public Es8311AudioCodec" in board
    assert "input_channels_ = 1;" in board
    assert "output_channels_ = 1;" in board
    assert "SetOutputVolume(kLcdWikiOutputVolume);" in board
    assert "static LcdWikiAudioCodec audio_codec" in board
    assert "#define ROBOT_UART_TX_PIN     GPIO_NUM_43" in config
    assert "#define ROBOT_UART_RX_PIN     GPIO_NUM_44" in config

def test_lcdwiki_es3c35p_caps_output_volume_at_safe_hardware_maximum():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "void SetOutputVolume(int volume) override" in board
    assert "const int safe_volume = std::clamp(volume, 0, kLcdWikiOutputVolume);" in board
    assert '"LCDWiki output volume limited requested=%d applied=%d"' in board
    assert "Es8311AudioCodec::SetOutputVolume(safe_volume);" in board

def test_lcdwiki_es3c35p_mounts_micro_sd_for_lesson_assets():
    config = read("main/boards/lcdwiki-es3c35p/config.h")
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert '#define SDCARD_MOUNT_POINT    "/sdcard"' in config
    assert has_define(config, "SDCARD_SDMMC_CLK_PIN", "GPIO_NUM_5")
    assert has_define(config, "SDCARD_SDMMC_CMD_PIN", "GPIO_NUM_4")
    assert has_define(config, "SDCARD_SDMMC_D0_PIN", "GPIO_NUM_6")
    assert has_define(config, "SDCARD_SDMMC_D1_PIN", "GPIO_NUM_7")
    assert has_define(config, "SDCARD_SDMMC_D2_PIN", "GPIO_NUM_2")
    assert has_define(config, "SDCARD_SDMMC_D3_PIN", "GPIO_NUM_3")
    assert "esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT" in board
    assert "slot_config.width = 4;" in board
    assert "InitializeSdCard();" in board
    assert "SD card mounted at %s" in board

def test_lcdwiki_es3c35p_fatfs_supports_long_lesson_asset_names():
    sdkconfig = read("sdkconfig.defaults.esp32s3") + "\n" + read("sdkconfig.defaults.local")

    assert "CONFIG_FATFS_LFN_HEAP=y" in sdkconfig
    assert "CONFIG_FATFS_LFN_NONE=y" not in sdkconfig

def test_lcdwiki_es3c35p_runs_boot_audio_diagnostic_tone():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "RunDiagnosticTone();" in board
    assert "ConfigurePaGpioForDiagnostic();" in board
    assert "const int amplitude = 24000;" in board
    assert 'PlayDiagnosticSegment("pa_low", 0, 660);' in board
    assert 'PlayDiagnosticSegment("pa_high", 1, 880);' in board
    assert 'PlayDiagnosticSegment("configured", AUDIO_CODEC_PA_INVERTED ? 0 : 1, 1100);' in board
    assert "LCDWiki audio diagnostic segment start name=%s" in board
    assert "LCDWiki audio diagnostic tone sequence end" in board
    assert "EnableOutput(true);" in board
    assert "OutputData(tone);" in board

def test_lcdwiki_es3c35p_does_not_register_flaky_touch_controller():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    init_touch = board[board.index("void InitializeTouch()") : board.index("void InitializeButtons()")]
    assert "Touch disabled for LCDWiki stability" in init_touch
    assert "return;" in init_touch
    assert "lvgl_port_add_touch" not in init_touch

def test_lcdwiki_es3c35p_boot_long_press_reenters_repair_pairing():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "boot_button_(BOOT_BUTTON_GPIO, false, 5000)" in board

    init_start = board.index("void InitializeButtons()")
    init_buttons = board[init_start : board.index("public:", init_start)]
    assert "boot_button_.OnPressDown" in init_buttons
    assert "boot_button_.OnPressUp" in init_buttons
    assert "LCDWiki BOOT press down" in init_buttons
    assert "LCDWiki BOOT press up" in init_buttons
    assert "boot_button_.OnLongPress" in init_buttons

    # Long-press is the "re-pair" gesture: forget the current claim/owner and
    # re-enter pairing so a (possibly different) parent phone can connect. It must
    # NOT merely reopen Wi-Fi config -- that left the device claimed/owned and
    # therefore "stuck" to the previously connected phone/account.
    long_press_body = init_buttons[init_buttons.index("boot_button_.OnLongPress"):]
    assert "LCDWiki BOOT long-press -> EnterRepairPairingMode" in long_press_body
    assert "Application::GetInstance().EnterRepairPairingMode();" in long_press_body
    assert "EnterWifiConfigMode();" not in long_press_body

def test_lcdwiki_es3c35p_boot_click_rearms_ble_wifi_config_mode():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    init_start = board.index("void InitializeButtons()")
    init_buttons = board[init_start : board.index("public:", init_start)]
    click_start = init_buttons.index("boot_button_.OnClick")
    click_body = init_buttons[click_start : init_buttons.index("boot_button_.OnLongPress", click_start)]

    assert "kDeviceStateStarting" in click_body
    assert "kDeviceStateWifiConfiguring" in click_body
    assert "kDeviceStateActivating" in click_body
    assert "kDeviceStateConnecting" in click_body
    assert click_body.index("kDeviceStateWifiConfiguring") < click_body.index("app.ToggleChatState();")
    assert click_body.index("kDeviceStateConnecting") < click_body.index("app.ToggleChatState();")
    assert "EnterWifiConfigMode();" in click_body[:click_body.index("app.ToggleChatState();")]

def test_lcdwiki_es3c35p_boot_click_ignores_active_lesson_before_wifi_config_or_toggle():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    init_start = board.index("void InitializeButtons()")
    init_buttons = board[init_start : board.index("public:", init_start)]
    click_start = init_buttons.index("boot_button_.OnClick")
    click_body = init_buttons[click_start : init_buttons.index("boot_button_.OnLongPress", click_start)]

    assert "app.IsLessonRuntimeActive()" in click_body
    assert click_body.index("app.IsLessonRuntimeActive()") < click_body.index("EnterWifiConfigMode();")
    assert click_body.index("app.IsLessonRuntimeActive()") < click_body.index("app.ToggleChatState();")
    guard = click_body[
        click_body.index("app.IsLessonRuntimeActive()") :
        click_body.index("auto state = app.GetDeviceState();")
    ]
    assert "return;" in guard
    assert "EnterWifiConfigMode();" not in guard
    assert "ToggleChatState" not in guard
