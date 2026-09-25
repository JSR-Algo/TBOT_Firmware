#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// =============================================================================
//  LCDWiki 3.5inch ESP32-S3 Display  (SKU: ES3C35P)
//  - Chip   : ESP32-S3 N16R8 (16MB QSPI Flash + 8MB OPI PSRAM)
//  - LCD    : 3.5" IPS 320x480, driver ST77922, giao tiếp QSPI
//  - Touch  : capacitive, I2C (tích hợp trong panel HMX035CTFT-001)
//  - Audio  : codec ES8311 (I2C) + amp SC8002B (FM8002E họ LM4871, active-low)
//  - Pin map: theo "ESP32-S3 IO资源分配表" chính thức của LCDWiki ES3C35P
// =============================================================================

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/uart.h>
#include <hal/lcd_types.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ---- I2S (ES8311) ----
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_17
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_18
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_21
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_16   // mic data ESP <- codec
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_15   // playback ESP -> codec

// ---- Audio codec / amplifier ----
// GPIO1 = chân enable amp SC8002B. Bảng IO LCDWiki ghi "低电平使能" (active-low),
// nên truyền pa_inverted=true khi tạo Es8311AudioCodec (xem file .cc).
#define AUDIO_CODEC_PA_PIN          GPIO_NUM_1
#define AUDIO_CODEC_PA_INVERTED     true
#define AUDIO_CODEC_I2C_NUM         I2C_NUM_0
#define AUDIO_CODEC_I2C_SDA_PIN     GPIO_NUM_38   // dùng chung với touch I2C
#define AUDIO_CODEC_I2C_SCL_PIN     GPIO_NUM_39   // dùng chung với touch I2C
#define AUDIO_CODEC_ES8311_ADDR     ES8311_CODEC_DEFAULT_ADDR

// ---- Buttons / LED ----
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BUILTIN_LED_GPIO        GPIO_NUM_40   // RGB LED 1 dây (WS2812) trên GPIO40
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// ---- LCD ST77922 (QSPI) ----
#define DISPLAY_SPI_HOST  SPI2_HOST
// Panel vật lý 320x480 portrait. UI hiển thị NGANG 480x320 bằng xoay phần mềm LVGL.
#define DISPLAY_WIDTH     480
#define DISPLAY_HEIGHT    320

#define DISPLAY_CS_PIN    GPIO_NUM_10
#define DISPLAY_CLK_PIN   GPIO_NUM_12
#define DISPLAY_DATA0_PIN GPIO_NUM_11
#define DISPLAY_DATA1_PIN GPIO_NUM_13
#define DISPLAY_DATA2_PIN GPIO_NUM_14
#define DISPLAY_DATA3_PIN GPIO_NUM_9
// LCD reset dùng chung chân EN của ESP32-S3 (không có GPIO riêng) -> NC.
#define DISPLAY_RST_PIN   GPIO_NUM_NC
// GPIO42 = LCD command/data select (DC) theo bảng IO; chế độ QSPI KHÔNG dùng DC.
// #define DISPLAY_DC_PIN  GPIO_NUM_42

#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_41
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_QSPI_PCLK_HZ            (20 * 1000 * 1000)  // 20MHz an toàn; có thể thử 40/80MHz

#define DISPLAY_RGB_ORDER   LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_INVERT_COLOR false
#define DISPLAY_SWAP_XY      true    // true = landscape (xoay 90° phần mềm LVGL)
#define DISPLAY_MIRROR_X     false
#define DISPLAY_MIRROR_Y     false
#define DISPLAY_OFFSET_X     0
#define DISPLAY_OFFSET_Y     0

// ---- Capacitive touch (I2C) ----
// LƯU Ý: theo bảng IO LCDWiki, GPIO47=RST, GPIO48=INT (ngược với một số tài liệu cũ).
#define TOUCH_SDA_PIN GPIO_NUM_38
#define TOUCH_SCL_PIN GPIO_NUM_39
#define TOUCH_RST_PIN GPIO_NUM_47
#define TOUCH_INT_PIN GPIO_NUM_48

// ---- Battery ADC ----
#define BAT_ADC_PIN   GPIO_NUM_8

// ---- Robot arm UART (đặc thù TBOT) ----
// Day noi main<->slave dau theo NHAN silkscreen TX/RX (cheo theo so GPIO):
//   main GPIO43(nhan TX) <-> slave GPIO44(nhan RX)
//   main GPIO44(nhan RX) <-> slave GPIO43(nhan TX)
// Nen main phai TX=GPIO43 (toi slave RX44) va RX=GPIO44 (nhan slave TX43).
// Profile alt giu chieu con lai de tuong thich kieu dau thang (43-43/44-44).
#define ROBOT_UART_NUM        UART_NUM_1
#define ROBOT_UART_TX_PIN     GPIO_NUM_43
#define ROBOT_UART_RX_PIN     GPIO_NUM_44
#define ROBOT_UART_BAUD_RATE  115200
#define ROBOT_UART_ALT_NUM    UART_NUM_1
#define ROBOT_UART_ALT_TX_PIN GPIO_NUM_44
#define ROBOT_UART_ALT_RX_PIN GPIO_NUM_43
#define ROBOT_UART_ACK_READER_ENABLED 1

// ---- MicroSD / TF card (SDIO 4-bit, LCDWiki ES3C35P spec) ----
#define SDCARD_MOUNT_POINT    "/sdcard"
#define SDCARD_SDMMC_CLK_PIN  GPIO_NUM_5
#define SDCARD_SDMMC_CMD_PIN  GPIO_NUM_4
#define SDCARD_SDMMC_D0_PIN   GPIO_NUM_6
#define SDCARD_SDMMC_D1_PIN   GPIO_NUM_7
#define SDCARD_SDMMC_D2_PIN   GPIO_NUM_2
#define SDCARD_SDMMC_D3_PIN   GPIO_NUM_3

#endif // _BOARD_CONFIG_H_
