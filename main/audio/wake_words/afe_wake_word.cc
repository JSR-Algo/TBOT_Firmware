#include "afe_wake_word.h"
#include "audio_service.h"
#include <algorithm>
#include <esp_log.h>
#include <sstream>
#include <cmath>

#define DETECTION_RUNNING_EVENT 1

#define TAG "AfeWakeWord"

namespace {
constexpr float kHiEspWakeThreshold = 0.55f;
}  // namespace

std::vector<int16_t> AfeWakeWord::SelectDominantMonoChannel(const std::vector<int16_t>& data,
                                                           int channels) {
    if (channels <= 1 || data.empty()) {
        return data;
    }

    const size_t frames = data.size() / channels;
    if (frames == 0) {
        return {};
    }

    // Per-chunk energy per channel.
    std::vector<int64_t> energy(channels, 0);
    for (size_t frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            int32_t sample = data[frame * channels + channel];
            energy[channel] += static_cast<int64_t>(sample) * sample;
        }
    }

    // Smooth across chunks (EMA, alpha = 1/8) so the pick reflects the mic slot's
    // SUSTAINED energy, not one quiet/noisy chunk. Per-chunk picking flip-flopped
    // during the low-energy onset of "Hi ESP" and corrupted the wake word.
    if (static_cast<int>(channel_energy_ema_.size()) != channels) {
        channel_energy_ema_.assign(channels, 0);
        dominant_channel_ = -1;
    }
    int best = 0;
    for (int channel = 0; channel < channels; ++channel) {
        channel_energy_ema_[channel] +=
            (energy[channel] - channel_energy_ema_[channel]) >> 3;
        if (channel_energy_ema_[channel] > channel_energy_ema_[best]) {
            best = channel;
        }
    }

    // Hysteresis: only move the sticky slot when another is clearly (>50%) louder
    // on the smoothed energy, so a noisy idle chunk can't bounce it mid-utterance.
    if (dominant_channel_ < 0 || dominant_channel_ >= channels) {
        dominant_channel_ = best;
    } else if (best != dominant_channel_ &&
               channel_energy_ema_[best] >
                   channel_energy_ema_[dominant_channel_] +
                       (channel_energy_ema_[dominant_channel_] >> 1)) {
        dominant_channel_ = best;
    }

    const int selected = dominant_channel_;
    std::vector<int16_t> mono(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        mono[frame] = data[frame * channels + selected];
    }
    return mono;
}

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    Stop();
    if (audio_detection_task_handle_ != nullptr) {
        vTaskDelete(audio_detection_task_handle_);
        audio_detection_task_handle_ = nullptr;
    }

    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;
    codec_input_channels_ = std::max(1, codec_->input_channels());

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                // Rename built-in "Hi,ESP" to user's robot name for display purposes.
                // Trigger phonetics is still "Hi, ESP" (model is fixed) but UI/log shows "Hi, Tâm".
                if (word == "Hi,ESP" || word == "Hi ESP" || word == "hiesp") {
                    word = "Hi, Tâm";
                }
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    if (!codec_->input_reference() && codec_input_channels_ > 1) {
        input_format = "M";
        afe_feed_channels_ = 1;
    } else {
        for (int i = 0; i < codec_input_channels_ - ref_num; i++) {
            input_format.push_back('M');
        }
        for (int i = 0; i < ref_num; i++) {
            input_format.push_back('R');
        }
        afe_feed_channels_ = codec_input_channels_;
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_LOW_COST;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // VAD tuning for weaker mic - more sensitive to speech onset, longer noise tolerance
    afe_config->vad_init = true;
    afe_config->vad_min_speech_ms = 64;   // detect speech faster (default 80)
    afe_config->vad_min_noise_ms = 800;   // tolerate brief pauses (default 1000)
    afe_config->vad_delay_ms = 128;       // smaller pre-speech buffer
    // Keep wake-word inference light enough for feed/fetch to stay balanced on S3.
    afe_config->ns_init = false;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    // ponytail: fixed LCDWiki Hi ESP sensitivity; add board/env tuning if false wakes show up.
    int threshold_ret = afe_iface_->set_wakenet_threshold(afe_data_, 1, kHiEspWakeThreshold);
    ESP_LOGI(TAG, "hiesp_wakenet_threshold index=1 value=%.2f ret=%d",
             kHiEspWakeThreshold, threshold_ret);

    // Wake-word fetch task. Keep it above Idle so AFE fetch drains during
    // TTS/display load, but below audio_input and audio_communication so the
    // feed/uplink paths stay dominant.
    xTaskCreate([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 4096, this, tskIDLE_PRIORITY + 1, &audio_detection_task_handle_);

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

int32_t AfeWakeWord::GetDetectionTaskStackHighWaterMark() const {
    return audio_detection_task_handle_ == nullptr
               ? -1
               : static_cast<int32_t>(uxTaskGetStackHighWaterMark(audio_detection_task_handle_));
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!(xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT)) {
        return;
    }
    const std::vector<int16_t>* feed_data = &data;
    std::vector<int16_t> mono_data;
    if (!codec_->input_reference() && codec_input_channels_ > 1) {
        mono_data = SelectDominantMonoChannel(data, codec_input_channels_);
        feed_data = &mono_data;
    }
    input_buffer_.insert(input_buffer_.end(), feed_data->begin(), feed_data->end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * afe_feed_channels_;
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;;
        }

        // Store the wake word data for voice recognition, like who is speaking
        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            // RMS measurement (log-only mode): compute RMS so we can tune
            // the human-voice threshold from real user data, but DO NOT
            // reject — let every wake through. Once we observe real RMS
            // values from genuine "Hi ESP" vs noise, change the policy.
            int16_t* samples = res->data;
            int sample_count = (int)(res->data_size / sizeof(int16_t));
            int64_t sum_sq = 0;
            for (int i = 0; i < sample_count; ++i) {
                int32_t s = (int32_t)samples[i];
                sum_sq += (int64_t)(s * s);
            }
            int rms = 0;
            if (sample_count > 0) {
                rms = (int)sqrt((double)(sum_sq / (int64_t)sample_count));
            }
            ESP_LOGI(TAG, "Wake DETECTED rms=%d samples=%d (log-only mode)",
                     rms, sample_count);

            Stop();
            last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                return;
            }

            // Get frame size
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);

            // Encode all PCM data
            int packets = 0;
            std::vector<int16_t> in_buffer;
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};

            for (auto& pcm: this_->wake_word_pcm_) {
                if (in_buffer.empty()) {
                    in_buffer = std::move(pcm);
                } else {
                    in_buffer.reserve(in_buffer.size() + pcm.size());
                    in_buffer.insert(in_buffer.end(), pcm.begin(), pcm.end());
                }

                while (in_buffer.size() >= frame_size) {
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = (uint8_t *)(in_buffer.data());
                    in.len = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len = outbuf_size;
                    out.encoded_bytes = 0;

                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                    }

                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + frame_size);
                }
            }
            this_->wake_word_pcm_.clear();
            // Close encoder
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
