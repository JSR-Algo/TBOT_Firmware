#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    bool IsCaptureReady() const override {
        return event_group_ && afe_iface_ && afe_data_ && task_created_;
    }
    void Feed(std::vector<int16_t>&& data, ChatCaptureTag tag = {}) override;
    void PrepareCapture(ChatCaptureTag tag) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data, ChatCaptureTag)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    EventGroupHandle_t event_group_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data, ChatCaptureTag)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    int codec_input_channels_ = 1;
    int afe_feed_channels_ = 1;
    bool is_speaking_ = false;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    std::mutex callback_mutex_;
    std::mutex fetch_mutex_;
    std::vector<int16_t> output_buffer_;
    ChatCaptureTag capture_tag_{};
    std::atomic<bool> shutdown_{false};
    bool task_created_ = false;
    bool initialization_attempted_ = false;

    // Sticky stereo->mono channel selection (mirrors AfeWakeWord) — locks onto the
    // real mic slot instead of flip-flopping per chunk on the stereo codec, so the
    // post-wake conversation audio fed to AFE/VAD stays on the live microphone.
    std::vector<int64_t> channel_energy_ema_;
    int dominant_channel_ = -1;
    std::vector<int16_t> SelectDominantMonoChannel(const std::vector<int16_t>& data, int channels);

    void AudioProcessorTask();
    void FetchAudio();
};

#endif
