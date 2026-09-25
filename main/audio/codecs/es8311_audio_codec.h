#ifndef _ES8311_AUDIO_CODEC_H
#define _ES8311_AUDIO_CODEC_H

#include "audio_codec.h"
#include "es8311_write_timing.h"
#include "es8311_data_adapter.h"

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>
#include <memory>


class Es8311AudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    std::unique_ptr<Es8311DataAdapter> checked_data_if_;
    std::atomic<uint32_t>* tx_eof_count_ = nullptr;
    std::unique_ptr<Es8311HardwareDrain> output_drain_;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t dev_ = nullptr;
    gpio_num_t pa_pin_ = GPIO_NUM_NC;
    bool pa_inverted_ = false;
    uint32_t write_count_ = 0;
    Es8311WriteTiming write_timing_;
    std::mutex data_if_mutex_;
    std::mutex read_mutex_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    void UpdateDeviceState();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true, bool pa_inverted = false);
    virtual ~Es8311AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    AudioOutputDrainSnapshot GetOutputDrainSnapshot() const override;
    bool SupportsChatOutputDrain() const override { return true; }
    bool ResetOutputDrain() override;
};

#endif // _ES8311_AUDIO_CODEC_H
