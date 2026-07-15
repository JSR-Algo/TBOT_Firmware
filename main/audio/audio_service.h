#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "wake_word_lifecycle_controller.h"
#include "protocol.h"
#include "ogg_demuxer.h"

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
// Give the low-priority Opus codec task slack so a brief stall on the mic path
// doesn't immediately drop PCM (was 2 -> tiny, caused growing encode_drop).
#define MAX_ENCODE_TASKS_IN_QUEUE 4
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
// TTS jitter buffer restored to 7200ms. The server bursts a whole response
// faster than realtime (~6s seen), and a 720ms cap dropped ~95 frames mid-burst
// -> choppy/stuttering speech. Barge-in stays INSTANT regardless of depth: the
// codec drops stale-generation packets at dequeue (gen-gating) and AbortSpeaking
// clears the queue, so a large backlog never delays an interrupt.
#define MAX_DECODE_PACKETS_IN_QUEUE (7200 / OPUS_FRAME_DURATION_MS)
// Mic uplink backlog restored to 2400ms so a transient codec stall buffers
// rather than dropping the user's question (was 720ms -> growing encode_drop).
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,                                                          \
        .channel            = ESP_AUDIO_MONO,                                                                     \
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        .bitrate            = ESP_OPUS_BITRATE_AUTO,                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                     \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = true,                                                                               \
        .enable_vbr         = true,                                                                               \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t incoming_decode_packet_count = 0;
    uint32_t decode_drop_count = 0;
    uint32_t decode_count = 0;
    uint32_t decode_fail_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
    uint32_t encode_drop_count = 0;   // mic frames dropped when encode queue stayed full
    uint32_t stale_frame_count = 0;   // decode frames dropped by barge-in gen-gate
};

struct AudioTaskStackHighWaterMarks {
    int32_t audio_input = -1;
    int32_t audio_output = -1;
    int32_t opus_codec = -1;
    int32_t afe_detection = -1;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    std::string GetLastWakeWord();
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool WaitForPlaybackQueueEmpty(uint32_t timeout_ms = 0);
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    // Materialize the AFE wake-word pipeline (create_from_config + spawn the
    // audio_detection fetch task) WITHOUT starting it. Hoisted off the Idle-time
    // prio-10 state transition onto the prio-2 activation task after boot HTTP.
    // After this, the Idle EnableWakeWordDetection(true) only has to Start()
    // (cheap), so the very first "Hi ESP" lands first try instead of racing AFE
    // init. Must NOT Start() / set AS_EVENT_WAKE_WORD_RUNNING — the FEED ring
    // stays empty until the locked Idle gate enables the mic, so the BLE/AFE
    // contention gate is untouched. Caller MUST gate on IsDeviceClaimed().
    using WakeWordPrewarmToken = WakeWordLifecycleController::PrewarmToken;
    using WifiProvisioningToken = WakeWordLifecycleController::ProvisioningToken;
    WakeWordPrewarmToken CaptureWakeWordPrewarmToken() const;
    void PrewarmWakeWord(WakeWordPrewarmToken token);
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);
    WifiProvisioningToken BeginWifiProvisioning();
    bool EndWifiProvisioningAndRearm(WifiProvisioningToken token);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    // Phat truc tiep PCM 16-bit mono o output_sample_rate (24kHz) — dung cho SFX
    // game doc tu SD (WAV). Chia frame va day thang vao playback queue, khong qua
    // opus decoder. Best-effort: bo qua neu service dung hoac playback queue day.
    void QueuePcmForPlayback(const std::vector<int16_t>& pcm);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);
    // Publish the active response generation. Decode frames whose stamped
    // generation differs are dropped at dequeue (barge-in stale-frame guard).
    void SetPlaybackGeneration(uint32_t generation) { playback_generation_.store(generation); }
    // Periodic-observability snapshot, read from the app task. The counters copy
    // is racy-but-benign (aligned 32-bit reads); queue depths take the lock.
    DebugStatistics GetDebugStatistics() const { return debug_statistics_; }
    void GetQueueDepths(uint32_t& decode, uint32_t& send, uint32_t& playback);
    AudioTaskStackHighWaterMarks GetTaskStackHighWaterMarks();

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    WakeWordLifecycleController wake_word_lifecycle_;
    std::mutex wake_word_control_mutex_;
    std::atomic<WakeWord*> wake_word_feed_target_{nullptr};
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    
    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex task_handle_mutex_;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;

    // Active response generation for barge-in gen-gating. Written via
    // SetPlaybackGeneration() (app/WS task), read on the codec task at dequeue.
    std::atomic<uint32_t> playback_generation_{0};

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void CreateWakeWordIfAvailable();
    void FeedWakeWord(const std::vector<int16_t>& data);
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
