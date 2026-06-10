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

// ST77922 init sequence cho panel HMX035CTFT-001 (320x480 QSPI) của LCDWiki ES3C35P.
// Trích nguyên xi từ firmware Xiaozhi v1.7.6 chính chủ LCDWiki/QDtech (init mặc định của
// component esp_lcd_st77922 dành cho panel ~532x300 khác -> gây sọc dọc).
// 0xF1/0xF2/0xF0 = chuyển bank lệnh (PAGE_CMD2/3/1). CASET=320, RASET=480.
const st77922_lcd_init_cmd_t kSt77922InitCmds[] = {
    {0xF1, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x00, 0x00, 0x00}, 3, 0},
    {0x65, (uint8_t []){0x80}, 1, 0},
    {0x79, (uint8_t []){0x06}, 1, 0},
    {0x7B, (uint8_t []){0x00, 0x08, 0x08}, 3, 0},
    {0x80, (uint8_t []){0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA}, 11, 0},
    {0x81, (uint8_t []){0x26, 0x52, 0x72, 0x27}, 4, 0},
    {0x84, (uint8_t []){0x92, 0x25}, 2, 0},
    {0x87, (uint8_t []){0x10, 0x10, 0x58, 0x00, 0x02, 0x3A}, 6, 0},
    {0x88, (uint8_t []){0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06}, 15, 0},
    {0x89, (uint8_t []){0x00, 0x00, 0x00}, 3, 0},
    {0x8A, (uint8_t []){0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19}, 11, 0},
    {0x8B, (uint8_t []){0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E}, 9, 0},
    {0x8C, (uint8_t []){0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09}, 13, 0},
    {0x8D, (uint8_t []){0x0C}, 1, 0},
    {0x8E, (uint8_t []){0x33, 0x01, 0x0C, 0x13, 0x01, 0x01}, 6, 0},
    {0xB3, (uint8_t []){0x00, 0x30}, 2, 0},
    {0xF1, (uint8_t []){0x00}, 1, 0},
    {0x71, (uint8_t []){0xC0}, 1, 0},
    {0x66, (uint8_t []){0x02, 0x3F}, 2, 0},
    {0xBE, (uint8_t []){0x1E, 0x00, 0x9D}, 3, 0},
    {0x70, (uint8_t []){0x01, 0xA6, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x60, 0x11, 0x00, 0x00, 0x1A}, 12, 0},
    {0x90, (uint8_t []){0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x2D, 0x2D}, 9, 0},
    {0x91, (uint8_t []){0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x2D, 0x2D}, 9, 0},
    {0x92, (uint8_t []){0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x2D}, 10, 0},
    {0x93, (uint8_t []){0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x2D}, 10, 0},
    {0x94, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 6, 0},
    {0x95, (uint8_t []){0x96, 0x16, 0x00, 0x00, 0xFF}, 5, 0},
    {0x96, (uint8_t []){0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x9A, 0x2D, 0x00, 0x44}, 12, 0},
    {0x97, (uint8_t []){0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x9A, 0x2D, 0x00, 0x44}, 12, 0},
    {0xBA, (uint8_t []){0x55, 0x9A, 0x2D, 0x9A, 0x2D}, 5, 0},
    {0x9A, (uint8_t []){0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0x9B, (uint8_t []){0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0x9C, (uint8_t []){0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00}, 13, 0},
    {0x9D, (uint8_t []){0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}, 8, 0},
    {0x9E, (uint8_t []){0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}, 7, 0},
    {0xB4, (uint8_t []){0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10}, 12, 0},
    {0xB5, (uint8_t []){0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12}, 12, 0},
    {0xB6, (uint8_t []){0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF}, 7, 0},
    {0x86, (uint8_t []){0xC6, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5}, 14, 0},
    {0xB7, (uint8_t []){0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D}, 16, 0},
    {0xB8, (uint8_t []){0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D}, 16, 0},
    {0xB9, (uint8_t []){0x23, 0x23}, 2, 0},
    {0xBF, (uint8_t []){0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B}, 6, 0},
    {0xF2, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x04, 0xDA, 0x12, 0x54, 0x47}, 5, 0},
    {0x77, (uint8_t []){0x6B, 0x5B, 0xFD, 0xC3, 0xC5}, 5, 0},
    {0x7A, (uint8_t []){0x15, 0x27}, 2, 0},
    {0x7B, (uint8_t []){0x04, 0x57}, 2, 0},
    {0x7E, (uint8_t []){0x01, 0x0E}, 2, 0},
    {0xBF, (uint8_t []){0x36}, 1, 0},
    {0xE3, (uint8_t []){0x40, 0x40}, 2, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xD0, (uint8_t []){0x00}, 1, 0},
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0x3F}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x21, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
    {0x2C, (uint8_t []){0x00}, 0, 0},
    {0x3A, (uint8_t []){0x01}, 1, 0},
    {0x36, (uint8_t []){0x00}, 1, 0},   // MADCTL portrait (panel không hỗ trợ MV swap)
    {0x35, (uint8_t []){0x01}, 1, 20},
};

