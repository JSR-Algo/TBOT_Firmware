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
#include <esp_system.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77922.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lcd_touch_st7123.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <src/misc/cache/lv_cache.h>

#define TAG "LCDWikiES3C35P"

namespace {

// ST77922 ở chế độ QSPI yêu cầu cửa sổ vẽ căn theo cột bội số 4.
// LVGL có thể gửi vùng bẩn lệch cột -> căn lại tại đây để tránh nhiễu/xé hình.
void St77922RounderCallback(lv_area_t* area) {
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
    area->x1 = std::max<int32_t>(0, area->x1);
    area->x2 = std::min<int32_t>(DISPLAY_WIDTH - 1, area->x2);
}

// Bản sao của SpiLcdDisplay (cùng cấu hình LVGL/font/theme qua base LcdDisplay),
// nhưng bổ sung rounder_cb cần cho ST77922 QSPI.
class St77922QspiDisplay : public LcdDisplay {
public:
    St77922QspiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
        int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
        : LcdDisplay(panel_io, panel, width, height) {

        // Vẽ trắng toàn màn để xác nhận panel sống.
        std::vector<uint16_t> buffer(width_, 0xFFFF);
        for (int y = 0; y < height_; y++) {
            esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
        }

        ESP_LOGI(TAG, "Turning display on");
        esp_err_t err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(err);
        }

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

    // Thả reset cho touch: drive CẢ GPIO47 và GPIO48 (vì còn nghi ngờ chân nào là RST).
    void ReleaseTouchReset() {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << 47) | (1ULL << 48);
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);
        gpio_set_level((gpio_num_t)47, 0);
        gpio_set_level((gpio_num_t)48, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)47, 1);
        gpio_set_level((gpio_num_t)48, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
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
            .max_transfer_sz = DISPLAY_WIDTH * 80 * static_cast<int>(sizeof(uint16_t)),
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
            DISPLAY_CS_PIN, nullptr, nullptr);
        io_config.pclk_hz = DISPLAY_QSPI_PCLK_HZ;
        ESP_LOGI(TAG, "ST77922 QSPI clock: %d Hz", (int)io_config.pclk_hz);
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

    // ---- Touch: panel HMX035CTFT-001 dùng controller tích hợp. LCDWiki không công bố
    // tên IC, nên dò lần lượt FT5x06 -> GT911 (2 địa chỉ). Touch không bắt buộc cho
    // voice chat; nếu không nhận thì firmware vẫn chạy bình thường, chỉ mất cảm ứng.
    bool TryInitFt5x06() {
        esp_lcd_panel_io_handle_t io = nullptr;
        const esp_lcd_panel_io_i2c_config_t io_cfg = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags = { .disable_control_phase = 1 },
            .scl_speed_hz = 400 * 1000,
        };
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_cfg, &io) != ESP_OK) return false;

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = DISPLAY_SWAP_XY, .mirror_x = DISPLAY_MIRROR_X, .mirror_y = DISPLAY_MIRROR_Y },
        };
        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, &tp) != ESP_OK) {
            esp_lcd_panel_io_del(io);
            return false;
        }
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = lv_display_get_default(), .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "FT5x06 touch initialized");
        return true;
    }

    bool TryInitGt911(uint8_t address) {
        esp_lcd_panel_io_handle_t io = nullptr;
        const esp_lcd_panel_io_i2c_config_t io_cfg = {
            .dev_addr = address,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .lcd_param_bits = 0,
            .flags = { .disable_control_phase = 1 },
            .scl_speed_hz = 400 * 1000,
        };
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_cfg, &io) != ESP_OK) return false;

        esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = address };
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = DISPLAY_SWAP_XY, .mirror_x = DISPLAY_MIRROR_X, .mirror_y = DISPLAY_MIRROR_Y },
            .driver_data = &gt911_cfg,
        };
        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &tp) != ESP_OK) {
            esp_lcd_panel_io_del(io);
            return false;
        }
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = lv_display_get_default(), .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "GT911 touch initialized addr=0x%02x", address);
        return true;
    }

    bool TryInitCst816(uint8_t address) {
        esp_lcd_panel_io_handle_t io = nullptr;
        const esp_lcd_panel_io_i2c_config_t io_cfg = {
            .dev_addr = address,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags = { .disable_control_phase = 1 },
            .scl_speed_hz = 400 * 1000,
        };
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_cfg, &io) != ESP_OK) return false;

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = DISPLAY_SWAP_XY, .mirror_x = DISPLAY_MIRROR_X, .mirror_y = DISPLAY_MIRROR_Y },
        };
        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, &tp) != ESP_OK) {
            esp_lcd_panel_io_del(io);
            return false;
        }
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = lv_display_get_default(), .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "CST816 touch initialized addr=0x%02x", address);
        return true;
    }

    // Sitronix ST7123 @ 0x55 — touch thực tế của panel HMX035CTFT-001 (cặp với LCD ST77922).
    bool TryInitSt7123() {
        esp_lcd_panel_io_handle_t io = nullptr;
        const esp_lcd_panel_io_i2c_config_t io_cfg = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .lcd_param_bits = 0,
            .flags = { .disable_control_phase = 1 },
            .scl_speed_hz = 400 * 1000,
        };
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_cfg, &io) != ESP_OK) return false;

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = DISPLAY_SWAP_XY, .mirror_x = DISPLAY_MIRROR_X, .mirror_y = DISPLAY_MIRROR_Y },
        };
        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_st7123(io, &tp_cfg, &tp) != ESP_OK) {
            esp_lcd_panel_io_del(io);
            return false;
        }
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = lv_display_get_default(), .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "ST7123 touch initialized addr=0x%02x", ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS);
        return true;
    }

    void InitializeTouch() {
        if (lv_display_get_default() == nullptr) {
            ESP_LOGW(TAG, "LVGL display missing; skipping touch");
            return;
        }
        if (TryInitSt7123()) return;
        if (TryInitFt5x06()) return;
        if (TryInitCst816(ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS)) return;
        if (TryInitGt911(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS)) return;
        if (TryInitGt911(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)) return;
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
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        // Workaround "màn đen khi cấp nguồn lạnh" cho dòng panel này: restart 1 lần
        // sau cold boot (chỉ chạy đúng 1 lần vì reset reason đổi thành SW).
        if (esp_reset_reason() == ESP_RST_POWERON) {
            fflush(stdout);
            esp_restart();
        }
        // Touch ST7123 lên nguồn cùng panel sau khi LCD init -> thả reset rồi dò.
        ReleaseTouchReset();
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
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
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            /*use_mclk=*/true, /*pa_inverted=*/AUDIO_CODEC_PA_INVERTED);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

} // namespace

DECLARE_BOARD(LCDWikiES3C35PBoard);
