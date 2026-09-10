#pragma once

#include "es8311_hardware_drain.h"
#include <audio_codec_data_if.h>
#include <cstddef>

// Own only this forwarding object. The managed interface is deleted separately
// with its public deleter; dependency code must never free this C++ base address.
class Es8311DataAdapter : public audio_codec_data_if_t {
public:
    using DriverWrite = int (*)(void*, const void*, size_t, size_t*, uint32_t);
    Es8311DataAdapter(const audio_codec_data_if_t* inner, void* tx,
                      DriverWrite driver_write, Es8311HardwareDrain& drain)
        : inner_(inner), tx_(tx), driver_write_(driver_write), drain_(drain) {
        open = [](const audio_codec_data_if_t* h, void* cfg, int size) {
            const auto& a = Self(h);
            a.drain_.Invalidate();
            return a.Check(a.inner_->open(a.inner_, cfg, size));
        };
        is_open = [](const audio_codec_data_if_t* h) {
            const auto& a = Self(h);
            return a.inner_->is_open(a.inner_);
        };
        enable = [](const audio_codec_data_if_t* h, esp_codec_dev_type_t type, bool enabled) {
            const auto& a = Self(h);
            a.drain_.Invalidate();
            // Preserve managed duplex sequencing. Its intermediate driver
            // errors may be masked; only exact writes + fresh EOFs certify DMA.
            return a.Check(a.inner_->enable(a.inner_, type, enabled));
        };
        set_fmt = [](const audio_codec_data_if_t* h, esp_codec_dev_type_t type,
                     esp_codec_dev_sample_info_t* fs) {
            const auto& a = Self(h);
            a.drain_.Invalidate();
            return a.Check(a.inner_->set_fmt(a.inner_, type, fs));
        };
        read = [](const audio_codec_data_if_t* h, uint8_t* data, int size) {
            const auto& a = Self(h);
            return a.inner_->read(a.inner_, data, size);
        };
        write = [](const audio_codec_data_if_t* h, uint8_t* data, int size) {
            const auto& a = Self(h);
            a.drain_.BeginWrite();
            size_t written = 0;
            int result = ESP_CODEC_DEV_INVALID_ARG;
            if (a.tx_ && data && size > 0) {
                result = a.driver_write_(a.tx_, data, static_cast<size_t>(size), &written, 1000);
                if (result == 0 && written != static_cast<size_t>(size)) result = ESP_CODEC_DEV_WRITE_FAIL;
            }
            a.drain_.FinishWrite(result == 0);
            return result;
        };
        close = [](const audio_codec_data_if_t* h) {
            const auto& a = Self(h);
            a.drain_.Invalidate();
            return a.Check(a.inner_->close(a.inner_));
        };
    }
private:
    static const Es8311DataAdapter& Self(const audio_codec_data_if_t* h) {
        return *static_cast<const Es8311DataAdapter*>(h);
    }
    int Check(int result) const {
        if (result != 0) drain_.Fail();
        return result;
    }
    const audio_codec_data_if_t* inner_;
    void* tx_;
    DriverWrite driver_write_;
    Es8311HardwareDrain& drain_;
};
