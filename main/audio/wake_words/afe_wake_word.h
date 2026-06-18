#ifndef AFE_WAKE_WORD_H
#define AFE_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <model_path.h>

#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>

#include "audio_codec.h"
#include "wake_word.h"

class AfeWakeWord : public WakeWord {
public:
    AfeWakeWord();
    ~AfeWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list);
    void Feed(const std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    void Start();
    void Stop();
    size_t GetFeedSize();
    void EncodeWakeWordData();
    bool GetWakeWordOpus(std::vector<uint8_t>& opus);
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

private:
    srmodel_list_t *models_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    char* wakenet_model_ = NULL;
    std::vector<std::string> wake_words_;
    EventGroupHandle_t event_group_;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    int codec_input_channels_ = 1;
    int afe_feed_channels_ = 1;
    std::string last_detected_wake_word_;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;

    // Sticky stereo->mono channel selection. The stereo codec (e.g. ES8311 on the
    // LCDWiki board) duplicates a single physical mic across two I2S slots; the
    // other slot is silence/noise. Picking the louder channel PER CHUNK made the
    // choice flip-flop during the quiet onset of "Hi ESP" (both slots near-zero ->
    // noise wins), corrupting the wake word so the wakenet missed it -> "Hi ESP"
    // had to be repeated several times. We smooth per-channel energy (EMA) and only
    // switch slot with hysteresis, so the choice locks onto the real mic slot.
    std::vector<int64_t> channel_energy_ema_;
    int dominant_channel_ = -1;
    std::vector<int16_t> SelectDominantMonoChannel(const std::vector<int16_t>& data, int channels);

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    std::deque<std::vector<int16_t>> wake_word_pcm_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;
    TaskHandle_t audio_detection_task_handle_ = nullptr;

    void StoreWakeWordData(const int16_t* data, size_t size);
    void AudioDetectionTask();
};

#endif
