#include "afe_audio_processor.h"
#include <algorithm>
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01
#define PROCESSOR_EXITED 0x02

#define TAG "AfeAudioProcessor"

std::vector<int16_t> AfeAudioProcessor::SelectDominantMonoChannel(const std::vector<int16_t>& data,
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
    // SUSTAINED energy, not one quiet/noisy chunk.
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

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (initialization_attempted_) return;
    initialization_attempted_ = true;
    if (!codec || !event_group_) return;
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    codec_input_channels_ = std::max(1, codec_->input_channels());

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

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

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = models ? esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL) : nullptr;
    char* vad_model_name = models ? esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL) : nullptr;
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!afe_config) return;
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (!afe_iface_) { afe_config_free(afe_config); return; }
    afe_data_ = afe_iface_->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (!afe_data_) return;
    
    task_created_ = xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, tskIDLE_PRIORITY + 9, NULL) == pdPASS;
}

AfeAudioProcessor::~AfeAudioProcessor() {
    shutdown_.store(true);
    if (event_group_) xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
    if (task_created_) {
        xEventGroupWaitBits(event_group_, PROCESSOR_EXITED, pdFALSE, pdTRUE, portMAX_DELAY);
    }
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (event_group_) vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data, ChatCaptureTag tag) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!IsRunning() || tag != capture_tag_) {
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

void AfeAudioProcessor::Start() {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    if (!IsCaptureReady()) return;
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    if (!event_group_) return;
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    std::lock_guard<std::mutex> fetch(fetch_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
    output_buffer_.clear();
}

void AfeAudioProcessor::PrepareCapture(ChatCaptureTag tag) {
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    std::lock_guard<std::mutex> fetch(fetch_mutex_);
    if (afe_data_ != nullptr) afe_iface_->reset_buffer(afe_data_);
    input_buffer_.clear();
    output_buffer_.clear();
    capture_tag_ = tag;
}

bool AfeAudioProcessor::IsRunning() {
    return event_group_ && (xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING);
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data, ChatCaptureTag)> callback) {
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (!shutdown_.load()) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);
        if (shutdown_.load()) break;
        FetchAudio();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    xEventGroupSetBits(event_group_, PROCESSOR_EXITED);
}

void AfeAudioProcessor::FetchAudio() {
    // Transition order: dispatch -> feed -> fetch. The SDK supports concurrent
    // feed/fetch; reset excludes both. Callbacks must not reenter Stop/Prepare.
    std::lock_guard<std::mutex> dispatch(callback_mutex_);
    std::unique_lock<std::mutex> lock(fetch_mutex_);
    if (!IsRunning() || afe_data_ == nullptr) return;
    // Never wait for feed while holding its mutex. SDK ring reset does not
    // promise to erase DSP/filter history; this fences owned samples only.
    const auto tag = capture_tag_;
    auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
    if (res == nullptr || res->ret_value == ESP_FAIL) return;
    auto output = output_callback_;
    auto vad = vad_state_change_callback_;
    const bool speaking = res->vad_state == VAD_SPEECH ? true :
        res->vad_state == VAD_SILENCE ? false : is_speaking_;
    const bool vad_changed = speaking != is_speaking_;
    is_speaking_ = speaking;
    std::vector<std::vector<int16_t>> frames;
    if (output) {
        const size_t samples = res->data_size / sizeof(int16_t);
        output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
        while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
            frames.emplace_back(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
    lock.unlock();
    if (vad && vad_changed) vad(speaking);
    for (auto& frame : frames) output(std::move(frame), tag);
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    std::lock_guard<std::mutex> fetch(fetch_mutex_);
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