constexpr int kLcdQspiClockHz = 20 * 1000 * 1000;
constexpr bool kHoldBootProbePattern = false;
constexpr int kLcdWikiOutputVolume = 70;

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

class LcdWikiAudioCodec : public Es8311AudioCodec {
private:
    std::vector<int16_t> BuildDiagnosticTone(int tone_hz, int duration_ms) {
        const int sample_rate = output_sample_rate();
        const int amplitude = 8000;
        const int tone_samples = sample_rate * duration_ms / 1000;
        const int period = std::max(1, sample_rate / tone_hz);
        std::vector<int16_t> tone(tone_samples);
        for (int i = 0; i < tone_samples; ++i) {
            tone[i] = ((i % period) < (period / 2)) ? amplitude : -amplitude;
        }
        return tone;
    }

    void PlayDiagnosticSegment(const char* name, int pa_level, int tone_hz) {
        if (AUDIO_CODEC_PA_PIN != GPIO_NUM_NC) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(AUDIO_CODEC_PA_PIN, pa_level));
        }

        std::vector<int16_t> tone = BuildDiagnosticTone(tone_hz, 450);
        ESP_LOGI(TAG, "LCDWiki audio diagnostic segment start name=%s sample_rate=%d samples=%u volume=%d pa_pin=%d pa_level=%d pa_gpio_level=%d",
                 name, output_sample_rate(), static_cast<unsigned>(tone.size()),
                 output_volume(), AUDIO_CODEC_PA_PIN, pa_level,
                 AUDIO_CODEC_PA_PIN == GPIO_NUM_NC ? -1 : gpio_get_level(AUDIO_CODEC_PA_PIN));
        OutputData(tone);

        std::vector<int16_t> silence(output_sample_rate() / 10, 0);
        OutputData(silence);
    }

    void RunDiagnosticTone() {
        EnableOutput(true);

        PlayDiagnosticSegment("pa_low", 0, 660);
        PlayDiagnosticSegment("pa_high", 1, 880);
        PlayDiagnosticSegment("configured", AUDIO_CODEC_PA_INVERTED ? 0 : 1, 1100);

        std::vector<int16_t> silence(output_sample_rate() / 10, 0);
        OutputData(silence);
        ESP_LOGI(TAG, "LCDWiki audio diagnostic tone sequence end");
    }

public:
    LcdWikiAudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate,
        int output_sample_rate, gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
        gpio_num_t dout, gpio_num_t din, gpio_num_t pa_pin, uint8_t es8311_addr,
        bool use_mclk = true, bool pa_inverted = false)
        : Es8311AudioCodec(i2c_master_handle, i2c_port, input_sample_rate, output_sample_rate,
            mclk, bclk, ws, dout, din, pa_pin, es8311_addr, use_mclk, pa_inverted) {
        input_channels_ = 2;
        output_channels_ = 2;
    }

    void Start() override {
        Es8311AudioCodec::Start();
        if (output_volume() != kLcdWikiOutputVolume) {
            SetOutputVolume(kLcdWikiOutputVolume);
        }
        RunDiagnosticTone();
    }
};

