import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")

def has_define(text: str, name: str, value: str) -> bool:
    return re.search(rf"^#define\s+{re.escape(name)}\s+{re.escape(value)}\b", text, re.MULTILINE) is not None


def test_lcdwiki_es3c35p_board_is_selected_and_registered():
    kconfig = read("main/Kconfig.projbuild")
    cmake = read("main/CMakeLists.txt")
    local_defaults = read("sdkconfig.defaults.local")
    sdkconfig = read("sdkconfig.es3c35p")

    assert "config BOARD_TYPE_LCDWIKI_ES3C35P" in kconfig
    assert 'bool "LCDWiki ES3C35P ESP32-S3 3.5 LCD"' in kconfig
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P" in cmake
    assert 'set(BOARD_TYPE "lcdwiki-es3c35p")' in cmake
    assert "CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y" in local_defaults
    assert "CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5=y" not in local_defaults
    assert "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" in sdkconfig
    assert "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set" in sdkconfig
    assert "CONFIG_ESP_CONSOLE_UART_NUM=-1" in sdkconfig

def test_lcdwiki_es3c35p_local_defaults_keep_mobile_ble_discovery_enabled():
    local_defaults = read("sdkconfig.defaults.local")

    assert "# CONFIG_USE_HOTSPOT_WIFI_PROVISIONING is not set" in local_defaults
    assert "CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING=y" in local_defaults
    assert "CONFIG_BT_BLUEDROID_ENABLED=y" in local_defaults


def test_lcdwiki_es3c35p_generated_language_matches_vietnamese_sdkconfig():
    sdkconfig = read("sdkconfig.es3c35p")
    lang_header = read("main/assets/lang_config.h")

    assert "CONFIG_LANGUAGE_VI_VN=y" in sdkconfig
    assert 'constexpr const char* CODE = "vi-VN";' in lang_header
    assert 'constexpr const char* CODE = "zh-CN";' not in lang_header
    assert 'constexpr const char* WIFI_CONFIG_MODE = "Chế độ cấu hình Wi-Fi";' in lang_header
    assert 'constexpr const char* WIFI_CONFIG_MODE = "配网模式";' not in lang_header


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
    assert "constexpr int kLcdWikiOutputVolume = 70" in board
    assert "class LcdWikiAudioCodec : public Es8311AudioCodec" in board
    assert "input_channels_ = 2;" in board
    assert "output_channels_ = 2;" in board
    assert "SetOutputVolume(kLcdWikiOutputVolume);" in board
    assert "static LcdWikiAudioCodec audio_codec" in board
    assert "#define ROBOT_UART_TX_PIN     GPIO_NUM_44" in config
    assert "#define ROBOT_UART_RX_PIN     GPIO_NUM_43" in config

def test_lcdwiki_es3c35p_runs_boot_audio_diagnostic_tone():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "RunDiagnosticTone();" in board
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

    init_start = board.index("void InitializeButtons()")
    init_buttons = board[init_start : board.index("public:", init_start)]
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
