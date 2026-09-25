#include "audio/chat_playback_reset.h"
#include "audio/audio_decode_fence.h"
#include "audio/audio_reset_epoch_publication.h"
#include "chat_start_handoff.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
template<class... T> void Log(T&&...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGD(...) Log(__VA_ARGS__)
const char* TAG="test";
constexpr int ESP_AUDIO_ERR_OK=0, MAX_DECODE_PACKETS_IN_QUEUE=8;
bool ShouldLogAudioDiagnostic(unsigned) { return false; }
static std::atomic<unsigned> resets{0};
static bool fail_reset=false;
static bool fail_resampler=false;
static unsigned resampler_resets=0;
constexpr int ESP_AE_ERR_OK=0;
int esp_ae_rate_cvt_reset(void*) { ++resampler_resets; return fail_resampler ? -1 : ESP_AE_ERR_OK; }
std::function<void()> reset_entry_hook;
std::function<void()> queue_entry_hook;
int esp_opus_dec_reset(void*) { ++resets; return fail_reset ? -1 : 0; }
struct AudioStreamPacket {
    uint32_t chat_reset_token=0, generation=0;
    int sample_rate=24000, frame_duration=60;
    bool conversation_audio=true;
    std::vector<uint8_t> payload{1};
};
struct AudioTask { uint32_t chat_reset_token=0, response_generation=0; };
struct AudioService {
    ChatPlaybackReset chat_playback_reset_;
    AudioDecodeFence audio_decode_fence_;
    AudioResetEpochPublication audio_reset_epoch_publication_;
    std::atomic<uint32_t> playback_generation_{1};
    std::mutex audio_queue_mutex_, decoder_mutex_, chat_decode_transition_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    void* opus_decoder_=this;
    void* output_resampler_=nullptr;
    struct { unsigned decode_drop_count=0, incoming_decode_packet_count=0; } debug_statistics_;
    uint32_t RequestChatPlaybackReset();
    bool ResetChatDecoder(uint32_t);
    bool PushChatPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>, uint32_t);
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>, bool=false);
    bool TryGetPlaybackResetEpoch(uint64_t&) const;
};
// PRODUCTION_METHODS
std::unique_ptr<AudioStreamPacket> Packet(uint32_t generation) {
    auto packet=std::make_unique<AudioStreamPacket>(); packet->generation=generation; return packet;
}
int main() {
    AudioService service;
    auto initial=service.RequestChatPlaybackReset();
    assert(initial && service.ResetChatDecoder(initial));
    assert(service.chat_playback_reset_.AllowsDecode(initial));
    assert(service.PushChatPacketToDecodeQueue(Packet(1), initial));
    assert(service.PushPacketToDecodeQueue(Packet(77))); // Legacy token zero survives chat cleanup.
    std::unique_lock<std::mutex> decoder(service.decoder_mutex_);
    auto second=service.RequestChatPlaybackReset();
    service.playback_generation_=2;
    ChatStartHandoff handoff;
    ChatStartHandoff::Request start{{1,7},1,1,0,100};
    assert(handoff.Publish(start));
    assert(handoff.Admit(start,2,second,101));
    ChatStartHandoff::Admission accepted;
    assert(handoff.TryAdmission(start,101,accepted));
    std::promise<void> entered;
    reset_entry_hook=[&]{entered.set_value();};
    auto resetter=std::async(std::launch::async,[&]{return service.ResetChatDecoder(second);});
    assert(entered.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready);
    auto intake=std::async(std::launch::async,[&]{return service.PushChatPacketToDecodeQueue(Packet(accepted.response_generation),accepted.reset_token);});
    assert(intake.wait_for(std::chrono::seconds(2))==std::future_status::ready && intake.get());
    uint64_t reset_epoch=99;
    assert(!service.TryGetPlaybackResetEpoch(reset_epoch) && reset_epoch==99);
    assert(!service.chat_playback_reset_.AllowsDecode(second));
    decoder.unlock(); assert(resetter.get());
    reset_entry_hook={};
    assert(service.audio_decode_queue_.size()==2);
    assert(service.audio_decode_queue_.front()->chat_reset_token==0);
    assert(service.audio_decode_queue_.back()->chat_reset_token==second);
    assert(service.chat_playback_reset_.AllowsDecode(second));
    assert(!service.chat_playback_reset_.AllowsDecode(initial));
    assert(service.TryGetPlaybackResetEpoch(reset_epoch) && reset_epoch==2);
    assert(service.ResetChatDecoder(second) && resets==2); // Same successful request is immutable.

    decoder.lock();
    auto third=service.RequestChatPlaybackReset();
    std::promise<void> entered_again;
    reset_entry_hook=[&]{entered_again.set_value();};
    auto old=std::async(std::launch::async,[&]{return service.ResetChatDecoder(third);});
    assert(entered_again.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready);
    auto fourth=service.RequestChatPlaybackReset();
    service.playback_generation_=4;
    assert(service.PushChatPacketToDecodeQueue(Packet(4),fourth));
    decoder.unlock(); assert(!old.get());
    reset_entry_hook={};
    assert(service.chat_playback_reset_.Pending());
    assert(service.ResetChatDecoder(fourth));
    assert(service.audio_decode_queue_.back()->chat_reset_token==fourth);
    assert(resets==3);

    auto failed=service.RequestChatPlaybackReset();
    const auto last_epoch=service.audio_decode_fence_.ResetEpoch();
    fail_reset=true;
    assert(!service.ResetChatDecoder(failed));
    assert(service.chat_playback_reset_.Pending() && service.audio_decode_fence_.ResetEpoch()==last_epoch);
    fail_reset=false; assert(service.ResetChatDecoder(failed));
    assert(!service.chat_playback_reset_.Pending());

    AudioService full;
    auto a=full.RequestChatPlaybackReset(); assert(full.ResetChatDecoder(a));
    for (int i=0;i<MAX_DECODE_PACKETS_IN_QUEUE;++i) assert(full.PushChatPacketToDecodeQueue(Packet(1),a));
    auto b=full.RequestChatPlaybackReset(); full.playback_generation_=2;
    assert(full.PushChatPacketToDecodeQueue(Packet(2),b));
    assert(full.audio_decode_queue_.size()==1);
    for (int i=1;i<MAX_DECODE_PACKETS_IN_QUEUE;++i) assert(full.PushChatPacketToDecodeQueue(Packet(2),b));
    assert(!full.PushChatPacketToDecodeQueue(Packet(2),b));

    AudioService admission;
    auto before=admission.RequestChatPlaybackReset();
    std::unique_lock<std::mutex> queue(admission.audio_queue_mutex_);
    std::promise<void> queue_entered;
    queue_entry_hook=[&]{queue_entered.set_value();};
    auto blocked=std::async(std::launch::async,[&]{return admission.PushChatPacketToDecodeQueue(Packet(1),before);});
    queue_entered.get_future().wait();
    admission.RequestChatPlaybackReset();
    queue.unlock(); assert(!blocked.get()); queue_entry_hook={};
    assert(admission.audio_decode_queue_.empty());

    AudioService resampling;
    resampling.output_resampler_=&resampling;
    auto resample_token=resampling.RequestChatPlaybackReset();
    fail_resampler=true;
    assert(!resampling.ResetChatDecoder(resample_token));
    assert(resampling.chat_playback_reset_.Pending());
    assert(resampling.audio_decode_fence_.ResetEpoch()==0);
    fail_resampler=false;
    assert(resampling.ResetChatDecoder(resample_token));
    assert(resampler_resets==2 && resampling.audio_decode_fence_.ResetEpoch()==1);
}
