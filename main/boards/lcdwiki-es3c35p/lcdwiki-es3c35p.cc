#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"

#include <algorithm>
#include <vector>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77922.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <src/misc/cache/lv_cache.h>

#define TAG "LCDWikiES3C35P"

namespace {

constexpr int kLcdQspiClockHz = 20 * 1000 * 1000;
constexpr bool kHoldBootProbePattern = true;

void EnableBacklightForBoot() {
    const gpio_config_t backlight_gpio_config = {
        .pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1));
    ESP_LOGI(TAG, "LCDWiki backlight GPIO%d forced high for boot, level=%d",
        DISPLAY_BACKLIGHT_PIN, gpio_get_level(DISPLAY_BACKLIGHT_PIN));
}

class LcdWikiBacklight : public Backlight {
public:
    LcdWikiBacklight() {
        EnableBacklightForBoot();
    }

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(DISPLAY_BACKLIGHT_PIN, brightness > 0 ? 1 : 0));
        if (brightness == 0 || brightness == 100) {
            ESP_LOGI(TAG, "LCDWiki backlight GPIO%d brightness=%u level=%d",
                DISPLAY_BACKLIGHT_PIN, brightness, gpio_get_level(DISPLAY_BACKLIGHT_PIN));
        }
    }
};

void St77922RounderCallback(lv_area_t* area) {
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
    area->x1 = std::max<int32_t>(0, area->x1);
    area->x2 = std::min<int32_t>(DISPLAY_WIDTH - 1, area->x2);
}

class St77922QspiDisplay : public LcdDisplay {
public:
    St77922QspiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
        int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
        : LcdDisplay(panel_io, panel, width, height) {

        ESP_LOGI(TAG, "Turning display on");
        esp_err_t err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(err);
        }

        DrawBootProbePattern();
        if (kHoldBootProbePattern) {
            ESP_LOGW(TAG, "LCDWiki boot probe hold active; LVGL/application display init skipped");
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                DrawBootProbePattern();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));

        ESP_LOGI(TAG, "Initialize LVGL library");
        lv_init();

#if CONFIG_SPIRAM
        size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
        if (psram_size_mb >= 8) {
            lv_image_cache_resize(2 * 1024 * 1024, true);
            ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
        } else if (psram_size_mb >= 2) {
            lv_image_cache_resize(512 * 1024, true);
            ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
        }
#endif

        ESP_LOGI(TAG, "Initialize LVGL port");
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
        port_cfg.task_affinity = 1;
#endif
        lvgl_port_init(&port_cfg);

        ESP_LOGI(TAG, "Adding ST77922 QSPI LCD display");
        const lvgl_port_display_cfg_t display_cfg = {
            .io_handle = panel_io_,
            .panel_handle = panel_,
            .control_handle = nullptr,
            .buffer_size = static_cast<uint32_t>(width_ * 20),
            .double_buffer = false,
            .trans_size = 0,
            .hres = static_cast<uint32_t>(width_),
            .vres = static_cast<uint32_t>(height_),
            .monochrome = false,
            .rotation = {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
            .rounder_cb = St77922RounderCallback,
            .color_format = LV_COLOR_FORMAT_RGB565,
            .flags = {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
        };

        display_ = lvgl_port_add_disp(&display_cfg);
        if (display_ == nullptr) {
            ESP_LOGE(TAG, "Failed to add display");
            return;
        }

        if (offset_x != 0 || offset_y != 0) {
            lv_display_set_offset(display_, offset_x, offset_y);
        }
    }

private:
    void DrawBootProbePattern() {
        constexpr int kChunkLines = 20;
        constexpr uint16_t kColors[] = {0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0};

        std::vector<uint16_t> buffer(width_ * kChunkLines);
        for (int y = 0; y < height_; y += kChunkLines) {
            int y_end = std::min(y + kChunkLines, height_);
            uint16_t color = kColors[(y * static_cast<int>(std::size(kColors))) / height_];
            std::fill(buffer.begin(), buffer.begin() + (y_end - y) * width_, color);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y_end, buffer.data()));
        }
        ESP_LOGI(TAG, "Boot probe pattern drawn");
    }
};

