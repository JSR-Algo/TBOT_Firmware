"""Recovery runs the existing production-method adapter, including wire gates."""
import os
import pytest
from test_chat_playout_intake import run_terminal_application
from test_protocol_work_lifetime import method


def recovery_fixture(fixture, source, header):
    fixture=fixture.replace(method(source,"bool Application::StartOpenChannelWorker"), "")
    fixture=fixture.replace("static bool forbid_allocation = false;", r'''
static bool fail_next_context=false;
void* operator new(size_t size,const std::nothrow_t&) noexcept {
    if(fail_next_context){fail_next_context=false;return nullptr;}
    return std::malloc(size ? size : 1);
}
static bool forbid_allocation = false;
''')
    fixture=fixture.replace("    void ScheduleReconnect(ListeningMode, bool) {}", "    void ScheduleReconnect(ListeningMode,bool);std::atomic<unsigned> reconnect_count_{0};")
    fixture=fixture.replace("    void RearmClaimedIdleWakeWord() {}", "    void RearmClaimedIdleWakeWord();")
    fixture=fixture.replace("        void EnableWakeWordDetection(bool){}", "        void EnableWakeWordDetection(bool){}bool IsWakeWordRunning(){return true;}")
    fixture=fixture.replace("void esp_timer_stop(void*) {}", r'''
struct esp_timer_create_args_t { void(*callback)(void*)=nullptr;void* arg=nullptr;const char* name=nullptr; };
static esp_timer_create_args_t timer_callback;
constexpr int ESP_OK=0;
int esp_timer_create(esp_timer_create_args_t* args,void** timer) {*timer=reinterpret_cast<void*>(1);timer_callback=*args;return ESP_OK;}
void esp_timer_stop(void*) {}
void esp_timer_start_once(void*,uint64_t) {}
uint32_t esp_random() { return 0; }
''')
    fixture=fixture.replace("    void EnterRepairPairingMode(ChatRequestContext={}) { ++repairs; }", "    void EnterRepairPairingMode(ChatRequestContext={});")
    fixture=fixture.replace('const char* PLEASE_WAIT = "wait";', 'const char* INITIALIZING="init";const char* PLEASE_WAIT = "wait";')
    fixture=fixture.replace("class Application;", r'''
struct Settings { Settings(const char*,bool){}std::string GetString(const char*){return {};}void SetString(const char*,const char*){}void SetInt(const char*,int){}void EraseKey(const char*){} };
namespace SystemReset {bool ReleaseCloudOwnership(){assert(false);return false;}}
struct TbotClaimSubstate {static constexpr int AvailableStandby=1;};
void SecureClearString(std::string& value){value.clear();}
enum class SsidMutationResult { kApplied };
struct SsidManager {static SsidManager& GetInstance(){static SsidManager manager;return manager;}SsidMutationResult ForceClearAndCancelTransaction(){return SsidMutationResult::kApplied;}};
class Application;
''')
    fixture=fixture.replace("    bool chat_cleanup_enabled_ = false;", r'''
    struct WifiConfigEntryPreparation {
        DeviceState original_state=0;ListeningMode resume_mode=kListeningModeAutoStop;
        bool resume_realtime=false,resume_listening=true,valid=false;
    };
    bool PrepareWifiConfigEntry(WifiConfigEntryPreparation&);
    bool PublishWifiConfigEntry(const WifiConfigEntryPreparation&);
    bool RollbackWifiConfigEntry(const WifiConfigEntryPreparation&) { assert(false);return false; }
    void SetLessonRuntimeActive(bool);
    struct PendingTbotClaim {} pending_tbot_claim_;
    std::string pending_tbot_claim_api_url_,pending_tbot_claim_token_;
    bool claim_confirmation_ambiguous_=false;
    void RenderClaimSubstate(int){}
    std::atomic<uint64_t> lesson_terminal_audio_generation_{0};
    std::atomic<int> deferred_heartbeat_auth_failure_status_{0};
    bool chat_cleanup_enabled_ = false;
''')
    fixture=fixture.replace("void SetDeviceState(int next) { state = next; }", "bool SetDeviceState(int next) { state = next;return true; }")
    fixture=fixture.replace("        void EnableAudioTesting(bool)", "        void EnableVoiceProcessing(bool){}void ResetDecoder(){}\n        void EnableAudioTesting(bool)")
    from test_chat_playout_intake import ROOT
    policy=(ROOT / "main/wifi_config_entry_policy.h").read_text()
    fixture=fixture.replace("class Application;", "constexpr int kDeviceStateStarting=7,kDeviceStateActivating=8;\n"+policy[policy.index("class WifiConfigEntryPolicy"):]+"\nclass Application;")
    epoch=(ROOT / "main/lesson_embodied_action.cc").read_text()
    fixture=fixture.replace("class Application;", method(epoch,"std::uint64_t NextLessonRuntimeGeneration")+"\nclass Application;")
    fixture = fixture.replace("bool Application::CompletePendingProtocolWork() { return false; }", "")
    fixture = fixture.replace("    bool PollChatProtocolCleanup() { assert(exercise_source_failure);return false; }", "    bool PollChatProtocolCleanup(); void RunChatProtocolCleanup();")
    fixture = fixture.replace("enum class NetworkWorkKind { kOpenChannel, kHeartbeat };", "enum class NetworkWorkKind { kOpenChannel, kHeartbeat, kProtocolCleanup };")
    start = fixture.index("int xQueueSend(void*, const NetworkWorkItem* work, int)")
    fixture = fixture[:start] + fixture[start:].replace(method(fixture[start:], "int xQueueSend"), r'''
static bool queued_ready=false;
int xQueueSend(void*, const NetworkWorkItem* work, int) {
    if (!queue_accept) return 0;
    assert(!queued_ready); queued_work=*work;queued_ready=true;return pdTRUE;
}
int xQueueReceive(void*, NetworkWorkItem* work, int) {
    if (!queued_ready) throw WorkerStopped{};
    *work=queued_work;queued_ready=false;return pdTRUE;
}
''', 1)
    fixture = fixture.replace("    bool OpenAudioChannel() override { return true; }", "    unsigned opens=0;bool opened=true,open_success=true;std::function<void()> on_open,on_attempt;\n    bool OpenAudioChannel() override { ++opens;if(on_attempt)on_attempt();opened=open_success;if(opened && on_open)on_open();return opened; }")
    fixture=fixture.replace("std::vector<uint64_t> deadlines;", "std::vector<uint64_t> deadlines;std::vector<uint32_t> epochs;")
    fixture=fixture.replace("deadlines.push_back(job.deadline_us);", "deadlines.push_back(job.deadline_us);epochs.push_back(job.connection_epoch);")
    fixture = fixture.replace("    bool IsAudioChannelOpened() const override { return true; }", "    bool IsAudioChannelOpened() const override { return opened; }")
    fixture = fixture.replace("{ ++closes; }", "{ ++closes;opened=false; }")
    fixture = fixture.replace("    bool chat_cleanup_enabled_ = false;", r'''
    struct ChatProtocolCleanup {
        std::unique_ptr<TestProtocol> protocol;
        ProtocolWorkLifetime::Action action=ProtocolWorkLifetime::Action::kNone;
        uint32_t epoch=0;bool intentional=false,success=false,destructive_prepared=false;
    } chat_protocol_work_;
    std::atomic<uint32_t> lesson_protocol_readers_{0};
    void BeginChatRebootAudioCleanup() { chat_reboot_audio_requested_=true; }
    void HandleReconnectTick();void ContinueOpenAudioChannel(ListeningMode);
    void StartPassiveLessonWebsocket();
    static void OpenChannelTask(void*);
    struct Epoch { uint64_t PublishedEpoch(){return 1;} } lesson_transport_epoch_gate_;
    bool chat_cleanup_enabled_ = false;
''')
    fixture = fixture.replace("void Protocol::SetError", "constexpr int kWakeWordAudioChannelOpenMaxAttempts=3,kWakeWordAudioChannelRetryDelayMs=700;\nvoid Protocol::SetError")
    # Replace only infrastructure methods; all cleanup/open/adoption methods are production.
    signatures = ["bool Application::PollChatProtocolCleanup", "void Application::RunChatProtocolCleanup",
                  "bool Application::CompletePendingProtocolWork", "bool Application::StartOpenChannelWorker",
                  "void Application::ContinueOpenAudioChannel", "void Application::StartPassiveLessonWebsocket",
                  "void Application::OpenChannelTask", "void Application::HandleReconnectTick"]
    signatures += ["void Application::ScheduleReconnect", "void Application::RearmClaimedIdleWakeWord", "void Application::CloseAudioChannelByIntent",
                   "void Application::ResetProtocol", "void Application::RequestInitializeProtocol", "void Application::Reboot",
                   "void Application::HandleConnectWatchdog", "bool Application::PrepareWifiConfigEntry",
                   "bool Application::PublishWifiConfigEntry", "void Application::SetLessonRuntimeActive"]
    signatures += ["void Application::EnterRepairPairingMode"]
    fixture += "\n" + "\n".join(method(source, s) for s in signatures)
    fixture += r'''
void Protocol::SetIncomingJsonTransportEpoch(uint64_t) {}
void Application::HeartbeatTask(void*) { assert(false); }
void Application::CompleteReboot() { assert(false); }
void Application::DoResetProtocol() { assert(false); }
'''
    fixture = fixture.replace("    TestActiveOpenGreeting();", "")
    main_start = fixture.index("int main() {")
    old_main = method(fixture[main_start:], "int main()")
    fixture = fixture.replace(old_main, r'''
void RunNetwork(Application& app) {
    try { Application::OpenChannelTask(&app); } catch(const WorkerStopped&) {}
    app.RunTasks();app.PollChatProtocolCleanup();
}
int main() {
    now_us=100;
    Application app;Setup(app);app.chat_cleanup_enabled_=true;app.exercise_source_failure=true;
    app.state=kDeviceStateIdle;app.online_intent_=true;
    auto* protocol=app.protocol_.get();
    if (RECOVERY_CASE==32) {
        protocol->open_success=false;
        protocol->on_attempt=[&] {
            app.SetLessonRuntimeActive(true);app.SetLessonRuntimeActive(false);
            // No Application recovery poll runs before the failed completion.
        };
    }
    protocol->on_open=[&] {
        protocol->healthy_epoch=8;
        if (RECOVERY_CASE==26 && protocol->opens==1) {
            const auto expired_generation=app.connect_generation_.load();
            now_us=10000100;app.PollChatRecovery(now_us);
            assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::Background);
            assert(app.state==kDeviceStateIdle && app.connect_in_flight_);
            now_us=35000100;app.HandleConnectWatchdog(expired_generation);
            assert(!app.connect_in_flight_ && app.protocol_work_lifetime_.Busy());
            assert(app.chat_recovery_.retry_at_us>now_us && "expired recovery must schedule after watchdog in Idle");
            protocol->opened=false;return;
        }
        if (RECOVERY_CASE==28 && protocol->opens==1) {
            const auto old_epoch=app.lesson_runtime_generation_.load();
            app.SetLessonRuntimeActive(true);app.SetLessonRuntimeActive(false);
            assert(app.lesson_runtime_generation_==old_epoch+2);
            app.PollChatRecovery(now_us);
            assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None);
            protocol->opened=false;return;
        }
        if (RECOVERY_CASE==24) app.HandleChatWake({},true);
        assert(app.chat_protocol_signals_->PublishOpened({2,8},app.connect_generation_,24000,now_us+1000000));
    };
    if (RECOVERY_CASE==19) app.HandleChatWake({},true);
    if (RECOVERY_CASE==31) {
        assert(app.RetainChatRecovery(Application::ChatRecoveryIntent::Kind::Wake,kListeningModeAutoStop));
        auto context=std::make_shared<ChatInboundMessage>();context->owner={{1,7},1,1};
        context->received_us=100;context->deadline_us=10000100;
        auto* request=cJSON_Parse("{\"request_id\":\"accepted-unpair\"}");
        app.BeginChatUnpair(request,context);cJSON_Delete(request);
        assert(app.chat_unpair_context_ && app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None);
    }
    app.HandleChatSourceFailure();
    if (RECOVERY_WAKE && RECOVERY_CASE!=19 && RECOVERY_CASE!=20 && RECOVERY_CASE!=24 && RECOVERY_CASE!=31) {
        if (RECOVERY_CASE==21) now_us=UINT64_MAX-1;
        if (RECOVERY_CASE==23) app.HandleStartListeningEvent();else app.HandleChatWake({},true);
        if (RECOVERY_CASE!=21) {
            assert(app.chat_recovery_.kind==(RECOVERY_CASE==23 ? Application::ChatRecoveryIntent::Kind::Listen : Application::ChatRecoveryIntent::Kind::Wake));
            assert(app.chat_recovery_.deadline_us==10000100);
        }
    }
    if (RECOVERY_CASE==1) app.HandleStopListeningEvent();
    if (RECOVERY_CASE==2) app.HandleChatAbort(kAbortReasonNone,false);
    if (RECOVERY_CASE==3) {
        now_us=200;
        app.HandleChatWake({},true);
        assert(app.chat_recovery_.deadline_us==10000100);
        now_us=10000100;
    }
    if (RECOVERY_CASE==4) now_us=99;
    if (RECOVERY_CASE==5) app.lesson_runtime_active_=true;
    if (RECOVERY_CASE==6) app.state=kDeviceStateWifiConfiguring;
    if (RECOVERY_CASE==7) ++app.protocol_generation_;
    if (RECOVERY_CASE==8) ++app.connect_generation_;
    if (RECOVERY_CASE==9) app.CloseAudioChannelByIntent();
    if (RECOVERY_CASE==10) {app.ResetProtocol();app.RunTasks();}
    if (RECOVERY_CASE==11) app.RequestInitializeProtocol();
    if (RECOVERY_CASE==12) {app.Reboot();app.RunTasks();}
    if (RECOVERY_CASE==13) {
        for(int i=0;i<4;++i){ChatControlIntents::Intent old;old.source={1,7};old.protocol_generation=1;old.connect_generation=1;old.job.kind=Kind::Wake;assert(app.chat_control_intents_.Push(old));}
    }
    if (RECOVERY_CASE==14) {
        for(int i=0;i<100;++i)app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==0 && app.protocol_work_lifetime_.Busy());
    }
    if (RECOVERY_CASE==22) {app.connect_generation_=UINT32_MAX;app.chat_recovery_.connect_generation=UINT32_MAX;}
    if (RECOVERY_CASE==27) {
        Application::WifiConfigEntryPreparation preparation;
        assert(app.PrepareWifiConfigEntry(preparation) && preparation.valid);
        assert(app.PublishWifiConfigEntry(preparation));
    }
    if (RECOVERY_CASE==30) {
        // A queued remote request from the failed source is stale and rejected.
        auto context=std::make_shared<ChatInboundMessage>();context->owner={{1,7},1,1};
        app.chat_protocol_signals_->Disable();
        auto* request=cJSON_Parse("{\"request_id\":\"stale-unpair\"}");
        app.BeginChatUnpair(request,context);cJSON_Delete(request);
        assert(!app.chat_unpair_context_ && app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::Wake);
    }
    if (RECOVERY_CASE==33) {
        app.SetLessonRuntimeActive(true);app.SetLessonRuntimeActive(false);
        app.ScheduleReconnect(kListeningModeAutoStop,false);
        assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None && "background scheduling cannot renew stale lesson ownership");
    }
    if (RECOVERY_CASE==0 || RECOVERY_CASE>=9) now_us=500100;
    app.HandleReconnectTick();
    if (RECOVERY_CASE==25) queue_accept=false;
    app.chat_outbound_worker_.RunOnce(now_us);app.PollChatOutbound();
    if (RECOVERY_CASE==25) {
        for(int i=0;i<100;++i)app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(!queued_ready && app.chat_protocol_state_==1 && protocol->opens==0);
        queue_accept=true;
    }
    app.PollChatProtocolCleanup();RunNetwork(app);
    if (RECOVERY_CASE==29) {
        app.EnterRepairPairingMode();app.RunTasks();
        assert(reboots==1 && app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None);
    }
    if (RECOVERY_CASE==20) app.HandleChatWake({},true);
    if (RECOVERY_CASE==15) queue_accept=false;
    if (RECOVERY_CASE==16) fail_next_context=true;
    if (RECOVERY_CASE==17) protocol->open_success=false;
    app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
    if (RECOVERY_CASE==18) app.protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
    RunNetwork(app);app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
    if (RECOVERY_CASE==32) {
        assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None && "failed completion cannot recreate recovery after lesson takeover");
        assert(!app.protocol_work_lifetime_.Busy() && !queued_ready);
        now_us=100000000;
        app.HandleReconnectTick();app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==1 && !queued_ready && protocol->sends.empty() && !app.microphone_uplink_authorized_);
        return 0;
    }
    if (RECOVERY_CASE==26) {
        assert(!app.protocol_work_lifetime_.Busy() && !queued_ready);
        assert(protocol->opens==1 && !app.microphone_uplink_authorized_ && protocol->sends.empty());
        for(int i=0;i<100;++i)app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==1 && !queued_ready);
        now_us=app.chat_recovery_.retry_at_us;
        app.HandleReconnectTick();RunNetwork(app);app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==2 && app.chat_protocol_signals_->MatchesSource({2,8}));
        assert(!app.microphone_uplink_authorized_ && protocol->sends.empty());return 0;
    }
    if (RECOVERY_CASE==28) {
        assert(!app.protocol_work_lifetime_.Busy());
        app.HandleReconnectTick();app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==1 && !queued_ready && protocol->sends.empty());
        assert(!app.microphone_uplink_authorized_);return 0;
    }
    if (RECOVERY_CASE>=15 && RECOVERY_CASE<=18) {
        if (RECOVERY_CASE==18) RunNetwork(app);
        assert(!app.chat_recovery_.opening && !app.microphone_uplink_authorized_);
        const auto old_opens=protocol->opens;
        for(int i=0;i<100;++i)app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==old_opens && !queued_ready);
        queue_accept=true;protocol->open_success=true;
        now_us=app.chat_recovery_.retry_at_us;
        app.HandleReconnectTick();RunNetwork(app);app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(protocol->opens==old_opens+1);
    }
    if (RECOVERY_CASE>=10 && RECOVERY_CASE<=12) {
        assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None);
        assert(!app.microphone_uplink_authorized_ && !queued_ready);return 0;
    }
    if (RECOVERY_CASE==1 || RECOVERY_CASE==2 || RECOVERY_CASE==22 || RECOVERY_CASE==27 || RECOVERY_CASE==29 || RECOVERY_CASE==31 || RECOVERY_CASE==33 || (RECOVERY_CASE>=5 && RECOVERY_CASE<=9)) {
        assert(protocol->opens==0 && "cancelled intent must not reopen");
        assert(!app.microphone_uplink_authorized_ && protocol->sends.empty());
        return 0;
    }
    assert(protocol->opens==(RECOVERY_CASE==17 ? 2U : 1U) && "reconnect timer consumed while cleanup pending");
    assert(app.chat_protocol_signals_->MatchesSource({2,8}));
    assert(!app.microphone_uplink_authorized_);
    if (RECOVERY_CASE==3 || RECOVERY_CASE==4 || RECOVERY_CASE==21) {
        assert(app.chat_recovery_.kind==Application::ChatRecoveryIntent::Kind::None);
        assert(app.state==kDeviceStateIdle && protocol->sends.empty());return 0;
    }
    if (RECOVERY_WAKE) {
        for(int i=0;i<4;++i)app.PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
        assert(app.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
        if (RECOVERY_CASE==23) {
            assert(!app.chat_control_intents_.Size());
            app.HandleStateChangedEvent();assert(!app.microphone_uplink_authorized_);
            app.chat_outbound_worker_.RunOnce(now_us);app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
            app.HandleStateChangedEvent();assert(!app.microphone_uplink_authorized_);
            app.chat_audio_prepared_=app.chat_rearm_prepared_;
            app.chat_audio_reset_completed_=app.audio_service_.reset_token;
            app.HandleStateChangedEvent();assert(app.microphone_uplink_authorized_);
            assert((protocol->sends==std::vector<int>{int(Kind::ListenStart)}));return 0;
        }
        assert(app.chat_control_intents_.Size()==1);
        app.PollChatControls(now_us);
        assert(app.chat_wake_read_pending_ && protocol->sends.empty());
        app.chat_wake_read_pending_=false;app.chat_wake_read_result_="worker wake";
        app.PollChatControls(now_us);app.chat_outbound_worker_.RunOnce(now_us);app.PollChatOutbound();
        app.HandleStateChangedEvent();assert(!app.microphone_uplink_authorized_);
        app.chat_outbound_worker_.RunOnce(now_us);app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        app.HandleStateChangedEvent();assert(!app.microphone_uplink_authorized_);
        app.chat_audio_prepared_=app.chat_rearm_prepared_;
        app.chat_audio_reset_completed_=app.audio_service_.reset_token;
        app.HandleStateChangedEvent();assert(app.microphone_uplink_authorized_);
        assert((protocol->sends==std::vector<int>{int(Kind::Wake),int(Kind::ListenStart)}));
        assert((protocol->epochs==std::vector<uint32_t>{8,8}));
        const uint64_t expected_deadline=(RECOVERY_CASE==20 || RECOVERY_CASE==24) ? 10500100 : 10000100;
        assert((protocol->deadlines==std::vector<uint64_t>{expected_deadline,expected_deadline}));
    }
}
''')
    return fixture


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
@pytest.mark.parametrize("wake", [False, True])
def test_actual_recovery_cleanup_timer_retirement(tmp_path, sanitize, wake):
    run_terminal_application(tmp_path, sanitize, 1, fixture_transform=lambda f,s,h:
        "#define RECOVERY_CASE 0\n#define RECOVERY_WAKE " + str(int(wake)) + "\n" + recovery_fixture(f,s,h))


@pytest.mark.parametrize("case", range(1,26))
def test_actual_recovery_cancellation_and_clock(tmp_path, case):
    run_terminal_application(tmp_path, "address", 1, fixture_transform=lambda f,s,h:
        f"#define RECOVERY_CASE {case}\n#define RECOVERY_WAKE 1\n" + recovery_fixture(f,s,h))


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
@pytest.mark.parametrize("case", range(26,34))
def test_actual_recovery_watchdog_and_lifecycle(tmp_path, case, sanitize):
    run_terminal_application(tmp_path, sanitize, 1, fixture_transform=lambda f,s,h:
        f"#define RECOVERY_CASE {case}\n#define RECOVERY_WAKE 1\n" + recovery_fixture(f,s,h))
