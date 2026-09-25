#include "audio/chat_playback_reset.h"
#include "audio/audio_decode_fence.h"
#include "audio/audio_reset_epoch_publication.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>
template<class... T> void Log(T&&...) {}
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_LOGD(...) Log(__VA_ARGS__)
#define ESP_LOGI(...) Log(__VA_ARGS__)
const char* TAG="test";
constexpr int MAX_PLAYBACK_TASKS_IN_QUEUE=4, ESP_AUDIO_ERR_OK=0, ESP_AUDIO_ERR_FAIL=-1;
constexpr int MAX_SEND_PACKETS_IN_QUEUE=4;
constexpr int ESP_AUDIO_DEC_RECOVERY_NONE=0, kAudioTaskTypeDecodeToPlaybackQueue=0;
bool ShouldLogAudioDiagnostic(unsigned) { return false; }
using esp_ae_sample_t=int16_t*;
struct esp_audio_dec_in_raw_t { uint8_t* buffer; uint32_t len; int consumed, frame_recover; };
struct esp_audio_dec_out_frame_t { uint8_t* buffer; uint32_t len, decoded_size; };
struct esp_audio_dec_info_t {};
static unsigned decodes=0;
static std::function<void()> decode_hook;
static std::function<void()> decode_claim_hook, open_hook;
static std::function<void()> reset_claim_hook;
static std::function<void()> decode_dequeued_hook;
static unsigned reset_calls=0, open_calls=0;
int esp_opus_dec_reset(void*) { ++reset_calls; return ESP_AUDIO_ERR_OK; }
void esp_opus_dec_close(void*) {}
struct esp_opus_dec_cfg_t { int rate,duration; };
#define OPUS_DEC_CFG(rate,duration) esp_opus_dec_cfg_t{rate,duration}
int esp_opus_dec_open(esp_opus_dec_cfg_t*, size_t, void** decoder) {
    ++open_calls; if (open_hook) open_hook(); *decoder=reinterpret_cast<void*>(1); return 0;
}
constexpr int ESP_AUDIO_MONO=1;
struct esp_ae_rate_cvt_cfg_t { int in,out,channels; };
#define RATE_CVT_CFG(in,out,channels) esp_ae_rate_cvt_cfg_t{in,out,channels}
void esp_ae_rate_cvt_close(void*) {}
constexpr int ESP_AE_ERR_OK=0;
int esp_ae_rate_cvt_reset(void*) { return ESP_AE_ERR_OK; }
int esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t*,void**) { return 0; }
struct Codec { int output_sample_rate() { return 24000; } };
struct Board {
    static Board& GetInstance() { static Board b; return b; }
    Codec* GetAudioCodec() { static Codec c; return &c; }
};
int esp_opus_dec_decode(void*, esp_audio_dec_in_raw_t*, esp_audio_dec_out_frame_t* out, esp_audio_dec_info_t*) {
    ++decodes; if (decode_hook) decode_hook(); out->decoded_size=2; return ESP_AUDIO_ERR_OK;
}
void esp_ae_rate_cvt_get_max_out_sample_num(void*, uint32_t, uint32_t*) {}
void esp_ae_rate_cvt_process(void*, int16_t*, size_t, int16_t*, uint32_t*) {}
struct AudioStreamPacket {
    uint32_t generation=1, chat_reset_token=0, timestamp=0;
    int sample_rate=24000, frame_duration=60;
    bool conversation_audio=true;
    std::vector<uint8_t> payload{1};
};
struct AudioTask {
    int type=0;
    uint32_t timestamp=0,response_generation=0,chat_reset_token=0;
    bool conversation_audio=false;
    std::vector<int16_t> pcm;
};
struct AudioService {
    ChatPlaybackReset chat_playback_reset_;
    AudioDecodeFence audio_decode_fence_;
    AudioResetEpochPublication audio_reset_epoch_publication_;
    std::mutex audio_queue_mutex_, decoder_mutex_, chat_decode_transition_mutex_;
    std::condition_variable audio_queue_cv_;
    std::atomic<uint32_t> playback_generation_{1};
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    std::deque<int> audio_encode_queue_,audio_send_queue_;
    bool service_stopped_=false;
    bool CanRun() const {
        // PRODUCTION_WAIT_PREDICATE
    }
    struct { unsigned stale_frame_count=0,decode_count=0,decode_fail_count=0; } debug_statistics_;
    void* opus_decoder_=this;
    void* output_resampler_=nullptr;
    int decoder_frame_size_=1,decoder_sample_rate_=24000,decoder_duration_ms_=60;
    Codec codec;
    Codec* codec_=&codec;
    void SetDecodeSampleRate(int,int);
    bool ResetChatDecoder(uint32_t);
    bool Once() {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        bool decoded_for_playback=false;
        do {
            // PRODUCTION_DECODE
        } while (false);
        return decoded_for_playback;
    }
    void Queue(uint32_t token) {
        auto packet=std::make_unique<AudioStreamPacket>(); packet->chat_reset_token=token;
        audio_decode_queue_.push_back(std::move(packet));
    }
};
// PRODUCTION_METHODS
int main() {
    AudioService pending;
    auto token=pending.chat_playback_reset_.Request();
    pending.Queue(token);
    assert(!pending.Once() && decodes==0 && pending.audio_decode_queue_.size()==1);
    assert(pending.ResetChatDecoder(token));
    assert(pending.Once() && decodes==1);
    assert(pending.audio_playback_queue_.front()->chat_reset_token==token);
    AudioService old;
    auto a=old.chat_playback_reset_.Request(); old.ResetChatDecoder(a); old.Queue(a);
    std::promise<void> entered, release;
    auto released=release.get_future().share();
    decode_hook=[&]{entered.set_value();released.wait();};
    auto worker=std::async(std::launch::async,[&]{return old.Once();});
    entered.get_future().wait();
    auto b=old.chat_playback_reset_.Request();
    release.set_value(); assert(!worker.get()); decode_hook={};
    assert(old.audio_playback_queue_.empty());
    old.ResetChatDecoder(b);
    old.Queue(a); assert(!old.Once()); assert(decodes==2);
    old.Queue(b); assert(old.Once()); assert(decodes==3);
    AudioService legacy;
    legacy.Queue(0); assert(legacy.Once() && decodes==4);

    AudioService claimed;
    auto c=claimed.chat_playback_reset_.Request(); claimed.ResetChatDecoder(c); claimed.Queue(c);
    std::promise<void> claim_entered,claim_release;
    auto claim_released=claim_release.get_future().share();
    decode_claim_hook=[&]{claim_entered.set_value();claim_released.wait();};
    auto late=std::async(std::launch::async,[&]{return claimed.Once();});
    claim_entered.get_future().wait();
    auto d=claimed.chat_playback_reset_.Request(); assert(claimed.ResetChatDecoder(d));
    const auto calls_before=decodes;
    claim_release.set_value(); assert(!late.get()); decode_claim_hook={};
    assert(decodes==calls_before);
    claimed.Queue(d); assert(claimed.Once() && decodes==calls_before+1);

    // Actual legacy decoder reconfiguration opens outside decoder_mutex_.
    // The outer transition still serializes it against chat reset.
    AudioService configuring;
    configuring.decoder_sample_rate_=16000;
    configuring.Queue(0);
    std::promise<void> open_entered,open_release;
    auto open_released=open_release.get_future().share();
    open_hook=[&]{open_entered.set_value();open_released.wait();};
    auto configure=std::async(std::launch::async,[&]{return configuring.Once();});
    open_entered.get_future().wait();
    auto r=configuring.chat_playback_reset_.Request();
    std::promise<void> reset_attempt;
    reset_claim_hook=[&]{reset_attempt.set_value();};
    auto reset=std::async(std::launch::async,[&]{return configuring.ResetChatDecoder(r);});
    reset_attempt.get_future().wait();
    assert(reset.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    open_release.set_value(); assert(!configure.get()); assert(reset.get()); open_hook={};
    reset_claim_hook={};
    assert(open_calls==1);

    AudioService legacy_pending;
    legacy_pending.Queue(0);
    auto requested=legacy_pending.chat_playback_reset_.Request();
    assert(!legacy_pending.CanRun());
    assert(!legacy_pending.Once());
    legacy_pending.audio_encode_queue_.push_back(1);
    assert(legacy_pending.CanRun());
    assert(legacy_pending.audio_decode_queue_.size()==1);
    assert(legacy_pending.ResetChatDecoder(requested));
    assert(legacy_pending.Once());

    AudioService legacy_claimed;
    legacy_claimed.Queue(0);
    std::promise<void> legacy_entered,legacy_release;
    auto legacy_released=legacy_release.get_future().share();
    decode_claim_hook=[&]{legacy_entered.set_value();legacy_released.wait();};
    auto legacy_late=std::async(std::launch::async,[&]{return legacy_claimed.Once();});
    legacy_entered.get_future().wait();
    auto latest=legacy_claimed.chat_playback_reset_.Request();
    assert(legacy_claimed.ResetChatDecoder(latest));
    const auto legacy_calls_before=decodes;
    legacy_release.set_value(); assert(!legacy_late.get()); decode_claim_hook={};
    assert(decodes==legacy_calls_before);
    legacy_claimed.Queue(latest); assert(legacy_claimed.Once());

    // Publish between dequeue and the old late watermark sample, then let
    // cleanup complete before this claimant acquires the transition lock.
    AudioService dequeue_race;
    dequeue_race.Queue(0);
    std::promise<void> dequeued, resume_dequeue, claim_ready, resume_claim;
    auto dequeue_release=resume_dequeue.get_future().share();
    auto claimant_release=resume_claim.get_future().share();
    decode_dequeued_hook=[&]{dequeued.set_value();dequeue_release.wait();};
    decode_claim_hook=[&]{claim_ready.set_value();claimant_release.wait();};
    auto crossed=std::async(std::launch::async,[&]{return dequeue_race.Once();});
    dequeued.get_future().wait();
    auto crossing_token=dequeue_race.chat_playback_reset_.Request();
    resume_dequeue.set_value();
    claim_ready.get_future().wait();
    assert(dequeue_race.ResetChatDecoder(crossing_token));
    const auto crossing_decodes=decodes;
    const auto crossing_opens=open_calls;
    resume_claim.set_value(); assert(!crossed.get());
    decode_claim_hook={}; decode_dequeued_hook={};
    assert(decodes==crossing_decodes && open_calls==crossing_opens);
    dequeue_race.Queue(crossing_token); assert(dequeue_race.Once());
}
