from pathlib import Path
import subprocess
import pytest
from test_protocol_work_lifetime import method

ROOT=Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("cue_name,expected_packets",[("popup.ogg",9),("exclamation.ogg",15),("vibration.ogg",14)])
def test_actual_cue_atomic_admission(tmp_path,cue_name,expected_packets):
    source=(ROOT / "main/audio/audio_service.cc").read_text()
    fixture='''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "audio/chat_playback_reset.h"
#include "audio/demuxer/ogg_demuxer.h"
constexpr int MAX_DECODE_PACKETS_IN_QUEUE=120,OPUS_FRAME_DURATION_MS=60;
constexpr int AUDIO_POWER_CHECK_INTERVAL_MS=1000;
void esp_timer_stop(int) {} void esp_timer_start_periodic(int,int) {}
std::atomic<uint64_t> cue_now{100};unsigned clock_calls=0;bool expire_final=false;
uint64_t esp_timer_get_time(){if(expire_final && ++clock_calls==3)return 1000;return cue_now.load();}
struct AudioStreamPacket {int sample_rate=0,frame_duration=0;uint32_t generation=0,chat_reset_token=0;std::vector<uint8_t> payload;};
struct AudioService {
 enum class ChatCueResult {Accepted,Busy,Stale,Failed};
 std::atomic<bool> service_stopped_{false};int audio_power_timer_=0;
 struct Codec {bool enabled=false;std::function<void()> enable_hook;bool output_enabled(){return enabled;}void EnableOutput(bool value){if(enable_hook)enable_hook();enabled=value;}} codec;
 Codec* codec_=&codec;
 ChatPlaybackReset chat_playback_reset_;std::atomic<uint32_t> playback_generation_{1};
 std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
 std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
 ChatCueResult TryPlayChatCue(std::string_view,uint32_t,uint32_t,uint64_t=10000100);
};
'''
    fixture+=method(source,"AudioService::ChatCueResult AudioService::TryPlayChatCue")
    fixture+='''
int main(int argc,char** argv){assert(argc==3);std::ifstream f(argv[1],std::ios::binary);std::string cue((std::istreambuf_iterator<char>(f)),{});
 AudioService service;auto token=service.chat_playback_reset_.Request();assert(service.chat_playback_reset_.Complete(token));
 service.service_stopped_=true;
 assert(service.TryPlayChatCue(cue,1,token)==AudioService::ChatCueResult::Failed);
 assert(!service.codec.enabled && service.audio_decode_queue_.empty());service.service_stopped_=false;
 std::promise<void> enabling,resume;auto resumed=resume.get_future();
 service.codec.enable_hook=[&]{enabling.set_value();resumed.wait();};
 auto stopped_during_enable=std::async(std::launch::async,[&]{return service.TryPlayChatCue(cue,1,token);});
 enabling.get_future().wait();service.service_stopped_=true;resume.set_value();
 assert(stopped_during_enable.get()==AudioService::ChatCueResult::Failed);
 assert(service.audio_decode_queue_.empty());service.service_stopped_=false;service.codec.enable_hook={};
 service.codec.enabled=false;
 std::promise<void> deadline_enabling,deadline_resume;auto deadline_resumed=deadline_resume.get_future();
 service.codec.enable_hook=[&]{deadline_enabling.set_value();deadline_resumed.wait();};
 auto expired_enable=std::async(std::launch::async,[&]{return service.TryPlayChatCue(cue,1,token,1000);});
 deadline_enabling.get_future().wait();cue_now=1000;deadline_resume.set_value();
 assert(expired_enable.get()==AudioService::ChatCueResult::Failed && service.audio_decode_queue_.empty());
 service.codec.enable_hook={};
 assert(service.TryPlayChatCue(cue,1,token,1000)==AudioService::ChatCueResult::Failed);
 cue_now=100;
 expire_final=true;clock_calls=0;
 assert(service.TryPlayChatCue(cue,1,token,1000)==AudioService::ChatCueResult::Failed);
 assert(service.audio_decode_queue_.empty());expire_final=false;
 service.audio_queue_mutex_.lock();
 auto contended=std::async(std::launch::async,[&]{return service.TryPlayChatCue(cue,1,token);});
 assert(contended.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 assert(contended.get()==AudioService::ChatCueResult::Busy);service.audio_queue_mutex_.unlock();
 for(int i=0;i<MAX_DECODE_PACKETS_IN_QUEUE;++i)service.audio_decode_queue_.push_back(std::make_unique<AudioStreamPacket>());
 assert(service.TryPlayChatCue(cue,1,token)==AudioService::ChatCueResult::Busy);
 assert(service.audio_decode_queue_.size()==MAX_DECODE_PACKETS_IN_QUEUE);
 service.audio_decode_queue_.clear();assert(service.TryPlayChatCue(cue,1,token)==AudioService::ChatCueResult::Accepted);
 auto count=service.audio_decode_queue_.size();assert(count==std::stoul(argv[2]));assert(service.codec.enabled);
 service.chat_playback_reset_.Request();assert(service.TryPlayChatCue(cue,1,token)==AudioService::ChatCueResult::Stale);
 assert(service.audio_decode_queue_.size()==count);
}
'''
    generated=tmp_path / "cue.cc";generated.write_text(fixture);binary=tmp_path / "cue"
    subprocess.run(["c++","-std=c++17","-pthread","-fsanitize=address,undefined","-DESP_LOGD(...)=((void)0)","-I",str(ROOT / "main"),"-I",str(ROOT / "tests/native_stubs"),str(generated),str(ROOT / "main/audio/demuxer/ogg_demuxer.cc"),"-o",str(binary)],check=True)
    subprocess.run([str(binary),str(ROOT / "main/assets/common" / cue_name),str(expected_packets)],check=True,timeout=15)
