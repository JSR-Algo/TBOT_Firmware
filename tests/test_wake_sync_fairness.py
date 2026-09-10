from pathlib import Path
import subprocess
import pytest
from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("sanitize", ["address,undefined", "thread"])
@pytest.mark.parametrize("voice_demo", [False, True])
def test_actual_quiet_admission_gives_running_wake_a_fixed_opportunity(tmp_path, sanitize, voice_demo):
    source = (ROOT / "main/application.cc").read_text()
    signatures = ["bool Application::BeginLessonAssetSyncQuiet", "void Application::EndLessonAssetSyncQuiet",
                  "void Application::ScheduleLessonAssetSyncWakeRearm()",
                  "void Application::ScheduleLessonAssetSyncWakeRearm(uint64_t delay_us)",
                  "void Application::RearmClaimedIdleWakeWord", "bool Application::SetDeviceState"]
    helper = "bool Application::HasLessonAssetSyncWakeOpportunity"
    methods = "\n".join(method(source, signature) for signature in signatures)
    methods += "\n" + (method(source, helper) if helper in source else
                         "bool Application::HasLessonAssetSyncWakeOpportunity(){return true;}")
    fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#include <thread>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using DeviceState=int;
constexpr int kDeviceStateIdle=0,kDeviceStateListening=1,kDeviceStateConnecting=2,kDeviceStateSpeaking=3;
constexpr int ESP_TIMER_TASK=0,ESP_OK=0;
uint64_t now=100;std::function<void()> clock_hook;int64_t esp_timer_get_time(){if(clock_hook)clock_hook();return now;}
struct esp_timer_create_args_t{void(*callback)(void*);void* arg;int dispatch_method;const char* name;bool skip_unhandled_events;};
struct Timer{esp_timer_create_args_t args{};uint64_t due=0;unsigned stops=0;};
using esp_timer_handle_t=Timer*;
int esp_timer_create(esp_timer_create_args_t* args,Timer** out){*out=new Timer{*args};return ESP_OK;}
void esp_timer_stop(Timer* timer){++timer->stops;timer->due=0;}
void esp_timer_start_once(Timer* timer,uint64_t delay){timer->due=now+delay;}
struct ConnectionSource{};
struct Signals{bool TrySource(ConnectionSource&){return true;}};
struct Application{
 std::atomic<bool> lesson_asset_sync_quiet_{false},lesson_runtime_active_{false},connect_in_flight_{false},reset_pending_{false};
 std::atomic<bool> chat_cleanup_enabled_{true},passive_ws_intent_{true},online_intent_{false},microphone_uplink_authorized_{false};
 std::atomic<bool> lesson_idle_repaint_suppressed_{false},tts_audio_accepting_{true};
 std::atomic<int64_t> listening_started_ms_{0},last_listening_activity_ms_{0};
 std::atomic<unsigned> speaking_generation_{1};
 bool lesson_asset_sync_wake_pending_=false;
 std::atomic<bool> lesson_asset_sync_wake_invalidated_{false};
 uint64_t lesson_asset_sync_wake_deadline_us_=0;
 uint32_t lesson_asset_sync_wake_revoked_=0;
 struct{uint32_t revoked=0;} chat_audio_desired_;
 Timer* lesson_asset_sync_wake_rearm_timer_=nullptr;
 std::atomic<int> state{kDeviceStateIdle};bool claimed=true,voice=false;
 struct Protocol{unsigned stops=0,resets=0;void SendStopListening(){++stops;}void ResetPassiveLiveness(){++resets;}bool IsAudioChannelOpened(){return true;}} transport;
 Protocol* protocol_=&transport;
 Signals signals;Signals* chat_protocol_signals_=&signals;
 struct Audio{bool wake=false,running=true;unsigned effects=0;
  bool IsRunning(){return running;}bool IsWakeWordRunning(){return wake;}
  void EnableVoiceProcessing(bool){++effects;}void EnableWakeWordDetection(bool enable){++effects;wake=enable;}
  void ResetDecoder(){++effects;}void* PopPacketFromSendQueue(){return nullptr;}
 } audio_service_;
 bool wake_requested=false;
 void RequestChatAudioCleanup(unsigned,bool,bool,bool wake){wake_requested=wake;}
 bool IsDeviceClaimed(){return claimed;}bool IsVoiceDetected(){return voice;}
 std::function<void()> state_hook;
 int GetDeviceState(){if(state_hook)state_hook();return state;}bool SetDeviceState(int next);
 struct Arms{void Cancel(){}} speaking_arm_dispatch_;
 struct Machine{std::atomic<int>& state;bool TransitionTo(int next){state=next;return true;}} state_machine_{state};
 std::vector<std::function<void()>> scheduled;
 void Schedule(std::function<void()> work){scheduled.push_back(work);}
 void FireTimer(){assert(lesson_asset_sync_wake_rearm_timer_ && now>=lesson_asset_sync_wake_rearm_timer_->due);lesson_asset_sync_wake_rearm_timer_->args.callback(this);for(auto& work:scheduled)work();scheduled.clear();}
 ~Application(){delete lesson_asset_sync_wake_rearm_timer_;}
 bool BeginLessonAssetSyncQuiet();void EndLessonAssetSyncQuiet();
 void ScheduleLessonAssetSyncWakeRearm();void ScheduleLessonAssetSyncWakeRearm(uint64_t);
 void RearmClaimedIdleWakeWord();bool HasLessonAssetSyncWakeOpportunity();
};
'''+methods+r'''
int main(){
 Application app;assert(app.BeginLessonAssetSyncQuiet());assert(!app.BeginLessonAssetSyncQuiet());
 app.EndLessonAssetSyncQuiet();auto* timer=app.lesson_asset_sync_wake_rearm_timer_;
 const auto due=timer->due;assert(due==1500100);const auto effects=app.audio_service_.effects,stops=timer->stops;
 // Every observed inter-sync gap is shorter than settling; refusals are inert.
 for(auto gap:{420000ULL,700000ULL,1000000ULL,1450000ULL}){now=100+gap;assert(!app.BeginLessonAssetSyncQuiet());assert(timer->due==due && timer->stops==stops && app.audio_service_.effects==effects);}
 now=due;app.FireTimer();assert(app.wake_requested && !app.audio_service_.wake);
 now+=10000000;assert(!app.BeginLessonAssetSyncQuiet()); // Requested/failed preparation is not running wake.
 app.audio_service_.wake=true;assert(!app.BeginLessonAssetSyncQuiet());
 const auto opportunity_end=now+3000000;
 for(unsigned retry=0;retry<26;++retry){now=opportunity_end-2900000+retry*100000;assert(!app.BeginLessonAssetSyncQuiet());}
 now=opportunity_end;assert(app.BeginLessonAssetSyncQuiet());assert(!app.audio_service_.wake);
 app.EndLessonAssetSyncQuiet(); // Worker allocation/start failure owes the same quiet cleanup/fairness.
 now+=1500000;app.FireTimer();assert(!app.BeginLessonAssetSyncQuiet());
 app.audio_service_.wake=true;assert(!app.HasLessonAssetSyncWakeOpportunity());
 now+=2000000;app.audio_service_.wake=false;assert(!app.HasLessonAssetSyncWakeOpportunity());
 app.audio_service_.wake=true;assert(!app.HasLessonAssetSyncWakeOpportunity());
 now+=2999999;assert(!app.BeginLessonAssetSyncQuiet());++now;assert(app.BeginLessonAssetSyncQuiet());
 app.EndLessonAssetSyncQuiet();const auto rescheduled=timer->due;app.EndLessonAssetSyncQuiet();assert(timer->due==rescheduled);
 now=rescheduled;app.FireTimer();app.audio_service_.wake=true;assert(!app.HasLessonAssetSyncWakeOpportunity());
 now+=2000000;app.SetDeviceState(kDeviceStateConnecting);app.SetDeviceState(kDeviceStateIdle);
 assert(!app.HasLessonAssetSyncWakeOpportunity());
 now+=2999999;assert(!app.BeginLessonAssetSyncQuiet());++now;assert(app.BeginLessonAssetSyncQuiet());
 app.EndLessonAssetSyncQuiet();now=timer->due;app.FireTimer();app.audio_service_.wake=true;
 assert(!app.HasLessonAssetSyncWakeOpportunity());now+=2000000;
 ++app.chat_audio_desired_.revoked; // A disable/restart completes between clock observations.
 assert(!app.HasLessonAssetSyncWakeOpportunity());now+=1000000;assert(!app.BeginLessonAssetSyncQuiet());
 now+=2000000;assert(app.BeginLessonAssetSyncQuiet());
 for(auto state:{kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking}){
  Application busy;busy.state=state;busy.voice=false;busy.online_intent_=true;
  assert(!busy.BeginLessonAssetSyncQuiet() && busy.audio_service_.effects==0 && !busy.lesson_asset_sync_quiet_);
 }
 Application legacy;legacy.chat_cleanup_enabled_=false;legacy.state=kDeviceStateListening;
 assert(legacy.BeginLessonAssetSyncQuiet() && legacy.transport.stops==1 && legacy.state==kDeviceStateIdle);
 Application legacy_active;legacy_active.chat_cleanup_enabled_=false;legacy_active.state=kDeviceStateListening;legacy_active.online_intent_=true;
 assert(!legacy_active.BeginLessonAssetSyncQuiet() && legacy_active.audio_service_.effects==0);
 Application public_sync;public_sync.claimed=false;
 for(int cycle=0;cycle<2;++cycle){assert(public_sync.BeginLessonAssetSyncQuiet());public_sync.EndLessonAssetSyncQuiet();assert(!public_sync.audio_service_.wake && !public_sync.wake_requested && !public_sync.lesson_asset_sync_wake_rearm_timer_);}
 Application unclaimed;assert(unclaimed.BeginLessonAssetSyncQuiet());unclaimed.EndLessonAssetSyncQuiet();
 unclaimed.claimed=false;assert(unclaimed.BeginLessonAssetSyncQuiet());unclaimed.EndLessonAssetSyncQuiet();
 assert(!unclaimed.audio_service_.wake && !unclaimed.wake_requested);
 Application raced;raced.lesson_asset_sync_wake_pending_=true;raced.audio_service_.wake=true;
 assert(!raced.HasLessonAssetSyncWakeOpportunity());const auto saved=raced.lesson_asset_sync_wake_deadline_us_;
 std::thread button([&]{raced.SetDeviceState(kDeviceStateConnecting);raced.SetDeviceState(kDeviceStateIdle);});button.join();
 assert(raced.lesson_asset_sync_wake_deadline_us_==saved); // Only the App owner may mutate its deadline.
 now+=2000000;assert(!raced.HasLessonAssetSyncWakeOpportunity());now+=3000000;
 clock_hook=[&]{std::thread departure([&]{raced.SetDeviceState(kDeviceStateConnecting);raced.SetDeviceState(kDeviceStateIdle);});departure.join();};
 assert(!raced.BeginLessonAssetSyncQuiet() && raced.audio_service_.effects==0);clock_hook={};
 assert(!raced.HasLessonAssetSyncWakeOpportunity());now+=3000000;assert(raced.BeginLessonAssetSyncQuiet());
 Application final_check;final_check.lesson_asset_sync_wake_pending_=true;final_check.audio_service_.wake=true;
 assert(!final_check.HasLessonAssetSyncWakeOpportunity());now+=3000000;unsigned reads=0;
 final_check.state_hook=[&]{if(++reads==2){std::thread departure([&]{final_check.SetDeviceState(kDeviceStateConnecting);final_check.SetDeviceState(kDeviceStateIdle);});departure.join();}};
 assert(!final_check.BeginLessonAssetSyncQuiet() && final_check.audio_service_.effects==0);
 Application stress;stress.lesson_asset_sync_wake_pending_=true;stress.audio_service_.wake=true;
 std::thread buttons([&]{for(int i=0;i<10000;++i){stress.SetDeviceState(kDeviceStateConnecting);stress.SetDeviceState(kDeviceStateIdle);}});
 for(int i=0;i<10000;++i)stress.HasLessonAssetSyncWakeOpportunity();buttons.join();
}
'''
    if voice_demo:
        fixture = fixture[:fixture.index("int main(){")] + r'''
int main(){
 Application app;app.audio_service_.wake=true;
 for(int i=0;i<100;++i) assert(!app.BeginLessonAssetSyncQuiet());
 assert(app.audio_service_.wake && !app.audio_service_.effects);
 assert(!app.lesson_asset_sync_quiet_ && !app.lesson_asset_sync_wake_rearm_timer_);
}
'''
    generated=tmp_path / "fairness.cc";generated.write_text(fixture);binary=tmp_path / "fairness"
    subprocess.run(["c++", "-std=c++17", "-pthread", f"-DCONFIG_TBOT_VOICE_DEMO={int(voice_demo)}", f"-fsanitize={sanitize}", str(generated), "-o", str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