class LCDWikiES3C35PBoard : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;

    void InitializeI2c() {
        const i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize ST77922 QSPI bus");
        const spi_bus_config_t buscfg = {
            .data0_io_num = DISPLAY_DATA0_PIN,
            .data1_io_num = DISPLAY_DATA1_PIN,
            .sclk_io_num = DISPLAY_CLK_PIN,
            .data2_io_num = DISPLAY_DATA2_PIN,
            .data3_io_num = DISPLAY_DATA3_PIN,
            .data4_io_num = GPIO_NUM_NC,
            .data5_io_num = GPIO_NUM_NC,
            .data6_io_num = GPIO_NUM_NC,
            .data7_io_num = GPIO_NUM_NC,
            .data_io_default_level = false,
            .max_transfer_sz = DISPLAY_WIDTH * 80 * sizeof(uint16_t),
            .flags = 0,
            .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
            .intr_flags = 0,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install ST77922 QSPI panel IO");
        esp_lcd_panel_io_spi_config_t io_config = ST77922_PANEL_IO_QSPI_CONFIG(
            DISPLAY_CS_PIN,
            nullptr,
            nullptr);
        io_config.pclk_hz = kLcdQspiClockHz;
        ESP_LOGI(TAG, "ST77922 QSPI clock: %d Hz", io_config.pclk_hz);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        const st77922_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_RST_PIN,
            .rgb_ele_order = DISPLAY_RGB_ORDER,
            .bits_per_pixel = 16,
            .vendor_config = const_cast<st77922_vendor_config_t*>(&vendor_config),
        };

        ESP_LOGI(TAG, "Install LCD driver ST77922");
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77922(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        if (DISPLAY_INVERT_COLOR) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_invert_color(panel, true));
        }
        if (DISPLAY_SWAP_XY) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_swap_xy(panel, true));
        }
        if (DISPLAY_MIRROR_X || DISPLAY_MIRROR_Y) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        }

        display_ = new St77922QspiDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    bool TryInitializeFt5x06Touch() {
        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        const esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags = {
                .disable_control_phase = 1,
            },
            .scl_speed_hz = 400 * 1000,
        };
        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "FT5x06 touch IO init failed: %s", esp_err_to_name(ret));
            return false;
        }

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        };

        esp_lcd_touch_handle_t tp = nullptr;
        ret = esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "FT5x06 touch not detected: %s", esp_err_to_name(ret));
            esp_lcd_panel_io_del(tp_io_handle);
            return false;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "FT5x06 touch initialized");
        return true;
    }

    bool TryInitializeGt911Touch(uint8_t address) {
        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        const esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = address,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .lcd_param_bits = 0,
            .flags = {
                .disable_control_phase = 1,
            },
            .scl_speed_hz = 400 * 1000,
        };
        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GT911 touch IO init failed addr=0x%02x: %s", address, esp_err_to_name(ret));
            return false;
        }

        esp_lcd_touch_io_gt911_config_t gt911_config = {
            .dev_addr = address,
        };
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
            .driver_data = &gt911_config,
        };

        esp_lcd_touch_handle_t tp = nullptr;
        ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GT911 touch not detected addr=0x%02x: %s", address, esp_err_to_name(ret));
            esp_lcd_panel_io_del(tp_io_handle);
            return false;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "GT911 touch initialized addr=0x%02x", address);
        return true;
    }

    void InitializeTouch() {
        if (lv_display_get_default() == nullptr) {
            ESP_LOGW(TAG, "LVGL display missing; skipping touch");
            return;
        }
        if (TryInitializeFt5x06Touch()) {
            return;
        }
        if (TryInitializeGt911Touch(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS)) {
            return;
        }
        if (TryInitializeGt911Touch(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)) {
            return;
        }
        ESP_LOGW(TAG, "Touch controller not detected; continuing without touch");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    LCDWikiES3C35PBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        EnableBacklightForBoot();
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->SetBrightness(100);
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, AUDIO_CODEC_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        static LcdWikiBacklight backlight;
        return &backlight;
    }
};

} // namespace

DECLARE_BOARD(LCDWikiES3C35PBoard);
