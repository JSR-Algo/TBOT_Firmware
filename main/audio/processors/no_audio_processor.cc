#include "no_audio_processor.h"
#include <esp_log.h>

#define TAG "NoAudioProcessor"

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data, ChatCaptureTag tag) {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    if (!is_running_ || !output_callback_) {
        return;
    }
    if (tag != capture_tag_) return;
    auto callback = output_callback_;
    std::vector<std::vector<int16_t>> frames;

    // Convert stereo to mono if needed
    if (codec_->input_channels() == 2) {
        for (size_t i = 0, j = 0; i < data.size() / 2; ++i, j += 2) {
            output_buffer_.push_back(data[j]);
        }
    } else {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    }

    // Output complete frames when buffer has enough data
    while (output_buffer_.size() >= (size_t)frame_samples_) {
        if (output_buffer_.size() == (size_t)frame_samples_) {
            frames.push_back(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            frames.emplace_back(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
    lock.unlock();
    for (auto& frame : frames) callback(std::move(frame), tag);
}

void NoAudioProcessor::PrepareCapture(ChatCaptureTag tag) {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    output_buffer_.clear();
    capture_tag_ = tag;
}

void NoAudioProcessor::Start() {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    is_running_ = true;
}

void NoAudioProcessor::Stop() {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    is_running_ = false;
    output_buffer_.clear();
}

bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data, ChatCaptureTag)> callback) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    output_callback_ = callback;
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    vad_state_change_callback_ = callback;
}

size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}
