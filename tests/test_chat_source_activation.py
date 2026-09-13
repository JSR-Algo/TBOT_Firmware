from pathlib import Path
import subprocess
import pytest
from test_protocol_work_lifetime import method

ROOT=Path(__file__).resolve().parents[1]

def test_lesson_producer_close_preserves_paused_queue_user(tmp_path):
    fixture=r'''
#include "lesson_queue_producer.h"
#include <cassert>
#include <future>
int main(){std::atomic<unsigned> readers{0};std::atomic<bool> stopped{false};
 std::promise<void> captured,resume;auto released=resume.get_future().share();
 auto producer=std::async(std::launch::async,[&]{LessonQueueProducer lease(readers,stopped);assert(lease);captured.set_value();released.wait();assert(readers==1);});
 captured.get_future().wait();stopped=true;assert(readers==1);
 {LessonQueueProducer rejected(readers,stopped);assert(!rejected);}
 assert(readers==1);resume.set_value();producer.get();assert(readers==0);
}
'''
    generated=tmp_path / "producer.cc";generated.write_text(fixture);binary=tmp_path / "producer"
    subprocess.run(["c++","-std=c++17","-pthread","-fsanitize=thread","-I",str(ROOT / "main"),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    source=(ROOT / "main/application.cc").read_text()
    for signature in ["void Application::EnqueueLessonMessage", "void Application::EnqueueLessonVisualCompletion", "void Application::EnqueueLessonEmbodiedCompletion", "void Application::RequestLessonStorageAbandonment"]:
        body=method(source,signature)
        assert body.index("LessonQueueProducer producer") < body.index("lesson_message_queue_")

def test_lesson_shutdown_waits_for_owned_work_before_task_delete(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"void Application::StopLessonMessageTask") if "void Application::StopLessonMessageTask" in source else "void Application::StopLessonMessageTask(){vTaskDelete(lesson_message_task_handle_);}"
    fixture=r'''
#include <atomic>
#include <cassert>
#include <memory>
#define CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P 1
constexpr int pdTRUE=1;
int pdMS_TO_TICKS(int value){return value;}
enum class LessonQueueItemKind{kAbandonTransport};
struct LessonQueueItem{LessonQueueItemKind kind;void* payload;unsigned transport_epoch;};
struct Application{void* lesson_message_task_handle_=reinterpret_cast<void*>(1);void* lesson_message_queue_=reinterpret_cast<void*>(2);std::atomic<bool> lesson_message_stop_{false},lesson_message_retired_{false};std::atomic<unsigned> lesson_message_producers_{1};void StopLessonMessageTask();};
Application* active;std::shared_ptr<int> permit;std::weak_ptr<int> observed;unsigned waits=0,deletes=0,wakes=0;
enum {eRunning,eSuspended};int scheduler_state=eRunning;int eTaskGetState(void*){return scheduler_state;}
int xQueueSendToFront(void*,LessonQueueItem*,int){assert(active->lesson_message_stop_ && active->lesson_message_producers_==0);++wakes;return pdTRUE;}
void vTaskDelay(int){assert(deletes==0);++waits;if(waits==1){assert(wakes==0);active->lesson_message_producers_=0;}else if(waits==2){permit.reset();active->lesson_message_retired_=true;}else scheduler_state=eSuspended;}
void vTaskDelete(void*){assert(observed.expired() && active->lesson_message_retired_ && scheduler_state==eSuspended);++deletes;}
'''+body+r'''
int main(){Application app;active=&app;permit=std::make_shared<int>(1);observed=permit;app.StopLessonMessageTask();assert(waits==3 && deletes==1 && wakes==1 && !app.lesson_message_task_handle_);app.StopLessonMessageTask();assert(deletes==1);}
'''
    generated=tmp_path / "lesson_shutdown.cc";generated.write_text(fixture);binary=tmp_path / "lesson_shutdown"
    subprocess.run(["c++","-std=c++17",str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    worker=method(source,"void Application::LessonMessageTask")
    assert "while (!self->lesson_message_stop_.load())" in worker
    assert worker.index("std::unique_ptr<ChatRequestContext>") < worker.index("self->lesson_message_retired_.store(true)")
    assert "StopLessonMessageTask();" in method(source,"Application::~Application")

def test_actual_hello_advertises_only_initialized_boolean_capability(tmp_path):
    source=(ROOT / "main/protocols/websocket_protocol.cc").read_text()
    body=method(source,"std::string WebsocketProtocol::GetHelloMessage")
    fixture=r'''
#include <cJSON.h>
#include <string>
#include <cassert>
constexpr int OPUS_FRAME_DURATION_MS=60;
bool IsValidEvidenceJourneyId(const std::string&){return false;}
struct WebsocketProtocol{bool conversation_audio_drain_ack_=false;int version_=1;std::string transient_evidence_journey_id_;std::string GetHelloMessage();};
'''+body+r'''
int main(){WebsocketProtocol protocol;for(bool ready:{false,true}){protocol.conversation_audio_drain_ack_=ready;auto text=protocol.GetHelloMessage();auto* root=cJSON_Parse(text.c_str());assert(root);auto* feature=cJSON_GetObjectItem(cJSON_GetObjectItem(root,"features"),"conversationAudioDrainAck");assert(ready ? cJSON_IsTrue(feature) : feature==nullptr);cJSON_Delete(root);}}
'''
    generated=tmp_path / "hello.cc";generated.write_text(fixture);binary=tmp_path / "hello"
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_selected_wifi_preparation_defers_audio_and_protocol():
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"bool Application::PrepareWifiConfigEntry")
    assert "if (chat_cleanup_enabled_)" in body
    assert body.index("RequestChatAudioCleanup") < body.index("SendStopListening")

def test_selected_periodic_metrics_use_nonwaiting_snapshot():
    source=(ROOT / "main/application.cc").read_text()
    metrics=source[source.index("// Print debug info every 10 seconds"):source.index("// Print debug info every 10 seconds")+600]
    assert "if (chat_cleanup_enabled_)" in metrics and "TryGetPlaybackDrainSnapshot" in metrics

def test_lesson_queue_retains_one_of_four_source_permits(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"void Application::EnqueueLessonMessage")
    fixture=r'''
#include "chat_inbound_messages.h"
#include "lesson_queue_producer.h"
#include <cassert>
#include <vector>
#define CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr int kLessonMessageQueueDepth=8,pdTRUE=1;
enum class LessonQueueItemKind{kFrame};
struct LessonQueueItem{LessonQueueItemKind kind;char* payload;uint64_t transport_epoch;void* source_context=nullptr;};
std::vector<LessonQueueItem> queued;bool full=false,send_fail=false;
unsigned uxQueueMessagesWaiting(void*){return full ? 8 : 0;}
int xQueueSend(void*,LessonQueueItem* item,int){if(send_fail)return 0;queued.push_back(*item);return 1;}
void LogLessonHeapBoundary(const char*,size_t){}
struct Application{void* lesson_message_queue_=reinterpret_cast<void*>(1);void* lesson_message_task_handle_=reinterpret_cast<void*>(1);unsigned faults=0;
 std::atomic<unsigned> lesson_message_producers_{0};std::atomic<bool> lesson_message_stop_{false};
 struct Admission{bool TryAcquire(){return true;}void Release(){}} lesson_queue_data_admission_;
 void FailChatRequest(const ChatRequestContext&){++faults;}
 void EnqueueLessonMessage(const cJSON*,uint64_t,ChatRequestContext);
};
'''+body+r'''
int main(){
 auto* root=cJSON_Parse("{\"type\":\"lesson_start\"}");ChatInboundMessages inbound;
 for(int failure=0;failure<3;++failure){Application app;full=failure==1;send_fail=failure==2;
  auto context=inbound.Own(root,{{1,7},1,1},100,77,"session");assert(context);std::weak_ptr<const ChatInboundMessage> permit=context;
  app.EnqueueLessonMessage(root,77,context);context.reset();
  if(failure){assert(permit.expired() && app.faults==1);}else{
   assert(!permit.expired() && inbound.Outstanding()==1 && queued.size()==1);
   auto item=queued.front();queued.clear();assert(item.transport_epoch==77);
   std::unique_ptr<ChatRequestContext> received(static_cast<ChatRequestContext*>(item.source_context));
   assert((*received)->lesson_epoch==77);cJSON_free(item.payload);received.reset();assert(permit.expired());
  }
 }
 Application stopped;stopped.lesson_message_stop_=true;
 auto context=inbound.Own(root,{{1,7},1,1},100,77,"session");
 stopped.EnqueueLessonMessage(root,77,context);context.reset();
 assert(stopped.faults==0 && queued.empty() && inbound.Outstanding()==0);
 cJSON_Delete(root);
}
'''
    generated=tmp_path / "lesson_queue.cc";generated.write_text(fixture);binary=tmp_path / "lesson_queue"
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_source_initialization_is_fail_closed_before_any_open():
    source=(ROOT / "main/application.cc").read_text()
    init=method(source,"void Application::InitializeProtocol")
    installed=init.index("if (!InitializeChatSourceRoute(is_websocket_protocol))")
    assert installed < init.index("StartPassiveLessonWebsocket();")
    assert installed < init.index("StartProtocolWorker();")
    failure=init[installed:init.index("// WebSocket Start()",installed)]
    assert "return;" in failure and "SetStatus" in failure

def test_lesson_continuations_keep_source_and_release_terminal_owner():
    lesson=(ROOT / "main/lesson_handler.cc").read_text()
    assert "g_session.source_context.reset();" in method(lesson,"void ClearTerminalLessonCursor")
    assert "IsChatRequestCurrent(g_session.source_context)" in method(lesson,"bool DispatchLessonEmbodiedCompletion")
    assert "IsChatRequestCurrent(g_session.source_context)" in method(lesson,"bool AcceptLessonVisualCompletion")
    handler=method(lesson,"void Application::HandleLessonMessage")
    assert "Schedule([" not in handler
    assert "ScheduleChatLesson(context," in handler

def test_source_registration_requires_all_workers_and_callbacks(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"bool Application::InitializeChatSourceRoute") if "bool Application::InitializeChatSourceRoute" in source else "bool Application::InitializeChatSourceRoute(bool){return false;}"
    fixture=r'''
#include "chat_protocol_signals.h"
#include <memory>
#include <atomic>
#include <cassert>
#include <functional>
#include <cJSON.h>
constexpr int OPUS_FRAME_DURATION_MS=60;
bool IsValidEvidenceJourneyId(const std::string&){return false;}
struct Protocol {struct SourceCallbacks{std::function<void()> audio,json,closed,opened,adopted,error;};bool installed=false;void SetSourceCallbacks(SourceCallbacks c){assert(c.audio&&c.json&&c.closed&&c.opened&&c.adopted&&c.error);installed=true;}};
struct WebsocketProtocol:Protocol {bool capable=false;bool& conversation_audio_drain_ack_=capable;int version_=1;std::string transient_evidence_journey_id_;std::string GetHelloMessage();void SetConversationAudioDrainAck(bool ready){assert(installed);capable=ready;}};
int open_channel_queue=1,open_channel_task=1;
struct Board {bool supported=true;static Board& GetInstance(){static Board board;return board;}Board* GetAudioCodec(){return this;}bool SupportsChatOutputDrain()const{return supported;}};
struct Application{
 std::unique_ptr<Protocol> protocol_{new WebsocketProtocol};std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
 std::atomic<uint64_t> protocol_generation_{1};std::atomic<bool> chat_cleanup_enabled_{false};
 bool chat_protocol_fault_=false,chat_protocol_infrastructure_fault_=false,chat_outbound_task_=true,audio_ready=true,callbacks_ready=true;
 bool InitializeChatAudioCleanupWorker(){return audio_ready;}
 Protocol::SourceCallbacks MakeChatSourceCallbacks(uint64_t,std::shared_ptr<ChatProtocolSignals>){Protocol::SourceCallbacks c;if(callbacks_ready)c.audio=c.json=c.closed=c.opened=c.adopted=c.error=[]{};return c;}
 bool InitializeChatSourceRoute(bool);
};
'''+body+method((ROOT / "main/protocols/websocket_protocol.cc").read_text(),"std::string WebsocketProtocol::GetHelloMessage")+r'''
bool HasCapability(Application& app){auto* protocol=static_cast<WebsocketProtocol*>(app.protocol_.get());auto text=protocol->GetHelloMessage();auto* root=cJSON_Parse(text.c_str());assert(root);auto* flag=cJSON_GetObjectItem(cJSON_GetObjectItem(root,"features"),"conversationAudioDrainAck");bool present=flag!=nullptr;assert(!present || cJSON_IsTrue(flag));cJSON_Delete(root);return present;}
int main(){
 for(int missing=0;missing<6;++missing){Application a;if(missing==0)a.chat_outbound_task_=false;if(missing==1)a.audio_ready=false;if(missing==2)open_channel_queue=0;if(missing==3)open_channel_task=0;if(missing==4)a.callbacks_ready=false;if(missing==5)a.chat_protocol_signals_.reset();
 assert(!a.InitializeChatSourceRoute(true));assert(!a.chat_cleanup_enabled_ && !a.protocol_->installed && !static_cast<WebsocketProtocol*>(a.protocol_.get())->capable);assert(a.chat_protocol_fault_ && a.chat_protocol_infrastructure_fault_);open_channel_queue=open_channel_task=1;}
 Application valid;assert(valid.InitializeChatSourceRoute(true));assert(valid.chat_cleanup_enabled_ && valid.protocol_->installed && HasCapability(valid));
 Application mqtt;assert(mqtt.InitializeChatSourceRoute(false));assert(!mqtt.chat_cleanup_enabled_ && !mqtt.protocol_->installed);
 Board::GetInstance().supported=false;Application unsupported;unsupported.chat_outbound_task_=false;
 assert(unsupported.InitializeChatSourceRoute(true));assert(!unsupported.chat_cleanup_enabled_ && !unsupported.protocol_->installed);
 assert(!static_cast<WebsocketProtocol*>(unsupported.protocol_.get())->capable && !unsupported.chat_protocol_infrastructure_fault_);
 assert(!HasCapability(unsupported) && !HasCapability(mqtt));
}
'''
    generated=tmp_path / "init.cc";generated.write_text(fixture);binary=tmp_path / "init"
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_selected_disconnect_and_idle_wake_do_not_enter_legacy_audio():
    source=(ROOT / "main/application.cc").read_text()
    disconnect=method(source,"void Application::HandleNetworkDisconnectedEvent")
    assert "RequestChatAudioCleanup" in disconnect
    assert "if (chat_cleanup_enabled_)" in disconnect
    wake=method(source,"void Application::RearmClaimedIdleWakeWord")
    assert wake.index("RequestChatAudioCleanup") < wake.index("EnableWakeWordDetection")

def test_lesson_capture_transition_arms_only_prepared_current_legacy_era(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    request=method(source,"bool Application::RequestChatLessonCapture") if "bool Application::RequestChatLessonCapture" in source else "bool Application::RequestChatLessonCapture(){return false;}"
    poll=method(source,"void Application::PollChatLessonCapture") if "void Application::PollChatLessonCapture" in source else "void Application::PollChatLessonCapture(uint64_t){}"
    fixture=r'''
#include "chat_inbound_messages.h"
#include "chat_protocol_signals.h"
#include "audio/chat_uplink_authorization.h"
#include <atomic>
#include <cassert>
uint64_t esp_timer_get_time(){return 100;}
#define ESP_LOGW(...) ((void)0)
constexpr int MAIN_EVENT_SEND_AUDIO=1,MAIN_EVENT_CHAT_OUTBOUND=2,kDeviceStateListening=4;
void xEventGroupSetBits(int,int){}
struct Application {
 std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
 ChatConnectionMessages::Owner chat_lesson_capture_owner_;uint64_t chat_lesson_capture_epoch_=0,chat_lesson_capture_deadline_us_=0;
 uint32_t chat_lesson_capture_token_=0,chat_audio_prepared_=0;bool chat_audio_fault_=false;int event_group_=0,state=4;
 std::atomic<uint32_t> speaking_generation_{1},connect_generation_{1};std::atomic<uint64_t> protocol_generation_{1};
 std::atomic<bool> lesson_asset_sync_quiet_{false},microphone_uplink_authorized_{false};bool lesson=true,current=true;
 struct Epoch{uint64_t epoch=7;uint64_t PublishedEpoch(){return epoch;}} lesson_transport_epoch_gate_;
 struct Audio {ChatUplinkAuthorization auth;bool ArmChatUplink(uint32_t token,bool scope){return auth.Arm(token,scope);}} audio_service_;
 struct Desired{uint32_t revoked=0;} chat_audio_desired_;enum class ChatWakePolicy{Listening};
 bool IsLessonVoiceRoute(){return lesson;}int GetDeviceState(){return state;}
 bool IsChatConnectionCurrent(ConnectionSource,uint64_t,uint32_t){return current;}
 uint32_t RequestChatAudioCleanup(uint32_t,bool reset,bool processing,bool,bool scope,bool,ChatWakePolicy){assert(!reset && processing && !scope);chat_audio_prepared_=0;return chat_audio_desired_.revoked=audio_service_.auth.Revoke();}
 bool RequestChatLessonCapture();void PollChatLessonCapture(uint64_t);
};
'''+request+poll+r'''
int main(){
 for(int change=0;change<5;++change){
  Application a;a.chat_protocol_signals_->EnableForSource({1,7});
  auto old=a.audio_service_.auth.Revoke();a.audio_service_.auth.AcknowledgePrepared(old,true);assert(a.audio_service_.auth.Arm(old,true));
  assert(a.RequestChatLessonCapture());auto pending=a.chat_lesson_capture_token_;
  assert(!a.audio_service_.auth.Accepts(a.audio_service_.auth.Capture()));
  a.PollChatLessonCapture(101);assert(!a.microphone_uplink_authorized_);
  a.audio_service_.auth.AcknowledgePrepared(pending,false);a.chat_audio_prepared_=pending;
  if(change==1)a.current=false;
  if(change==2)++a.lesson_transport_epoch_gate_.epoch;
  if(change==3)a.state=3;
  if(change==4)a.chat_audio_desired_.revoked=a.audio_service_.auth.Revoke();
  a.PollChatLessonCapture(102);
  assert(a.microphone_uplink_authorized_==(change==0));
  if(change==0){auto tag=a.audio_service_.auth.Capture();assert(!tag.chat_scope && a.audio_service_.auth.Accepts(tag));}
 }
 Application revoked;revoked.chat_protocol_signals_->EnableForSource({1,7});revoked.audio_service_.auth.Revoke();
 assert(revoked.RequestChatLessonCapture());auto token=revoked.chat_lesson_capture_token_;
 revoked.audio_service_.auth.AcknowledgePrepared(token,false);revoked.chat_audio_prepared_=token;revoked.PollChatLessonCapture(101);assert(revoked.microphone_uplink_authorized_);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";generated=tmp_path / "capture.cc";generated.write_text(fixture);binary=tmp_path / "capture"
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_passive_tick_allows_only_selected_chat_reservation():
    source=(ROOT / "main/application.cc").read_text()
    start=source.index("bool passive_liveness_failed = false;")
    end=source.index("if (!passive_liveness_failed",start)
    body=source[start:end]
    assert "BusyExcept(chat_outbound_reservation_)" in body
    assert "IsSelectedNormalChatRoute()" not in body
    assert "selected_chat_source" in body

def test_passive_ping_uses_bounded_connection_lane(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"bool Application::MaintainChatPassiveLiveness") if "bool Application::MaintainChatPassiveLiveness" in source else "bool Application::MaintainChatPassiveLiveness() { return true; }"
    fixture=r'''
#include "chat_inbound_messages.h"
#include "chat_protocol_signals.h"
#include <cassert>
#define ESP_LOGW(...) ((void)0)
struct Application {
 std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
 struct Protocol {int action=1;int ObserveChatPassiveLiveness(ConnectionSource){return action;}} protocol;Protocol* protocol_=&protocol;
 ChatConnectionMessages chat_connection_messages_;uint64_t chat_passive_ping_id_=0;unsigned sends=0;
 std::atomic<uint32_t> chat_source_connect_generation_{1};
 uint64_t RequestChatConnectionText(const std::string& text){assert(text=="{\"type\":\"ping\"}");++sends;return 1;}
 bool MaintainChatPassiveLiveness();
};
'''+body+r'''
int main(){
 Application app;assert(app.chat_protocol_signals_->EnableForSource({1,7}));
 assert(app.MaintainChatPassiveLiveness() && app.sends==1);
 assert(app.MaintainChatPassiveLiveness() && app.sends==1);
 app.chat_passive_ping_id_=0;app.protocol.action=0;assert(app.MaintainChatPassiveLiveness() && app.sends==1);
 app.protocol.action=-1;assert(!app.MaintainChatPassiveLiveness());
 Application full;full.chat_protocol_signals_->EnableForSource({1,7});
 full.chat_connection_messages_.Admit({{1,7},1,1},"a",100);full.chat_connection_messages_.Admit({{1,7},1,1},"b",100);
 assert(full.MaintainChatPassiveLiveness() && !full.sends);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";generated=tmp_path / "ping.cc";generated.write_text(fixture);binary=tmp_path / "ping"
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_robot_action_continuation_keeps_source(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"bool Application::HandleRobotActionMessage").replace("const cJSON* root)","const cJSON* root, ChatRequestContext context)")
    fixture=r'''
#include "chat_inbound_messages.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <vector>
#define ESP_LOGI(...) ((void)0)
struct Application {
 std::atomic<bool> lesson_runtime_active_{false};bool current=true;unsigned effects=0;
 std::vector<std::function<void()>> queued;
 bool IsChatRequestCurrent(const ChatRequestContext& c)const{return !c || current;}
 void Schedule(std::function<void()> f){queued.push_back(std::move(f));}
 bool SendHeadSetAngle(int){++effects;return true;}bool SendLeftArmSetPercent(int){++effects;return true;}bool SendRightArmSetPercent(int){++effects;return true;}bool SendBothArmsSetPercent(int){++effects;return true;}bool SendHeadSetPercent(int){++effects;return true;}
 bool SendLeftArmRaise(){++effects;return true;}bool SendRightArmRaise(){++effects;return true;}bool SendLeftArmLower(){++effects;return true;}bool SendRightArmLower(){++effects;return true;}bool SendBothArmsRaise(){++effects;return true;}bool SendBothArmsLower(){++effects;return true;}bool SendHeadTurnLeft(){++effects;return true;}bool SendHeadTurnRight(){++effects;return true;}bool SendHeadCenter(){++effects;return true;}
 bool HandleRobotActionMessage(const cJSON*,ChatRequestContext);
};
'''+body+r'''
int main(){
 for(const char* action:{"head_set_angle","left_arm_set_percent","both_arms_raise"}){
  Application app;auto context=std::make_shared<ChatInboundMessage>();std::weak_ptr<const ChatInboundMessage> permit=context;
  auto* root=cJSON_CreateObject();cJSON_AddStringToObject(root,"action",action);
  assert(app.HandleRobotActionMessage(root,context));cJSON_Delete(root);context.reset();assert(!permit.expired());
  app.current=false;for(auto& f:app.queued)f();app.queued.clear();assert(!app.effects && permit.expired());
 }
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o"
    generated=tmp_path / "robot.cc";generated.write_text(fixture);binary=tmp_path / "robot"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_source_failure_defers_audio_and_preserves_setup(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    body=method(source,"void Application::HandleChatSourceFailure") if "void Application::HandleChatSourceFailure" in source else "void Application::HandleChatSourceFailure() {}"
    fixture=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <array>
#define ESP_LOGW(...) ((void)0)
constexpr int kDeviceStateWifiConfiguring=1,kDeviceStateAudioTesting=2,kDeviceStateIdle=3,kDeviceStateListening=4,kDeviceStateSpeaking=5,kDeviceStateConnecting=6;
enum class PowerSaveLevel {LOW_POWER};
namespace Lang {namespace Strings {const char* PLEASE_WAIT="wait";const char* SERVER_UNAVAILABLE_RETRYING="retry";}namespace Sounds {const char* OGG_EXCLAMATION="cue";}}
struct Board {static Board& GetInstance(){static Board b;return b;}Board* GetDisplay(){return this;}void SetPowerSaveLevel(PowerSaveLevel){}void SetChatMessage(const char*,const char*){}void SetStatus(const char*){}void SetEmotion(const char*){}};
struct ProtocolWorkLifetime {enum class Action{kClose};unsigned closes=0;void Request(Action){++closes;}};
struct Application {
 // This boundary fixture leaves retained recovery inactive; test_chat_recovery
 // executes its actual methods and the open/cleanup/source lifecycle.
 struct ChatRecoveryIntent { enum class Kind { Background, Wake, Listen };Kind kind=Kind::Background;uint64_t received_us=0,deadline_us=0;int mode=0;bool read_wake=false;std::array<char,129> wake_text{};size_t wake_size=0; } chat_recovery_;
 enum class ChatRearmPhase { None, Pending };ChatRearmPhase chat_rearm_phase_=ChatRearmPhase::None;
 enum class ChatListenOrigin { Wake, User };ChatListenOrigin chat_listen_origin_=ChatListenOrigin::Wake;
 struct ChatOutboundMailbox { enum class Kind { Wake }; };
 struct Job { ChatOutboundMailbox::Kind kind=ChatOutboundMailbox::Kind::Wake;uint64_t deadline_us=0;std::array<char,129> payload{};size_t payload_size=0; } chat_rearm_job_;
 struct Control { Job job;bool resolve_wake=false; };struct Controls { const Control* Front(){return nullptr;} } chat_control_intents_;
 uint64_t chat_listen_received_us_=0;int chat_rearm_mode_=0;bool chat_rearm_voice_intent_=false;
 bool RetainChatRecovery(ChatRecoveryIntent::Kind,int){return false;}
 std::atomic<bool> tts_audio_accepting_{true},microphone_uplink_authorized_{true},backend_offline_{false},lesson_runtime_active_{false},passive_ws_intent_{false},online_intent_{true},connect_in_flight_{false},reconnect_passive_{false},lesson_interactive_listen_pending_{false},lesson_interactive_listening_active_{false};
 std::atomic<uint32_t> speaking_generation_{1},lesson_interactive_listen_generation_{0};
 uint64_t deferred_close_generation_=1;ProtocolWorkLifetime protocol_work_lifetime_;
 struct Gesture{void Cancel(){}} speaking_arm_dispatch_;int state=kDeviceStateSpeaking;
 unsigned cleanups=0,retires=0,reconnects=0,passive=0,cues=0,abandon=0;
 void RetireChatOutbound(){++retires;}void RequestChatAudioCleanup(uint32_t,bool,bool,bool){++cleanups;}
 void RequestLessonStorageAbandonment(){++abandon;}int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}
 bool ShouldKeepManagementHeartbeat(){return state==kDeviceStateIdle;}void StartHeartbeat(){}void StopHeartbeat(){}void DispatchDeviceHeartbeat(){}
 int GetDefaultListeningMode(){return 1;}void ScheduleReconnect(int,bool){++reconnects;}void SchedulePassiveLessonReconnect(){++passive;}
 bool RequestChatCue(const char*){++cues;return true;}void PollChatProtocolCleanup(){}
 void HandleChatSourceFailure();
};
'''+body+r'''
int main(){
 Application normal;normal.HandleChatSourceFailure();assert(!normal.tts_audio_accepting_ && !normal.microphone_uplink_authorized_ && normal.cleanups==1 && normal.retires==1);
 assert(normal.state==kDeviceStateIdle && normal.reconnects==1 && normal.protocol_work_lifetime_.closes==1);
 Application setup;setup.state=kDeviceStateWifiConfiguring;setup.HandleChatSourceFailure();assert(setup.state==kDeviceStateWifiConfiguring && !setup.reconnects && !setup.passive);
 Application lesson;lesson.lesson_runtime_active_=true;lesson.passive_ws_intent_=true;lesson.HandleChatSourceFailure();assert(lesson.passive==1 && !lesson.reconnects);
}
'''
    generated=tmp_path / "fault.cc";generated.write_text(fixture);binary=tmp_path / "fault"
    subprocess.run(["c++","-std=c++17",str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_actual_open_adoption_requires_current_live_published_source(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    poll=method(source,"void Application::PollChatSourceOpen") if "void Application::PollChatSourceOpen" in source else "void Application::PollChatSourceOpen(uint64_t) {}"
    fixture=r'''
#include "chat_protocol_signals.h"
#include <atomic>
#include <memory>
#include <cassert>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
enum class PowerSaveLevel {PERFORMANCE};
struct Board {static Board& GetInstance(){static Board b;return b;}unsigned saves=0;void SetPowerSaveLevel(PowerSaveLevel){++saves;}Board* GetAudioCodec(){return this;}int output_sample_rate(){return 24000;}};
struct Application {
 struct ChatRecoveryIntent { enum class Kind { None };Kind kind=Kind::None;bool attempted=false,adopted=false;uint64_t protocol_generation=0,lesson_generation=0;uint32_t connect_generation=0; } chat_recovery_;
 std::atomic<uint64_t> lesson_runtime_generation_{0};
 void CancelChatRecovery(){assert(false);}
 std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
 struct Protocol {uint32_t epoch=7;uint32_t CurrentConnectionEpoch(){return epoch;}} protocol;Protocol* protocol_=&protocol;
 uint32_t chat_source_open_handled_=0;std::atomic<uint32_t> connect_generation_{1},chat_source_connect_generation_{0};
 std::atomic<uint64_t> protocol_generation_{1};
 std::atomic<bool> chat_protocol_owned_{false},passive_ws_intent_{true},online_intent_{false},backend_offline_{true},microphone_uplink_authorized_{false};
 std::atomic<bool> lesson_runtime_active_{false},lesson_interactive_listen_pending_{false},lesson_interactive_listening_active_{false};
 bool suppressed=false,claimed=true;unsigned starts=0,stops=0,dispatches=0,claim_stops=0;
 struct {void Reset(){}} backend_recovery_window_;
 bool IsConnectSuccessPublicationSuppressed(){return suppressed;}
 bool IsDeviceClaimed(){return claimed;}void StartHeartbeat(){++starts;}void StopHeartbeat(){++stops;}void DispatchDeviceHeartbeat(){++dispatches;}void StopClaimPoll(){++claim_stops;}void DismissAlert(){}
 bool SelectChatProtocolSource(ConnectionSource source,uint64_t,uint32_t connect){chat_source_connect_generation_=connect;return chat_protocol_signals_->EnableForSource(source);}
 void PollChatSourceOpen(uint64_t);
};
'''+poll+r'''
int main(){
 Application app;assert(app.chat_protocol_signals_->PublishOpened({1,7},1,24000,250100));
 app.PollChatSourceOpen(100);assert(app.chat_protocol_signals_->MatchesSource({1,7}));
 assert(!app.online_intent_ && !app.microphone_uplink_authorized_ && app.starts==1 && app.claim_stops==1);
 app.PollChatSourceOpen(101);assert(app.starts==1);
 Application stale;stale.chat_protocol_signals_->PublishOpened({1,7},1,24000,250100);stale.connect_generation_=2;stale.PollChatSourceOpen(100);assert(!stale.chat_protocol_signals_->SourceSelected() && !stale.starts);
 Application closed;closed.chat_protocol_signals_->PublishOpened({1,7},1,24000,250100);closed.protocol.epoch=0;closed.PollChatSourceOpen(100);assert(!closed.starts);
 Application late;late.chat_protocol_signals_->PublishOpened({1,7},1,24000,250100);late.PollChatSourceOpen(250100);assert(!late.starts && !late.chat_protocol_signals_->SourceSelected());
 ChatProtocolSignals::Failure failure;
 assert(late.chat_protocol_signals_->ReadFailure(failure) && failure.source.source_id==1);
 Application held;held.chat_protocol_signals_->PublishOpened({1,7},1,24000,250100);held.chat_protocol_owned_=true;
 held.PollChatSourceOpen(100);assert(!held.starts);held.chat_protocol_owned_=false;
 held.PollChatSourceOpen(200);assert(held.starts==1);
 Application held_late;held_late.chat_protocol_signals_->PublishOpened({1,7},1,24000,250100);held_late.chat_protocol_owned_=true;
 held_late.PollChatSourceOpen(100);held_late.chat_protocol_owned_=false;held_late.PollChatSourceOpen(250100);
 assert(!held_late.starts && held_late.chat_protocol_signals_->ReadFailure(failure));
}
'''
    generated=tmp_path / "adopt.cc";generated.write_text(fixture);binary=tmp_path / "adopt"
    subprocess.run(["c++","-std=c++17","-pthread","-I",str(ROOT / "main"),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_owned_open_publication_is_latest_and_not_adoption(tmp_path):
    fixture=r'''
#include "chat_protocol_signals.h"
#include <cassert>
int main(){
 ChatProtocolSignals signals;ChatProtocolSignals::Opened open;
 assert(!signals.ReadOpened(open));
 assert(signals.PublishOpened({1,7},2,24000));
 assert(!signals.SourceSelected());assert(signals.ReadOpened(open));
 assert(open.source.source_id==1 && open.connect_generation==2 && open.sample_rate==24000);
 assert(signals.PublishOpened({2,8},3,16000));
 assert(!signals.PublishOpened({1,7},2,24000));assert(signals.ReadOpened(open));
 assert(open.source.source_id==2 && open.source.connection_epoch==8 && open.connect_generation==3);
    assert(signals.EnableForSource(open.source));assert(signals.MatchesSource({2,8}));
    assert(signals.PublishConnectionFault({2,8},3,ChatProtocolSignals::Error));
    assert(!signals.MatchesSource({2,8}));
    ChatProtocolSignals::Failure failure;
    assert(signals.ReadFailure(failure) && failure.source.source_id==2 && failure.connect_generation==3);
    ChatProtocolSignals pending;
    assert(pending.PublishConnectionFault({1,7},2,ChatProtocolSignals::Closed));
    assert(!pending.EnableForSource({1,7}));
    assert(pending.EnableForSource({2,8}));
    assert(!pending.PublishConnectionFault({1,7},2,ChatProtocolSignals::Error));
    assert(pending.MatchesSource({2,8}));
}
'''
    generated=tmp_path / "opened.cc";generated.write_text(fixture);binary=tmp_path / "opened"
    subprocess.run(["c++","-std=c++17","-pthread","-I",str(ROOT / "main"),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_actual_lesson_tts_continuations_and_drain_epoch(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    dispatch=method(source,"void Application::DispatchIncomingJson")
    dispatch=dispatch[:dispatch.index('        } else if (strcmp(type->valuestring, "stt") == 0)')]+"        }\n}\n"
    fixture=r'''
#include "chat_inbound_messages.h"
#include "chat_runtime_timing.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
uint64_t esp_timer_get_time(){return 100000;}
constexpr int kDeviceStateSpeaking=1,kDeviceStateListening=2,kDeviceStateIdle=3;
constexpr int kListeningModeRealtime=1,kListeningModeAutoStop=2,kListeningModeManualStop=3;
constexpr int kTtsStopPlaybackDrainTimeoutMs=10000;
struct Board {static Board& GetInstance(){static Board b;return b;}Board* GetDisplay(){return this;}void SetChatMessage(const char*,const char*){}};
struct Application {
 std::atomic<bool> lesson_asset_sync_quiet_{false},lesson_runtime_active_{true},tts_audio_accepting_{false};
 std::atomic<bool> lesson_interactive_listen_pending_{true},lesson_interactive_listening_active_{false},lesson_idle_repaint_suppressed_{false};
 std::atomic<bool> microphone_uplink_authorized_{false},passive_ws_intent_{true},online_intent_{false};
 std::atomic<int64_t> listening_started_ms_{0},last_listening_activity_ms_{0},last_speaking_activity_ms_{0};
 std::atomic<uint32_t> speaking_generation_{0};std::atomic<uint64_t> lesson_terminal_audio_generation_{0};
 int state=kDeviceStateListening,listening_mode_=kListeningModeManualStop;bool current=true,aborted_=true;
 unsigned timers=0;std::vector<std::function<void()>> queued;
 struct Audio {
  unsigned resets=0;std::function<void()> drain;
  void ResetDecoder(){++resets;}void EnableVoiceProcessing(bool){}void SetPlaybackGeneration(uint32_t){}
  bool WaitForPlaybackQueueEmpty(int){if(drain)drain();return true;}void* PopPacketFromSendQueue(){return nullptr;}
 } audio_service_;
 struct Gesture {unsigned begins=0;void BeginResponse(uint32_t){++begins;}void Cancel(){}} speaking_arm_dispatch_;
 struct Protocol {void SendTtsDrainAck(const std::string&){}void SendStartListening(int){}} protocol;Protocol* protocol_=&protocol;
 // This fixture executes only the untagged legacy TTS branch.
 bool HandleLessonPlayoutTts(const cJSON* root,ChatRequestContext) {
  assert(!cJSON_GetObjectItem(root,"playoutId"));return false;
 }
 bool IsChatRequestCurrent(const ChatRequestContext& c)const{return !c || current;}
 bool IsChatLessonRequestCurrent(const ChatRequestContext& c)const{return IsChatRequestCurrent(c);}
 int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}int GetDefaultListeningMode(){return kListeningModeRealtime;}
 void ArmSpeakingTimeout(){++timers;}void Schedule(std::function<void()> f){queued.push_back(std::move(f));}
 void Run(){auto batch=std::move(queued);queued.clear();for(auto& f:batch)f();}
 void DispatchIncomingJson(const cJSON*,uint64_t,bool,ChatRequestContext);
};
'''+dispatch+r'''
int main(){
 Application app;auto context=std::make_shared<ChatInboundMessage>();
 auto* start=cJSON_Parse("{\"type\":\"tts\",\"state\":\"start\"}");
 app.DispatchIncomingJson(start,99,true,context);cJSON_Delete(start);
 assert(app.tts_audio_accepting_ && app.audio_service_.resets==1 && app.speaking_arm_dispatch_.begins==1);
 app.Run();assert(app.state==kDeviceStateSpeaking && !app.aborted_ && app.timers==1);
 auto* stop=cJSON_Parse("{\"type\":\"tts\",\"state\":\"stop\"}");
 app.DispatchIncomingJson(stop,99,true,context);app.audio_service_.drain=[&]{app.current=false;};
 app.Run();assert(app.state==kDeviceStateSpeaking);
 app.current=true;app.audio_service_.drain={};app.DispatchIncomingJson(stop,99,true,context);app.Run();assert(app.state==kDeviceStateListening);
 app.state=kDeviceStateSpeaking;app.DispatchIncomingJson(stop,99,true,context);app.current=false;app.Run();assert(app.state==kDeviceStateSpeaking);
 cJSON_Delete(stop);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o"
    generated=tmp_path / "lesson.cc";generated.write_text(fixture);binary=tmp_path / "lesson"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_unpair_waits_for_delivery_or_original_deadline(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    begin=method(source,"void Application::BeginChatUnpair") if "void Application::BeginChatUnpair" in source else "void Application::BeginChatUnpair(const cJSON*,ChatRequestContext) { EnterRepairPairingMode(); }"
    poll=method(source,"void Application::PollChatUnpair") if "void Application::PollChatUnpair" in source else "void Application::PollChatUnpair(uint64_t) {}"
    fixture=r'''
#include "chat_inbound_messages.h"
#include <cassert>
#include <atomic>
uint64_t now=100;uint64_t esp_timer_get_time(){return now;}
struct Application {
 ChatRequestContext chat_unpair_context_;uint64_t chat_unpair_id_=0,chat_unpair_deadline_us_=0;
 bool chat_unpair_completed_=false,current=true;std::atomic<bool> lesson_runtime_active_{false},lesson_asset_sync_quiet_{false};
 unsigned repairs=0;std::string payload;uint64_t send_received=0;
 bool IsChatRequestCurrent(const ChatRequestContext&)const{return current;}
 uint64_t RequestChatConnectionText(const std::string& text,ChatRequestContext,uint64_t received=0){payload=text;send_received=received;return 7;}
 void EnterRepairPairingMode(ChatRequestContext={}){++repairs;}
 void CancelChatRecovery(){}
 void BeginChatUnpair(const cJSON*,ChatRequestContext);void PollChatUnpair(uint64_t);
};
'''+begin+'\n'+poll+r'''
int main(){
 auto context=std::make_shared<ChatInboundMessage>();context->deadline_us=10000100;
 auto* root=cJSON_Parse("{\"request_id\":\"ack\\\"id\"}");
 Application sent;sent.BeginChatUnpair(root,context);assert(sent.repairs==0);
 auto* ack=cJSON_Parse(sent.payload.c_str());assert(ack);
 assert(std::string(cJSON_GetObjectItem(ack,"request_id")->valuestring)=="ack\"id");cJSON_Delete(ack);
 sent.PollChatUnpair(100);assert(!sent.repairs);
 sent.chat_unpair_completed_=true;sent.PollChatUnpair(101);assert(sent.repairs==1);
 sent.PollChatUnpair(102);assert(sent.repairs==1);
 Application timeout;timeout.BeginChatUnpair(root,context);timeout.PollChatUnpair(10000099);assert(!timeout.repairs);
 timeout.PollChatUnpair(10000100);assert(timeout.repairs==1);
 Application stale;stale.BeginChatUnpair(root,context);stale.current=false;stale.PollChatUnpair(10000100);assert(!stale.repairs && !stale.chat_unpair_context_);
 Application quiet;quiet.lesson_asset_sync_quiet_=true;quiet.BeginChatUnpair(root,context);assert(!quiet.chat_unpair_context_ && !quiet.repairs);
 now=9000000;Application delayed;delayed.BeginChatUnpair(root,context);
 assert(delayed.send_received==9000000 && delayed.chat_unpair_deadline_us_==19000000);
 now=12000000;delayed.BeginChatUnpair(root,context);delayed.PollChatUnpair(now);
 assert(delayed.chat_unpair_deadline_us_==19000000 && !delayed.repairs);
 delayed.PollChatUnpair(18999999);assert(!delayed.repairs);
 delayed.PollChatUnpair(19000000);assert(delayed.repairs==1);
 cJSON_Delete(root);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o"
    generated=tmp_path / "unpair.cc";generated.write_text(fixture);binary=tmp_path / "unpair"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_mcp_send_budget_starts_before_schedule(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    send=method(source,"void Application::SendMcpMessage")
    fixture=r'''
#include "chat_inbound_messages.h"
#include <cassert>
#include <functional>
#include <vector>
uint64_t now=100;uint64_t esp_timer_get_time(){return now;}
extern "C" void cJSON_Delete(cJSON*){}
struct Application {
 std::vector<std::function<void()>> queued;uint64_t received=0;
 bool fail_schedule=false;unsigned faults=0;
 void FailChatRequest(const ChatRequestContext&){++faults;}
 struct Protocol {void SendMcpMessage(const std::string&){}} protocol;Protocol* protocol_=&protocol;
 bool IsChatRequestCurrent(const ChatRequestContext&)const{return true;}
 void Schedule(std::function<void()> f){if(fail_schedule)throw std::bad_alloc();queued.push_back(std::move(f));}
 uint64_t RequestChatConnectionText(const std::string&,ChatRequestContext,uint64_t receipt=0){received=receipt ? receipt : now;return 1;}
 void SendMcpMessage(const std::string&,ChatRequestContext);
};
'''+send+r'''
int main(){
 Application app;auto context=std::make_shared<ChatInboundMessage>();
 app.SendMcpMessage("{}",context);now=9000000;
 for(auto& action:app.queued)action();assert(app.received==100);
 app.fail_schedule=true;app.SendMcpMessage("{}",context);assert(app.faults==1);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    generated=tmp_path / "budget.cc";generated.write_text(fixture);binary=tmp_path / "budget";obj=tmp_path / "cjson.o"
    fixture=fixture.replace('extern "C" void cJSON_Delete(cJSON*){}','')
    generated.write_text(fixture)
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

@pytest.mark.parametrize("failsafe", [False, True])
def test_storage_completion_survives_schedule_allocation_failure(tmp_path, failsafe):
    source=(ROOT / "main/mcp_server.cc").read_text()
    worker=method(source,"void McpServer::LessonAssetSyncTaskBody")
    entry=method(source,"void McpServer::LessonAssetSyncTaskEntry")
    poll=method(source,"void McpServer::PollLessonAssetSyncCompletion") if "void McpServer::PollLessonAssetSyncCompletion" in source else "void McpServer::PollLessonAssetSyncCompletion() {}"
    publish=method(source,"void McpServer::PublishLessonAssetSyncCompletion") if "void McpServer::PublishLessonAssetSyncCompletion" in source else ""
    fixture=r'''
#include "chat_inbound_messages.h"
#include <cassert>
#include <atomic>
#include <functional>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
extern "C" void cJSON_Delete(cJSON*){}
using esp_err_t=int;constexpr int ESP_OK=0;
bool watchdog_failure=false;
int esp_task_wdt_add(void*){if(watchdog_failure)throw 1;return 0;}int esp_task_wdt_delete(void*){return 0;}
std::weak_ptr<const ChatInboundMessage> deleting_permit;std::function<void()> at_delete;
const char* esp_err_to_name(int){return "";}void vTaskDeleteWithCaps(void*){assert(deleting_permit.use_count()==1);at_delete();std::exit(0);}
struct PropertyList {};
uint64_t tool_now=100;
struct McpTool {std::string Call(PropertyList,ChatRequestContext={}){tool_now=20000100;return "full result";}};
struct Application {
 bool current=true;unsigned quiet_ends=0;
 static Application& GetInstance(){static Application app;return app;}
 bool IsChatRequestCurrent(const ChatRequestContext& context){return !context || current;}
 void Schedule(std::function<void()>){throw std::bad_alloc();}
 void EndLessonAssetSyncQuiet(){++quiet_ends;}
};
struct McpServer {
 std::atomic<bool> lesson_asset_sync_in_flight_{true},lesson_asset_sync_completion_ready_{false};
 int lesson_asset_sync_response_id_=0;bool lesson_asset_sync_succeeded_=false;
 bool lesson_asset_sync_send_reply_=true;
 std::string lesson_asset_sync_response_;ChatRequestContext lesson_asset_sync_request_;
 unsigned replies=0;
 void ReplyResult(int,const std::string&,ChatRequestContext c){if(Application::GetInstance().IsChatRequestCurrent(c))++replies;}
 void ReplyError(int,const std::string&,ChatRequestContext c){if(Application::GetInstance().IsChatRequestCurrent(c))++replies;}
 static void LessonAssetSyncTaskBody(void*) noexcept;
 static void LessonAssetSyncTaskEntry(void*) noexcept;
 void PollLessonAssetSyncCompletion();
 void PublishLessonAssetSyncCompletion(int,bool,std::string,ChatRequestContext,bool=true) noexcept;
};
struct LessonAssetSyncTaskContext {McpServer* server;int id;McpTool* tool;PropertyList arguments;ChatRequestContext request_context;};
'''+worker+'\n'+entry+'\n'+publish+'\n'+poll+r'''
int main(){
 McpServer server;McpTool tool;auto request=std::make_shared<ChatInboundMessage>();
 std::weak_ptr<const ChatInboundMessage> permit=request;
 auto* context=new LessonAssetSyncTaskContext{&server,1,&tool,{},request};request.reset();
 server.LessonAssetSyncTaskBody(context);
 assert(server.lesson_asset_sync_in_flight_);assert(!permit.expired());
 Application::GetInstance().current=false;
 server.PollLessonAssetSyncCompletion();
 assert(Application::GetInstance().quiet_ends==1);assert(!server.lesson_asset_sync_in_flight_);
 assert(server.replies==0);assert(permit.expired());
 server.PollLessonAssetSyncCompletion();assert(Application::GetInstance().quiet_ends==1);
 Application::GetInstance().current=true;server.lesson_asset_sync_in_flight_=true;
 auto live=std::make_shared<ChatInboundMessage>();live->deadline_us=10000100;
 std::weak_ptr<const ChatInboundMessage> live_permit=live;
 auto* live_context=new LessonAssetSyncTaskContext{&server,2,&tool,{},live};live.reset();
 server.LessonAssetSyncTaskBody(live_context);assert(tool_now>10000100 && !live_permit.expired());
 server.PollLessonAssetSyncCompletion();assert(server.replies==1 && live_permit.expired());
 server.lesson_asset_sync_in_flight_=true;
 auto final_request=std::make_shared<ChatInboundMessage>();deleting_permit=final_request;
 auto* final_context=new LessonAssetSyncTaskContext{&server,3,&tool,{},final_request};final_request.reset();
 at_delete=[&]{server.PollLessonAssetSyncCompletion();assert(deleting_permit.expired());};
 watchdog_failure=FAILSAFE;
 server.LessonAssetSyncTaskEntry(final_context);assert(false);
}
'''
    fixture=fixture.replace("FAILSAFE", "true" if failsafe else "false")
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    generated=tmp_path / "storage.cc";generated.write_text(fixture);binary=tmp_path / "storage"
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_reboot_final_continuation_checks_source(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    reboot=method(source,"void Application::Reboot(").replace("Reboot()", "Reboot(ChatRequestContext context)")
    fixture=r'''
#include "chat_inbound_messages.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <vector>
#define ESP_LOGI(...) ((void)0)
extern "C" void cJSON_Delete(cJSON*){}
struct ProtocolWorkLifetime {enum class Action{kReboot};void Request(Action){}};
struct Application {
 std::atomic<bool> lesson_runtime_active_{false},reboot_pending_{false};
 ProtocolWorkLifetime protocol_work_lifetime_;bool current=true;unsigned closes=0;
 std::vector<std::function<void()>> queued;
 bool IsChatRequestCurrent(const ChatRequestContext& context)const{return !context || current;}
 void Schedule(std::function<void()> f){queued.push_back(std::move(f));}
 void CloseAudioChannelByIntent(){++closes;}
 void CompletePendingProtocolWork(){}
 void Reboot(ChatRequestContext);
};
'''+reboot+r'''
int main(){
 Application app;auto context=std::make_shared<ChatInboundMessage>();
 app.Reboot(context);app.current=false;
 for(auto& effect:app.queued)effect();assert(app.closes==0);assert(!app.reboot_pending_);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    generated=tmp_path / "reboot.cc";generated.write_text(fixture);binary=tmp_path / "reboot"
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_nested_system_tools_keep_source_context(tmp_path):
    source=(ROOT / "main/mcp_server.cc").read_text()
    start=source.index('    AddUserOnlyTool("self.reboot"')
    end=source.index('    // Display control',start)
    registrations=source[start:end]
    fixture=r'''
#include "chat_inbound_messages.h"
#include <cassert>
#include <functional>
#include <vector>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
unsigned delays=0;void vTaskDelay(int){++delays;}
extern "C" void cJSON_Delete(cJSON*){}
using ReturnValue=bool;
struct SourceMcpCall {};
constexpr int kPropertyTypeString=1;
struct Property {Property(const char*,int,const char*){};};
struct PropertyList {
 PropertyList()=default;PropertyList(std::initializer_list<Property>){}
 PropertyList operator[](const char*)const{return {};}
 template<class T>T value()const{return "firmware";}
};
struct Application {
 bool current=true;unsigned reboot=0,upgrade=0;
 std::vector<std::function<void()>> queued;
 static Application& GetInstance(){static Application app;return app;}
 bool IsChatRequestCurrent(const ChatRequestContext& context)const{return !context || current;}
 void Schedule(std::function<void()> f){queued.push_back(std::move(f));}
 void Reboot(ChatRequestContext context={}){if(IsChatRequestCurrent(context))++reboot;}
 bool UpgradeFirmware(const std::string&){++upgrade;return true;}
};
struct McpServer {
 ChatRequestContext context;
 void AddUserOnlyTool(const char*,const char*,PropertyList,std::function<bool(const PropertyList&)> f){f({});}
 void AddUserOnlyTool(const char*,const char*,PropertyList,SourceMcpCall,std::function<bool(const PropertyList&,ChatRequestContext)> f){f({},context);}
 void Register();
};
void McpServer::Register(){
'''+registrations+r'''
}
int main(){
 auto& app=Application::GetInstance();McpServer server;
 server.context=std::make_shared<ChatInboundMessage>();
 std::weak_ptr<const ChatInboundMessage> permit=server.context;
 server.Register();server.context.reset();
 assert(!permit.expired());
 app.current=false;for(auto& effect:app.queued)effect();app.queued.clear();
 assert(app.reboot==0 && app.upgrade==0);assert(permit.expired());
 app.current=true;server.context=std::make_shared<ChatInboundMessage>();
 server.Register();for(auto& effect:app.queued)effect();
 assert(app.reboot==1 && app.upgrade==1);
 assert(delays==0);
}
'''
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    generated=tmp_path / "nested.cc";generated.write_text(fixture)
    binary=tmp_path / "nested"
    subprocess.run(["c++","-std=c++17","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_owned_json_permit(tmp_path):
    generated=tmp_path / "inbound.cc"
    generated.write_text(r'''
#include "chat_inbound_messages.h"
#include <cassert>
int main(){
    ChatInboundMessages inbound;
    auto* root=cJSON_Parse("{\"type\":\"mcp\",\"payload\":{\"id\":4}}");
    ChatConnectionMessages::Owner owner{{1,7},1,1};
    for(int i=0;i<4;++i)assert(inbound.Admit(root,owner,100,99));
    assert(!inbound.Admit(root,owner,100,99));cJSON_Delete(root);
    auto context=inbound.Take();assert(context && context->lesson_epoch==99);
    assert(cJSON_GetObjectItem(context->root.get(),"payload"));
    assert(inbound.Outstanding()==4);
    auto continuation=context;context.reset();assert(inbound.Outstanding()==4);
    continuation.reset();assert(inbound.Outstanding()==3);
    root=cJSON_Parse("{\"type\":\"tts\",\"state\":\"start\"}");
    auto inline_lesson=inbound.Own(root,owner,100,99,"session");
    assert(inline_lesson && inbound.Outstanding()==4);
    assert(!inbound.Own(root,owner,100,99,"session"));cJSON_Delete(root);
    inline_lesson.reset();assert(inbound.Outstanding()==3);
    ChatInboundMessage reply;reply.session_id="session\"\\";
    const auto encoded=reply.EncodeMcpReply("{\"result\":\"complete\"}");
    auto* envelope=cJSON_Parse(encoded.c_str());assert(envelope);
    assert(std::string(cJSON_GetObjectItem(envelope,"session_id")->valuestring)==reply.session_id);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(envelope,"payload"),"result"));cJSON_Delete(envelope);
}
''')
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    obj=tmp_path / "cjson.o";binary=tmp_path / "inbound"
    subprocess.run(["cc","-fsanitize=address,undefined","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-pthread","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_actual_mcp_reply_owner_and_escaping(tmp_path):
    source=(ROOT / "main/mcp_server.cc").read_text()
    error=method(source,"void McpServer::ReplyError").replace('const std::string& message) {','const std::string& message, ChatRequestContext context) {')
    result=method(source,"void McpServer::ReplyResult").replace('const std::string& result) {','const std::string& result, ChatRequestContext context) {')
    fixture=r'''
#include "chat_inbound_messages.h"
#include <cassert>
struct Application {
 ChatRequestContext seen;std::string payload;
 static Application& GetInstance(){static Application app;return app;}
 bool IsChatRequestCurrent(const ChatRequestContext&){return true;}
 void FailChatRequest(const ChatRequestContext&){++failures;}unsigned failures=0;
 void SendMcpMessage(const std::string& text,ChatRequestContext context={}){payload=text;seen=context;}
};
struct McpServer {void ReplyError(int,const std::string&,ChatRequestContext);void ReplyResult(int,const std::string&,ChatRequestContext);};
'''
    fixture+=error+'\n'+result+r'''
int main(){
 auto context=std::make_shared<ChatInboundMessage>();McpServer server;
 server.ReplyError(3,"quoted\"\\\n",context);
 assert(Application::GetInstance().seen==context);
 auto* json=cJSON_Parse(Application::GetInstance().payload.c_str());assert(json);
 assert(std::string(cJSON_GetObjectItem(cJSON_GetObjectItem(json,"error"),"message")->valuestring)=="quoted\"\\\n");cJSON_Delete(json);
 server.ReplyResult(4,"{\"value\":1}",context);assert(Application::GetInstance().seen==context);
}
'''
    generated=tmp_path / "reply.cc";generated.write_text(fixture)
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o";binary=tmp_path / "reply"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_actual_non_tts_dispatch(tmp_path):
    source=(ROOT / "main/application.cc").read_text()
    dispatch=method(source,"void Application::DispatchIncomingJson")
    begin=dispatch.index('        if (strcmp(type->valuestring, "tts") == 0) {')
    end=dispatch.index('        } else if (strcmp(type->valuestring, "stt") == 0)',begin)
    dispatch=dispatch[:begin]+'        if (strcmp(type->valuestring, "tts") == 0) { assert(false);\n'+dispatch[end:]
    fixture=r'''
#include "chat_inbound_messages.h"
#include "chat_runtime_timing.h"
#include <cassert>
#include <atomic>
#include <functional>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define CONFIG_RECEIVE_CUSTOM_MESSAGE 1
#define CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P 1
uint64_t esp_timer_get_time(){return 100000;}
namespace Lang {namespace Sounds {const char* OGG_VIBRATION="cue";}}
struct Board {
 std::string role,text,emotion;
 static Board& GetInstance(){static Board board;return board;}
 Board* GetDisplay(){return this;}
 void SetChatMessage(const char* who,const char* message){role=who;text=message;}
 void SetEmotion(const char* value){emotion=value;}
 void EnterWifiConfigMode(){}
};using WifiBoard=Board;
struct McpServer {
 ChatRequestContext seen;
 static McpServer& GetInstance(){static McpServer server;return server;}
 void ParseMessage(const cJSON*,ChatRequestContext context={}){seen=context;}
};
struct Application {
 std::atomic<bool> lesson_asset_sync_quiet_{false},lesson_runtime_active_{false};
 struct Protocol {bool SendLessonFrame(const char*){assert(false);return false;}} protocol;
 Protocol* protocol_=&protocol;
 bool current=true;uint64_t lesson_epoch=0;unsigned robot=0,alerts=0;
 unsigned unpair=0;ChatRequestContext unpair_context;
 std::vector<std::function<void()>> scheduled;
 bool IsChatRequestCurrent(const ChatRequestContext& context)const{return !context || current;}
 void Schedule(std::function<void()> action){scheduled.push_back(std::move(action));}
 void HandleEmotionGesture(const char*){}
 void Reboot(ChatRequestContext={}){}
 void EnterRepairPairingMode(){}
 void BeginChatUnpair(const cJSON*,ChatRequestContext context){++unpair;unpair_context=context;}
 void Alert(const char*,const char*,const char*,const char*){++alerts;}
 bool HandleRobotActionMessage(const cJSON*,ChatRequestContext={}){++robot;return true;}
 void EnqueueLessonMessage(const cJSON*,uint64_t epoch,ChatRequestContext){lesson_epoch=epoch;}
 void DispatchIncomingJson(const cJSON*,uint64_t,bool,ChatRequestContext);
};
'''
    fixture+=dispatch+r'''
int main(){
 Application app;auto context=std::make_shared<ChatInboundMessage>();
 auto* root=cJSON_Parse("{\"type\":\"mcp\",\"payload\":{\"id\":1}}");
 app.DispatchIncomingJson(root,99,true,context);cJSON_Delete(root);
 assert(McpServer::GetInstance().seen==context);
 root=cJSON_Parse("{\"type\":\"stt\",\"text\":\"owned\"}");
 app.DispatchIncomingJson(root,99,true,context);cJSON_Delete(root);
 app.current=false;for(auto& effect:app.scheduled)effect();
 assert(Board::GetInstance().text.empty() || Board::GetInstance().text=="owned");
 assert(app.scheduled.empty());
 app.current=true;root=cJSON_Parse("{\"type\":\"lesson_prepare\"}");
 app.DispatchIncomingJson(root,99,true,context);cJSON_Delete(root);assert(app.lesson_epoch==99);
 root=cJSON_Parse("{\"type\":\"system\",\"command\":\"unpair\",\"request_id\":\"ack\"}");
 app.DispatchIncomingJson(root,99,true,context);cJSON_Delete(root);
 assert(app.unpair==1 && app.unpair_context==context);
}
'''
    generated=tmp_path / "dispatch.cc";generated.write_text(fixture)
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON";obj=tmp_path / "cjson.o";binary=tmp_path / "dispatch"
    subprocess.run(["cc","-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT / "main"),"-I",str(cjson),str(generated),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
