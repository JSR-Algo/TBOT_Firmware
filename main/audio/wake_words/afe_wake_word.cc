#include "afe_wake_word.h"
#include "audio_service.h"
#include <algorithm>
#include <esp_log.h>
#include <sstream>
#include <cmath>
#include <utility>

#define DETECTION_RUNNING_EVENT 1
#define SHUTDOWN_EVENT 2
#define DETECTION_EXITED_EVENT 4
#define ENCODE_EXITED_EVENT 8
#define DETECTION_STOPPED_EVENT 16

#define TAG "AfeWakeWord"

namespace {
constexpr float kHiEspWakeThreshold = 0.55f;
constexpr uint32_t kDiagnosticSpeechFloorRms = 100;
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
    xEventGroupSetBits(event_group_, DETECTION_EXITED_EVENT | ENCODE_EXITED_EVENT |
                                        DETECTION_STOPPED_EVENT);
}

AfeWakeWord::~AfeWakeWord() {
    Shutdown(UINT32_MAX);

    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (models_ != nullptr && owns_models_) {
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
        owns_models_ = true;
    } else {
        models_ = models_list;
        owns_models_ = false;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
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
    char* configured_wakenet_models[] = {
        afe_config->wakenet_model_name,
        afe_config->wakenet_model_name_2,
    };
    wake_words_by_model_.clear();
    for (char* model_name : configured_wakenet_models) {
        if (model_name == nullptr) {
            break;
        }
        std::vector<std::string> model_wake_words;
        const char* words = esp_srmodel_get_wake_words(models_, model_name);
        if (words != nullptr) {
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                // The fixed phonetics stay unchanged; only the user-facing label is renamed.
                if (word == "Hi,ESP" || word == "Hi ESP" || word == "hiesp") {
                    word = "Hi, Tâm";
                }
                model_wake_words.push_back(std::move(word));
            }
        }
        wake_words_by_model_.push_back(std::move(model_wake_words));
    }
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
    shutting_down_.store(false);
    xEventGroupClearBits(event_group_, DETECTION_EXITED_EVENT);
    const BaseType_t detection_created = xTaskCreate([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->audio_detection_task_handle_.store(
            xTaskGetCurrentTaskHandle(), std::memory_order_release);
        const EventGroupHandle_t exit_events = this_->event_group_;
        this_->AudioDetectionTask();
        {
            auto lifecycle_lock = this_->run_synchronization_.AcquireTransition();
            this_->audio_detection_task_handle_.store(nullptr, std::memory_order_release);
        }
        xEventGroupSetBits(exit_events, DETECTION_EXITED_EVENT);
        vTaskDelete(NULL);
    }, "audio_detection", 4096, this, tskIDLE_PRIORITY + 1, nullptr);
    if (detection_created != pdPASS) {
        xEventGroupSetBits(event_group_, DETECTION_EXITED_EVENT);
        audio_detection_task_handle_.store(nullptr, std::memory_order_release);
        return false;
    }

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

int32_t AfeWakeWord::GetDetectionTaskStackHighWaterMark() const {
    auto lifecycle_lock = run_synchronization_.AcquireTransition();
    const TaskHandle_t task_handle =
        audio_detection_task_handle_.load(std::memory_order_acquire);
    return task_handle == nullptr
               ? -1
               : static_cast<int32_t>(uxTaskGetStackHighWaterMark(task_handle));
}

WakeWordProgress AfeWakeWord::GetProgress() {
    return {
        feed_count_.load(std::memory_order_relaxed),
        fetch_count_.load(std::memory_order_relaxed),
        run_generation_.load(std::memory_order_relaxed),
        telemetry_.TakeSnapshot(),
    };
}

void AfeWakeWord::Start() {
    auto lifecycle_lock = run_synchronization_.AcquireTransition();
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }
    run_synchronization_.BeginStart(run_generation_);
    xEventGroupClearBits(event_group_, DETECTION_STOPPED_EVENT);
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    auto lifecycle_lock = run_synchronization_.AcquireTransition();
    const bool was_running =
        (xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) != 0;
    const uint32_t stop_generation = run_synchronization_.BeginStopAndClear(
        run_generation_,
        [this]() { xEventGroupClearBits(event_group_, DETECTION_STOPPED_EVENT); },
        [this]() { xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT); });

    const TaskHandle_t detection_task =
        audio_detection_task_handle_.load(std::memory_order_acquire);
    if (!was_running || xTaskGetCurrentTaskHandle() == detection_task) {
        run_synchronization_.PublishStopped(stopped_generation_, stop_generation);
        xEventGroupSetBits(event_group_, DETECTION_STOPPED_EVENT);
    } else {
        const int64_t wait_started_us = esp_timer_get_time();
        const bool stopped = run_synchronization_.WaitForStopAcknowledgement(
            stopped_generation_, stop_generation, kStopAckTimeoutMs,
            [wait_started_us]() {
                return static_cast<uint32_t>(
                    (esp_timer_get_time() - wait_started_us) / 1000);
            },
            [this](uint32_t remaining_ms) {
                xEventGroupWaitBits(event_group_, DETECTION_STOPPED_EVENT,
                                    pdTRUE, pdTRUE,
                                    std::max<TickType_t>(
                                        1, pdMS_TO_TICKS(remaining_ms)));
            });
        if (!stopped) {
            ESP_LOGE(TAG,
                     "afe stop acknowledgement timeout generation=%lu feed=%lu fetch=%lu",
                     static_cast<unsigned long>(run_generation_.load(std::memory_order_relaxed)),
                     static_cast<unsigned long>(feed_count_.load(std::memory_order_relaxed)),
                     static_cast<unsigned long>(fetch_count_.load(std::memory_order_relaxed)));
            return;
        }
    }

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
    const int expected_bytes = static_cast<int>(chunk_size * sizeof(int16_t));
    while (input_buffer_.size() >= chunk_size) {
        const int accepted_bytes = afe_iface_->feed(afe_data_, input_buffer_.data());
        const bool fully_accepted = accepted_bytes == expected_bytes;
        if (fully_accepted) {
            telemetry_.ObserveFeedChunk(input_buffer_.data(), chunk_size,
                                        kDiagnosticSpeechFloorRms, esp_timer_get_time());
            feed_count_.fetch_add(1, std::memory_order_relaxed);
        }
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
        auto bits = xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT | SHUTDOWN_EVENT,
                                        pdFALSE, pdFALSE, portMAX_DELAY);
        if ((bits & SHUTDOWN_EVENT) || shutting_down_.load()) {
            break;
        }

        while (xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) {
            const uint32_t fetch_generation =
                run_generation_.load(std::memory_order_acquire);
            auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(kFetchWaitMs));
            const EventBits_t current_bits = xEventGroupGetBits(event_group_);
            if ((current_bits & SHUTDOWN_EVENT) || shutting_down_.load()) {
                break;
            }
            if (!(current_bits & DETECTION_RUNNING_EVENT) ||
                fetch_generation != run_generation_.load(std::memory_order_acquire) ||
                !run_synchronization_.IsCurrent(run_generation_, fetch_generation)) {
                continue;
            }
            if (res == nullptr || res->ret_value == ESP_FAIL) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            auto lifecycle_lock = run_synchronization_.TryAcquireTransition();
            if (!lifecycle_lock.owns_lock()) {
                continue;
            }
            if (!(xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) ||
                !run_synchronization_.IsCurrent(run_generation_, fetch_generation)) {
                continue;
            }
            fetch_count_.fetch_add(1, std::memory_order_relaxed);

            WakeDecisionCategory decision_category;
            switch (res->wakeup_state) {
                case WAKENET_NO_DETECT:
                    decision_category = WakeDecisionCategory::kNone;
                    break;
                case WAKENET_CHANNEL_VERIFIED:
                    decision_category = WakeDecisionCategory::kTransition;
                    break;
                case WAKENET_DETECTED:
                    decision_category = WakeDecisionCategory::kDetected;
                    break;
                default:
                    decision_category = WakeDecisionCategory::kOther;
                    break;
            }
            telemetry_.ObserveWakeState(decision_category, res->wakenet_model_index,
                                        static_cast<int>(wake_words_by_model_.size()));

            // Store the wake word data for voice recognition, like who is speaking
            StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

            if (res->wakeup_state == WAKENET_DETECTED) {
                const std::string* wake_word = ResolveWakeWordLabel(
                    wake_words_by_model_, res->wakenet_model_index,
                    res->wake_word_index);
                if (wake_word == nullptr) {
                    size_t word_count = 0;
                    if (res->wakenet_model_index >= 1 &&
                        res->wakenet_model_index <=
                            static_cast<int>(wake_words_by_model_.size())) {
                        word_count = wake_words_by_model_[res->wakenet_model_index - 1].size();
                    }
                    ESP_LOGW(TAG,
                             "Wake detection returned invalid indices model=%d model_count=%d word=%d word_count=%u",
                             res->wakenet_model_index,
                             static_cast<int>(wake_words_by_model_.size()),
                             res->wake_word_index,
                             static_cast<unsigned>(word_count));
                    continue;
                }
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

                if (!(xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) ||
                    fetch_generation != run_generation_.load(std::memory_order_acquire)) {
                    continue;
                }
                Stop();
                last_detected_wake_word_ = *wake_word;

                if (wake_word_detected_callback_) {
                    wake_word_detected_callback_(last_detected_wake_word_);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        run_synchronization_.PublishStopped(
            stopped_generation_, run_generation_.load(std::memory_order_acquire));
        xEventGroupSetBits(event_group_, DETECTION_STOPPED_EVENT);
        if ((xEventGroupGetBits(event_group_) & SHUTDOWN_EVENT) || shutting_down_.load()) {
            break;
        }
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
    if (shutting_down_.load() || encode_active_.exchange(true)) {
        return;
    }
    const size_t stack_size = 4096 * 6;
    xEventGroupClearBits(event_group_, ENCODE_EXITED_EVENT);
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        wake_word_opus_.clear();
    }
    const BaseType_t encode_created = xTaskCreateWithCaps([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        const EventGroupHandle_t exit_events = this_->event_group_;
        const auto finish = [this_]() {
            this_->encode_active_.store(false);
            this_->wake_word_encode_task_ = nullptr;
        };
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                    this_->wake_word_cv_.notify_all();
                }
                finish();
                xEventGroupSetBits(exit_events, ENCODE_EXITED_EVENT);
                vTaskDeleteWithCaps(nullptr);
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
        finish();
        xEventGroupSetBits(exit_events, ENCODE_EXITED_EVENT);
        vTaskDeleteWithCaps(nullptr);
    }, "encode_wake_word", stack_size, this, 2, &wake_word_encode_task_, MALLOC_CAP_SPIRAM);
    if (encode_created != pdPASS) {
        wake_word_encode_task_ = nullptr;
        encode_active_.store(false);
        xEventGroupSetBits(event_group_, ENCODE_EXITED_EVENT);
    }
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return shutting_down_.load() || !wake_word_opus_.empty();
    });
    if (wake_word_opus_.empty()) {
        return false;
    }
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}

bool AfeWakeWord::Shutdown(uint32_t timeout_ms) {
    shutting_down_.store(true);
    Stop();
    xEventGroupSetBits(event_group_, SHUTDOWN_EVENT);
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        wake_word_opus_.push_back({});
    }
    wake_word_cv_.notify_all();

    const EventBits_t required = DETECTION_EXITED_EVENT | ENCODE_EXITED_EVENT;
    const TickType_t wait_ticks = timeout_ms == UINT32_MAX
                                      ? portMAX_DELAY
                                      : pdMS_TO_TICKS(timeout_ms);
    const EventBits_t exited = xEventGroupWaitBits(event_group_, required, pdFALSE, pdTRUE,
                                                   wait_ticks);
    return (exited & required) == required;
}
