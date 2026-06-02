from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


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


def test_lcdwiki_es3c35p_uses_st77922_qspi_pins_and_touch_i2c():
    manifest = read("main/idf_component.yml")
    config = read("main/boards/lcdwiki-es3c35p/config.h")
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "espressif/esp_lcd_st77922" in manifest
    assert "#define DISPLAY_CS_PIN GPIO_NUM_10" in config
    assert "#define DISPLAY_CLK_PIN GPIO_NUM_12" in config
    assert "#define DISPLAY_DATA0_PIN GPIO_NUM_11" in config
    assert "#define DISPLAY_DATA1_PIN GPIO_NUM_13" in config
    assert "#define DISPLAY_DATA2_PIN GPIO_NUM_14" in config
    assert "#define DISPLAY_DATA3_PIN GPIO_NUM_9" in config
    assert "#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_41" in config
    assert "#define TOUCH_SDA_PIN GPIO_NUM_38" in config
    assert "#define TOUCH_SCL_PIN GPIO_NUM_39" in config
    assert "#define TOUCH_RST_PIN GPIO_NUM_48" in config
    assert "#define TOUCH_INT_PIN GPIO_NUM_47" in config
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

    assert "DISPLAY_WIDTH * 80 * sizeof(uint16_t)" in board
    assert "kLcdQspiClockHz = 20 * 1000 * 1000" in board
    assert "io_config.pclk_hz = kLcdQspiClockHz" in board
    assert "class LcdWikiBacklight" in board
    assert ".mode = GPIO_MODE_INPUT_OUTPUT" in board
    assert "gpio_set_level(DISPLAY_BACKLIGHT_PIN, brightness > 0 ? 1 : 0)" in board
    assert "gpio_get_level(DISPLAY_BACKLIGHT_PIN)" in board
    assert "EnableBacklightForBoot" in board
    assert "Boot probe pattern drawn" in board
    assert "kHoldBootProbePattern = true" in board
    assert "LCDWiki boot probe hold active" in board
    assert board.index("ESP_LOGI(TAG, \"Turning display on\")") < board.index("DrawBootProbePattern")


def test_lcdwiki_es3c35p_uses_lcdwiki_audio_and_uart_pins():
    config = read("main/boards/lcdwiki-es3c35p/config.h")

    assert "#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_17" in config
    assert "#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_18" in config
    assert "#define AUDIO_I2S_GPIO_WS   GPIO_NUM_21" in config
    assert "#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_16" in config
    assert "#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_15" in config
    assert "#define ROBOT_UART_TX_PIN     GPIO_NUM_44" in config
    assert "#define ROBOT_UART_RX_PIN     GPIO_NUM_43" in config
