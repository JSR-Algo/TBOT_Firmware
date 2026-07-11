#include "es8311_audio_codec.h"

#include <algorithm>
#include <esp_err.h>
#include <esp_log.h>
#include <vector>

#define TAG "Es8311AudioCodec"

Es8311AudioCodec::Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk, bool pa_inverted) {
    duplex_ = true; // 是否双工
    input_reference_ = false; // 是否使用参考输入，实现回声消除
    input_channels_ = 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    pa_inverted_ = pa_inverted;
    input_gain_ = 40;  // increased from 30 for better mic sensitivity (built-in MEMS LMA2718 is weak)

    assert(input_sample_rate_ == output_sample_rate_);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = pa_inverted_;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Es8311AudioCodec");
    } else {
        ESP_LOGI(TAG, "Es8311AudioCodec initialized");
    }
}

Es8311AudioCodec::~Es8311AudioCodec() {
    esp_codec_dev_delete(dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8311AudioCodec::UpdateDeviceState() {
    bool opened = false;
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != NULL);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = static_cast<uint8_t>(std::max(input_channels_, output_channels_)),
            .channel_mask = 0,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, input_gain_));
        // Stock top is 0 dB; +2 dB is a small step for quieter Live TTS without
        // the PA clipping we hit at +4/+12 dB. No extra PCM digital multiply.
        {
            static esp_codec_dev_vol_map_t s_vol_map[] = {
                {.vol = 0, .db_value = -50.0f},
                {.vol = 100, .db_value = 2.0f},
            };
            esp_codec_dev_vol_curve_t curve = {
                .vol_map = s_vol_map,
                .count = 2,
            };
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_set_vol_curve(dev_, &curve));
        }
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
        opened = true;
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        esp_codec_dev_close(dev_);
        dev_ = nullptr;
    }
    if (dev_ != nullptr && input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_set_in_gain(dev_, input_gain_));
    }
    if (dev_ != nullptr && output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_set_out_vol(dev_, output_volume_));
    }
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        int pa_level = pa_inverted_ ? !level : level;
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(pa_pin_, pa_level));
        ESP_LOGI(TAG, "es8311_state opened=%d input_enabled=%d output_enabled=%d dev=%p pa_pin=%d pa_level=%d pa_gpio_level=%d pa_inverted=%d volume=%d",
                 opened, input_enabled_, output_enabled_, dev_, pa_pin_, pa_level,
                 gpio_get_level(pa_pin_), pa_inverted_, output_volume_);
    } else {
        ESP_LOGI(TAG, "es8311_state opened=%d input_enabled=%d output_enabled=%d dev=%p pa_pin=-1 volume=%d",
                 opened, input_enabled_, output_enabled_, dev_, output_volume_);
    }
}

void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2    
				.ext_clk_freq_hz = 0,
			#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2   
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8311AudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, volume));
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es8311AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311AudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t)));
        // The built-in MEMS mic (LMA2718) is very weak even at max analog PGA
        // (input_gain_ = 40): captured speech lands around rms 15-41, far below
        // what the wake-word AFE and the cloud Gemini Live VAD/STT need (it sees
        // the input as near-silence and fragments turns -> no response). Apply a
        // digital post-gain so the downstream pipeline gets a usable level.
        // Speech peaks stay well under int16 max at this factor; hard-clamp guards
        // the rare loud transient.
        constexpr int kInputDigitalGain = 4;  // reduced from 8: 8x amplified noise+speaker-echo, dragging turns to max_capture
        for (int i = 0; i < samples; ++i) {
            int32_t v = static_cast<int32_t>(dest[i]) * kInputDigitalGain;
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            dest[i] = static_cast<int16_t>(v);
        }
    }
    return samples;
}

int Es8311AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        if (dev_ == nullptr) {
            ESP_LOGE(TAG, "es8311_write failed reason=device_not_open samples=%d", samples);
            return 0;
        }
        const int16_t* write_data = data;
        int write_samples = samples;
        std::vector<int16_t> stereo_data;
        if (output_channels_ == 2) {
            stereo_data.resize(samples * 2);
            for (int i = 0; i < samples; ++i) {
                stereo_data[i * 2] = data[i];
                stereo_data[i * 2 + 1] = data[i];
            }
            write_data = stereo_data.data();
            write_samples = static_cast<int>(stereo_data.size());
        }
        esp_err_t ret = esp_codec_dev_write(dev_, (void*)write_data, write_samples * sizeof(int16_t));
        write_count_++;
        if (write_count_ <= 5 || (write_count_ % 20) == 0 || ret != ESP_OK) {
            ESP_LOGI(TAG, "es8311_write count=%lu samples=%d write_samples=%d bytes=%u channels=%d ret=%s(%d) output_enabled=%d volume=%d",
                     static_cast<unsigned long>(write_count_), samples, write_samples,
                     static_cast<unsigned>(write_samples * sizeof(int16_t)), output_channels_,
                     esp_err_to_name(ret), ret, output_enabled_, output_volume_);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(ret);
    }
    return samples;
}