// ST77922 QSPI yêu cầu cột (trục native) căn theo bội số 4. Khi xoay phần mềm, trục
// native có thể ứng với x HOẶC y của toạ độ logic -> căn CẢ HAI trục cho chắc.
void St77922RounderCallback(lv_area_t* area) {
    area->x1 = (area->x1 >> 2) << 2;
    area->y1 = (area->y1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
    area->y2 = ((area->y2 >> 2) << 2) + 3;
    area->x1 = std::max<int32_t>(0, area->x1);
    area->y1 = std::max<int32_t>(0, area->y1);
    area->x2 = std::min<int32_t>(DISPLAY_WIDTH - 1, area->x2);
    area->y2 = std::min<int32_t>(DISPLAY_HEIGHT - 1, area->y2);
}

// Hiển thị NGANG (landscape) bằng XOAY PHẦN MỀM của LVGL (panel ST77922 không xoay
// được bằng phần cứng). Panel vật lý 320x480; khi landscape, UI là 480x320 và LVGL
// xoay 90° từng mảnh khi flush. Rounder căn cột 4px (cả 2 trục) để ST77922 QSPI không nhiễu.
class St77922QspiDisplay : public LcdDisplay {
public:
    // width/height = kích thước LOGIC của UI (480x320 khi landscape).
    St77922QspiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
        int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool landscape)
        : LcdDisplay(panel_io, panel, width, height) {

        // Kích thước VẬT LÝ của panel (luôn 320x480 portrait).
        const int native_w = landscape ? height : width;
        const int native_h = landscape ? width : height;

        // Xoá nền (đen) toàn panel (toạ độ native) trước khi LVGL tiếp quản.
        std::vector<uint16_t> line(native_w, 0x0000);
        for (int y = 0; y < native_h; y++) {
            esp_lcd_panel_draw_bitmap(panel_, 0, y, native_w, y + 1, line.data());
        }

        ESP_LOGI(TAG, "Turning display on");
        esp_err_t err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(err);
        }

        DrawBootProbePattern();
        if (kHoldBootProbePattern) {
            ESP_LOGW(TAG, "LCDWiki boot probe diagnostic hold active");
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

        ESP_LOGI(TAG, "Adding ST77922 QSPI LCD (native %dx%d, %s)", native_w, native_h,
            landscape ? "landscape/sw-rotate" : "portrait");
        // hres/vres = native (panel vật lý). LVGL set_rotation(90) -> UI thành 480x320.
        lvgl_port_display_cfg_t display_cfg = {
            .io_handle = panel_io_,
            .panel_handle = panel_,
            .control_handle = nullptr,
            .buffer_size = static_cast<uint32_t>(native_w * 20),
            .double_buffer = false,
            .trans_size = 0,
            .hres = static_cast<uint32_t>(native_w),
            .vres = static_cast<uint32_t>(native_h),
            .monochrome = false,
            .rotation = {
                .swap_xy = false,
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
        display_cfg.flags.sw_rotate = landscape ? 1 : 0;  // gán runtime (tránh narrowing bitfield)

        display_ = lvgl_port_add_disp(&display_cfg);
        if (display_ == nullptr) {
            ESP_LOGE(TAG, "Failed to add display");
            return;
        }

        if (landscape) {
            lvgl_port_lock(0);
            lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90);  // đổi 270 nếu lộn đầu
            lvgl_port_unlock();
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
        io_config.pclk_hz = kLcdQspiClockHz;
        ESP_LOGI(TAG, "ST77922 QSPI clock: %d Hz", (int)io_config.pclk_hz);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        const st77922_vendor_config_t vendor_config = {
            .init_cmds = kSt77922InitCmds,
            .init_cmds_size = sizeof(kSt77922InitCmds) / sizeof(kSt77922InitCmds[0]),
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
        ESP_LOGW(TAG, "Touch disabled for LCDWiki stability; ST7123 read errors can abort lvgl_port_touchpad_read");
        return;

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
            auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
                state == kDeviceStateActivating || state == kDeviceStateConnecting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "LCDWiki BOOT long-press -> EnterWifiConfigMode");
            EnterWifiConfigMode();
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
        static LcdWikiAudioCodec audio_codec(i2c_bus_, AUDIO_CODEC_I2C_NUM,
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
        static LcdWikiBacklight backlight;
        return &backlight;
    }
};

} // namespace

DECLARE_BOARD(LCDWikiES3C35PBoard);
