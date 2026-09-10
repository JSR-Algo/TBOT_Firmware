from pathlib import Path
import os
import subprocess
import pytest
from test_protocol_work_lifetime import method
from test_chat_protocol_source import source_transport_fixture

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_owned_terminal_intake(tmp_path,sanitize):
    assert (ROOT / "main/chat_playout_intake.h").exists(), "Missing fixed terminal stop handoff"
    header=(ROOT / "main/chat_playout_intake.h").read_text()
    header=header.replace("class ChatPlayoutIntake", "extern std::function<void()> stop_publish_hook;\nclass ChatPlayoutIntake")
    header=header.replace("published_stamp_.store(stamp,std::memory_order_release);",
                          "if (stop_publish_hook) stop_publish_hook();\npublished_stamp_.store(stamp,std::memory_order_release);")
    (tmp_path / "chat_playout_intake.h").write_text(header)
    generated=tmp_path / "intake.cc"
    generated.write_text((ROOT / "tests/native/chat_playout_intake_test.cc").read_text())
    binary=tmp_path / "intake"
    subprocess.run(["c++","-std=c++17","-pthread","-Wall","-Wextra","-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I",str(ROOT / "main"),str(generated),
                    "-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)


def test_terminal_application_methods_exist():
    source=(ROOT / "main/application.cc").read_text()
    for name in ["EstablishChatPlayoutResponse", "HandleChatTerminalStop", "PollChatPlayout", "RecoverChatPlayout"]:
        assert "Application::"+name in source, f"Missing bounded terminal application method: {name}"


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
@pytest.mark.parametrize("send_wake",[0,1])
def test_actual_terminal_application(tmp_path,sanitize,send_wake):
    run_terminal_application(tmp_path,sanitize,send_wake)


def run_terminal_application(tmp_path,sanitize,send_wake,extra_tests="",extra_definitions="",fixture_transform=None):
    app=(ROOT / "main/application.cc").read_text()
    header=(ROOT / "main/application.h").read_text()
    fixture=(ROOT / "tests/native/chat_outbound_worker_test.cc").read_text().split("int main() {")[0]
    prefix='#include "chat_runtime_timing.h"\n#include <optional>\n#include "audio/conversation_playout_controller.h"\n#include <atomic>\n#include <mutex>\n#define private public\n#include "chat_protocol_signals.h"\n#undef private\n'
    fixture=prefix+fixture
    fixture=fixture.replace("    void PollChatRecovery(uint64_t) { assert(false); }", "")
    fixture=fixture.replace("    void CancelChatRecovery() {}", "")
    fixture=fixture.replace("    struct RecoveryAdapter { bool opening=false,adopted=false;uint32_t connect_generation=0;uint64_t protocol_generation=0,lesson_generation=0; } chat_recovery_;", "")
    fixture=fixture.replace("struct TestProtocol : Protocol {", "struct TestProtocol : Protocol {\n    std::function<Result(const Job&, const std::function<bool()>&)> full_text_send;")
    fixture=fixture.replace("        return SendChatControlIfCurrent(job, current);", "        if(full_text_send)return full_text_send(job,current);\n        return SendChatControlIfCurrent(job, current);")
    fixture=fixture.replace("#define ESP_LOGW(...) ((void)0)", r'''
#include <cstdarg>
#include <cstdio>
std::mutex runtime_log_mutex;
std::vector<std::string> runtime_logs;
std::vector<std::string> informational_logs;
void TestLog(const char*, const char* format, ...) {
    char line[256];
    va_list arguments; va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
    std::lock_guard<std::mutex> lock(runtime_log_mutex);
    runtime_logs.emplace_back(line);
}
void TestInfoLog(const char*, const char* format, ...) {
    char line[256];
    va_list arguments; va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
    std::lock_guard<std::mutex> lock(runtime_log_mutex);
    informational_logs.emplace_back(line);
}
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
constexpr const char* TAG="test";
''')
    fixture=fixture.replace("#define ESP_LOGI(...) ((void)0)", "#define ESP_LOGI(...) TestInfoLog(__VA_ARGS__)")
    fixture=fixture.replace("    void PollChatSourceOpen(uint64_t) {}", "    uint32_t chat_source_open_handled_=0;void PollChatSourceOpen(uint64_t);\n    bool IsConnectSuccessPublicationSuppressed(){return false;}\n    void StartHeartbeat(){}void StopHeartbeat(){}void DispatchDeviceHeartbeat(){}void StopClaimPoll(){}void DismissAlert(){}")
    fixture=fixture.replace("struct Board {", "enum class PowerSaveLevel {PERFORMANCE,LOW_POWER};\nstruct Board {\n    void SetPowerSaveLevel(PowerSaveLevel){}Board* GetAudioCodec(){return this;}int output_sample_rate(){return 24000;}void SetChatMessage(const char*,const char*){}")
    fixture=fixture.replace('using ChatRequestContext = std::shared_ptr<void>;', '')
    fixture=fixture.replace('    bool IsChatRequestCurrent(const ChatRequestContext&) const { return true; }', '')
    fixture=fixture.replace('#include "chat_protocol_signals.h"', '#include "chat_protocol_signals.h"\n#include "chat_inbound_messages.h"')
    fixture=fixture.replace('#include "chat_protocol_signals.h"', '#include "chat_protocol_signals.h"\n#include "chat_outbound_worker.h"\n#include "chat_control_intents.h"')
    fixture=fixture.replace("    void PollChatProtocolSignals() {}", "    void PollChatProtocolSignals();")
    fixture=fixture.replace("    void PollChatStart(uint64_t) {}", "")
    fixture=fixture.replace("    void PollChatLessonCapture(uint64_t) {}", "")
    fixture=fixture.replace("    void PollChatUnpair(uint64_t) {}", "")
    begin=fixture.index("    struct ConnectionMessages {")
    end=fixture.index("    bool IsChatConnectionCurrent",begin)
    end=fixture.index("\n",end)
    fixture=fixture[:begin]+fixture[end:]
    fixture=fixture.replace("    struct Controls { size_t Size() const { return 0; } } chat_control_intents_;\n    bool DeliverChatControl(const Completion&) { return false; }\n    void PollChatControls(uint64_t) {}", "")
    fixture=fixture.replace("constexpr DeviceState kDeviceStateConnecting = 1, kDeviceStateIdle = 2;",
        "constexpr DeviceState kDeviceStateConnecting = 1, kDeviceStateIdle = 2, kDeviceStateSpeaking = 3, kDeviceStateListening = 4, kDeviceStateAudioTesting=5,kDeviceStateWifiConfiguring=6;")
    fixture=fixture.replace('const char* PLEASE_WAIT = "wait";', 'const char* PLEASE_WAIT = "wait"; const char* LISTENING="listening";const char* SERVER_UNAVAILABLE_RETRYING="retry";')
    fixture=fixture.replace('namespace Lang { namespace Strings', 'namespace Lang { namespace Sounds { const char* OGG_POPUP="popup"; const char* OGG_EXCLAMATION="exclamation"; } }\nnamespace Lang { namespace Strings')
    fixture=fixture.replace('    void SetStatus(const char*) {}', '''
    std::string status;
    void SetStatus(const char* value) { status=value; }
    Board* GetLed() { return this; }
    void OnStateChanged() {}
    void ClearChatMessages() {}
    void SetEmotion(const char*) {}
''')
    fixture=fixture.replace('class Application;','''
struct TbotConnectStateSpec {};
struct TbotConnectMapper { static const TbotConnectStateSpec* Resolve(int,int,int,bool) { return nullptr; } };
const char* ConnectStateScreenCopy(const TbotConnectStateSpec*) { return "idle"; }
#define MAIN_EVENT_STATE_CHANGED 4096
class Application;
''')
    fixture=fixture.replace("void vTaskDelay(int) {}", "std::function<void()> receiver_wait;\nvoid vTaskDelay(int) { if(receiver_wait)receiver_wait(); }")
    fixture=fixture.replace("    uint32_t chat_playout_stamp_=0;\n    bool chat_playout_recovery_=false;\n    void PollChatPlayout(uint64_t) { assert(false); }\n", "")
    start=header.index("    bool EstablishChatPlayoutResponse(")
    end=header.index("    bool PollChatProtocolCleanup();",start)
    fields=header[start:end]
    fields='\n'.join(line for line in fields.split('\n') if 'void DispatchIncomingJson(' not in line)
    fields=fields.replace('    bool IsLessonVoiceRoute() const;', '    bool IsLessonVoiceRoute() const { return false; }')
    fields=fields.replace('    bool IsChatLessonRequestCurrent(const ChatRequestContext& context) const;', '    bool IsChatLessonRequestCurrent(const ChatRequestContext&) const { return true; }')
    fields=fields.replace('    void HandleChatLessonAudio(const std::shared_ptr<ChatProtocolSignals>& signals,\n        uint64_t protocol_generation, ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet);', '    void HandleChatLessonAudio(const std::shared_ptr<ChatProtocolSignals>&, uint64_t, ConnectionSource, std::unique_ptr<AudioStreamPacket>) { assert(false); }')
    fixture=fixture.replace("    bool chat_cleanup_enabled_ = false;",fields+'''
    std::atomic<uint32_t> chat_protocol_state_{0};
    void ArmConnectWatchdog() {}
    std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
    std::atomic<bool> chat_protocol_owned_{false},tts_audio_accepting_{true};
    bool chat_protocol_fault_=false;
    uint32_t chat_source_failure_handled_=0;
    void HandleChatSourceFailure() {}
    std::atomic<uint32_t> chat_source_connect_generation_{1};
    std::atomic<uint32_t> protocol_callback_connect_generation_{1};
    uint64_t chat_protocol_fault_generation_=0;
    uint32_t chat_protocol_fault_era_=0;
    Protocol::SourceCallbacks MakeChatSourceCallbacks(uint64_t,std::shared_ptr<ChatProtocolSignals>);
    bool SelectChatProtocolSource(ConnectionSource,uint64_t,uint32_t);
    std::atomic<uint32_t> speaking_generation_{1};
    unsigned cleanups=0;
    bool chat_reboot_audio_requested_=false;
    bool chat_playback_fault_=false;
    ListeningMode listening_mode_=kListeningModeRealtime;
    enum class ChatWakePolicy { Explicit, Listening };
    uint32_t chat_audio_prepared_=0, chat_audio_reset_completed_=1,chat_audio_reset_serial_=1;
    bool chat_audio_fault_=false;
    bool chat_wake_read_pending_=false;
    uint32_t chat_wake_read_serial_=0;
    std::optional<std::string> chat_wake_read_result_;
    struct Gesture { unsigned begins=0;void Cancel() {} void BeginResponse(uint32_t) { ++begins; } } speaking_arm_dispatch_;
    std::atomic<uint32_t> interrupt_count_{0};
    std::atomic<int64_t> last_speaking_activity_ms_{0};
    bool aborted_=false;
    unsigned repairs=0;
    void EnterRepairPairingMode(ChatRequestContext={}) { ++repairs; }
    unsigned cues=0,timeout_rearms=0;
    bool RequestChatCue(std::string_view) { ++cues;return true; }
    bool voice_detected=false;
    bool IsVoiceDetected() { return voice_detected; }
    static constexpr uint32_t kListeningNoSpeechTimeoutMs=1000,kListeningRealtimeNoSpeechTimeoutMs=1000,kListeningMaxTurnMs=2000,kListeningAutoStopMaxTurnMs=2000,kSpeakingTimeoutMs=1000;
    void ArmSpeakingTimeout() { ++timeout_rearms; }
    void HandleListeningWatchdogTick();
    void HandleSpeakingTimeout(uint32_t);
    void SelectedSendAudioEvent();
    std::atomic<bool> lesson_asset_sync_quiet_{false};
    std::atomic<int64_t> listening_started_ms_{0},last_listening_activity_ms_{0};
    int claim_substate_=0;
    int GetBleSubstate() { return 0; }
    bool IsDeviceClaimed() { return true; }
    ListeningMode GetDefaultListeningMode() { return kListeningModeAutoStop; }
    void HandleStateChangedEvent();
    void HandleStopListeningEvent();
    void HandleStartListeningEvent();
    void SetListeningMode(ListeningMode);
    void HandleWakeWordDetectedEvent();
    void ContinueWakeWordInvoke(const std::string&);
    void FinishWakeWordInvoke(const std::string&);
    void AbortSpeaking(AbortReason);
    bool IsChatRequestCurrent(const ChatRequestContext&) const;
    void FailChatRequest(const ChatRequestContext&) { assert(false); }
    std::string dispatched_text;uint64_t dispatched_epoch=0;
    std::function<void(ChatRequestContext)> json_dispatch;
    void DispatchIncomingJson(const cJSON* root,uint64_t epoch,bool,ChatRequestContext context) {
        dispatched_text=cJSON_GetObjectItem(root,"text")->valuestring;dispatched_epoch=epoch;
        if(json_dispatch)json_dispatch(std::move(context));
    }
    uint32_t RequestChatPlaybackCleanup(uint32_t) { return ++audio_service_.reset_token; }
    uint32_t RequestChatAudioCleanup(uint32_t,bool,bool,bool,bool=true,bool=false,ChatWakePolicy=ChatWakePolicy::Explicit) { ++cleanups;audio_service_.current=false;return 1; }
    bool chat_cleanup_enabled_ = false;
''')
    fixture=fixture.replace("        void Stop() {}",'''
        bool reset_busy=false,snapshot_busy=false;
        unsigned snapshot_calls=0;
        uint32_t reset_token=1;
        std::vector<uint32_t> received_generations,received_tokens;
        bool PushChatPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet,uint32_t token) {
            received_generations.push_back(packet->generation);received_tokens.push_back(token);return true;
        }
        bool IsCurrentChatPlaybackReset(uint32_t token) const { return token && token!=UINT32_MAX && token==reset_token; }
        bool IsChatPlaybackResetPending() const { return reset_busy; }
        bool ArmChatUplink(uint32_t) { current=true;return true; }
        void EnableAudioTesting(bool) { assert(false); }
        bool TryGetPlaybackResetEpoch(uint64_t& epoch) const { if(reset_busy)return false;epoch=1;return true; }
        bool TryGetPlaybackDrainSnapshot(PlaybackDrainSnapshot& out) {
            ++snapshot_calls;
            if(snapshot_busy)return false;
            out={1,reset_token,false,0,0,false,false,{1,AudioOutputDrainState::Drained}};return true;
        }
        void Stop() {}
''')
    fixture += "\nvoid Application::PollChatLessonCapture(uint64_t) {}\n"
    methods=["bool Application::EstablishChatPlayoutResponse","void Application::HandleChatTerminalStop",
             "void Application::CancelChatRecovery", "bool Application::RetainChatRecovery",
             "void Application::PollChatRecovery", "bool Application::CompleteChatRecoveryOpen",
             "void Application::HandleChatStart", "void Application::HandleChatAudio",
             "void Application::PollChatStart", "void Application::RecoverChatStart",
             "bool Application::SelectChatProtocolSource",
             "void Application::PollChatSourceOpen",
             "bool Application::AdvanceChatRearm", "void Application::RenderChatRearm",
             "bool Application::IsChatRearmRecoveryCurrent",
             "void Application::HandleStopListeningEvent",
             "bool Application::IsSelectedNormalChatRoute", "bool Application::HandleChatStopListening", "bool Application::RetainChatActiveListen",
             "bool Application::RequestChatControl", "void Application::PollChatControls", "bool Application::DeliverChatControl",
             "bool Application::IsChatConnectionCurrent", "bool Application::IsChatRequestCurrent", "uint64_t Application::RequestChatConnectionText", "void Application::PollChatConnectionMessages", "void Application::BeginChatUnpair", "void Application::PollChatUnpair",
             "void Application::PollChatInboundMessages",
             "bool Application::BeginChatListen", "bool Application::HandleChatAbort",
             "bool Application::HandleChatWake",
             "void Application::PollChatPlayout","void Application::RecoverChatPlayout",
             "bool Application::InitializeChatOutboundWorker","ChatOutboundMailbox::Result Application::ActivateChatOutbound",
             "ChatOutboundMailbox::Result Application::SubmitChatOutbound","void Application::RetireChatOutbound",
             "bool Application::PollChatOutbound","bool Application::IsChatOutboundCompletionCurrent","void Application::NotifyChatOutbound",
             "void Application::ChatOutboundTask", "Protocol::SourceCallbacks Application::MakeChatSourceCallbacks",
             "void Application::PollChatProtocolSignals", "void Application::PollChatOutboundEvents"]
    state_handler=method(app,"void Application::HandleStateChangedEvent")
    state_handler=state_handler[:state_handler.index("    DeviceState new_state")] + '    assert(false && "legacy state effects reached");\n}'
    entries=[]
    for signature, marker in [
        ("void Application::HandleStartListeningEvent", "    auto state ="),
        ("void Application::SetListeningMode", "    passive_ws_intent_.store(false);"),
        ("void Application::HandleWakeWordDetectedEvent", "    if (lesson_asset_sync_quiet_"),
        ("void Application::ContinueWakeWordInvoke", "    // Check state again"),
        ("void Application::FinishWakeWordInvoke", "    auto state ="),
        ("void Application::AbortSpeaking", "    speaking_arm_dispatch_.Cancel();"),
        ("void Application::HandleListeningWatchdogTick", "    uint32_t decode_q ="),
        ("void Application::HandleSpeakingTimeout", "    ESP_LOGW(TAG, \"speaking_timeout generation="),
    ]:
        entry=method(app,signature)
        entries.append(entry[:entry.index(marker)]+'    assert(false && "legacy blocking entry reached");\n}')
    send=app[app.index('        if (bits & MAIN_EVENT_SEND_AUDIO) {'):]
    send=send[:send.index('            static uint32_t send_event_count')]
    entries.append('void Application::SelectedSendAudioEvent() { int bits=1;\n#define MAIN_EVENT_SEND_AUDIO 1\n'+send+'assert(false && "legacy audio work reached");\n}}}\n#undef MAIN_EVENT_SEND_AUDIO')
    fixture=fixture.replace("    void HandleChatSourceFailure() {}", "    bool exercise_source_failure=false;void ActiveOpenSourceFailure();void HandleChatSourceFailure() { if(exercise_source_failure)ActiveOpenSourceFailure(); }\n    bool ShouldKeepManagementHeartbeat(){return false;}")
    fixture=fixture.replace("    bool PollChatProtocolCleanup() { assert(false); return false; }", "    bool PollChatProtocolCleanup() { assert(exercise_source_failure);return false; }")
    recovery=method(app,"void Application::HandleChatSourceFailure").replace("Application::HandleChatSourceFailure", "Application::ActiveOpenSourceFailure")
    fixture=fixture.replace("// PRODUCTION_METHODS","\n".join(method(app,s) for s in methods)+"\n"+state_handler+"\n"+"\n".join(entries)+"\n"+recovery)
    fixture += "\n" + method(app,"bool Application::StartOpenChannelWorker")
    transport=source_transport_fixture().split("int main() {")[0]
    includes="\n".join(line for line in transport.splitlines() if line.startswith("#include"))
    transport="\n".join(line for line in transport.splitlines() if not line.startswith(("#include", "#define ESP_LOG")))
    fixture=includes+"\n"+fixture+"\nnamespace transport_fixture {\n"+transport+"\n}\n"
    open_completion=app[app.index("    self->Schedule([self, ok, mode, gen, wake_word, wake_word_invoke, passive_preconnect,"):]
    open_completion=method(open_completion,"self->Schedule")+");"
    fixture=fixture.replace("    void SchedulePassiveLessonReconnect() {}", "    void SchedulePassiveLessonReconnect() {}\n    void ScheduleLessonAssetSyncWakeRearm(uint64_t){}")
    fixture=fixture.replace("        void EnableAudioTesting(bool)", "        void EnableWakeWordDetection(bool){}\n        void EnableAudioTesting(bool)")
    fixture+="\nconstexpr int MAIN_EVENT_ERROR=2;\nvoid CompleteActiveOpen(Application& app,uint64_t reservation){auto* self=&app;bool ok=true,wake_word_invoke=false,passive_preconnect=false;auto mode=kListeningModeRealtime;uint32_t gen=1;uint64_t worker_protocol_generation=1;std::string wake_word;\n"+open_completion+"\napp.RunTasks();}\n"
    fixture+=(ROOT / "tests/native/chat_active_open_test.cc").read_text()
    fixture+=extra_definitions
    fixture+=(ROOT / "tests/native/chat_terminal_application_test.cc").read_text().replace("int main() {", "int main() {\n    TestActiveOpenGreeting();\n"+extra_tests)
    if fixture_transform:
        fixture=fixture_transform(fixture,app,header)
    generated=tmp_path / "application.cc";generated.write_text(fixture)
    flags=[f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}", f"-DCONFIG_SEND_WAKE_WORD_DATA={send_wake}", "-DESP_LOGD(...)=((void)0)"]
    cjson=Path.home() / "esp/esp-idf/components/json/cJSON"
    obj=tmp_path / "cjson.o"
    subprocess.run(["cc",*flags,"-Wno-deprecated-declarations","-I",str(cjson),"-c",str(cjson / "cJSON.c"),"-o",str(obj)],check=True)
    binary=tmp_path / "app"
    subprocess.run(["c++","-std=c++17","-pthread","-Wall","-Wextra","-Werror","-Wno-unused-variable",*flags,
                    "-I",str(ROOT / "main"),"-I",str(ROOT / "tests/native_stubs"),"-I",str(cjson),
                    str(generated),str(ROOT / "main/chat_outbound_worker.cc"),str(obj),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
