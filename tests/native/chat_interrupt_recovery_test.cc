#include <algorithm>
void Setup(Application&);
void Start(Application&,ConnectionSource);
void ReadyForRearm(Application&);

void InterruptStop(Application& app, bool continuation=true, ConnectionSource source={1,7},
                   uint64_t protocol_generation=1) {
    auto* root=cJSON_Parse(continuation ?
        "{\"type\":\"tts\",\"state\":\"stop\",\"reason\":\"interrupt\",\"continue_listening\":true,\"listen_mode\":\"realtime\"}" :
        "{\"type\":\"tts\",\"state\":\"stop\",\"reason\":\"interrupt\",\"continue_listening\":false,\"listen_mode\":\"manual\"}");
    app.HandleChatTerminalStop(app.chat_protocol_signals_,protocol_generation,source,root,now_us);
    cJSON_Delete(root);
}

void InterruptSetup(Application& app) {
    Setup(app);app.exercise_interrupt_cleanup=true;
    app.state=kDeviceStateSpeaking;app.online_intent_=true;
    app.microphone_uplink_authorized_=true;
    app.chat_protocol_signals_->start_audio=app.chat_playout_response_;
}

void RetireInterruptWorker(Application& app) {
    app.chat_outbound_worker_.RunOnce(now_us);
    app.PollChatPlayout(now_us);
    assert(!app.chat_start_obsolete_reservation_ && app.chat_outbound_generation_);
}

void DeliverInterruptListen(Application& app) {
    app.AdvanceChatRearm(now_us);
    assert(app.chat_rearm_admitted_ && !app.microphone_uplink_authorized_);
    app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);
    assert(app.chat_rearm_delivery_ && !app.microphone_uplink_authorized_);
}

void TestOwnedInterruptRecovery() {
    now_us=100;
    {
        Application app;InterruptSetup(app);
        const auto poll_clock=now_us.load();
        now_us=110;InterruptStop(app);app.PollChatPlayout(poll_clock);
        assert(!app.chat_playout_recovery_ && app.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
        assert(app.chat_rearm_job_.deadline_us==110+ConversationPlayoutController::kTimeoutUs);
        now_us=100;
    }
    for (bool pending_listen : {false,true}) {
        Application app;InterruptSetup(app);
        auto* root=cJSON_Parse("{\"type\":\"tts\",\"state\":\"stop\",\"drainId\":\"chat:cancelled\",\"continue_listening\":true,\"listen_mode\":\"realtime\"}");
        app.HandleChatTerminalStop(app.chat_protocol_signals_,1,{1,7},root,now_us);cJSON_Delete(root);
        app.PollChatPlayout(now_us);
        if(pending_listen) {
            app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);
            app.AdvanceChatRearm(now_us);
        }
        const auto cleanup_before=app.cleanups;
        const auto old_id=pending_listen ? app.chat_rearm_job_.request_id : app.chat_playout_ack_.request_id;
        Barrier old_send;app.protocol_->send_barrier=&old_send;
        auto sending=std::async(std::launch::async,[&]{app.chat_outbound_worker_.RunOnce(now_us);});old_send.Wait();
        InterruptStop(app);app.PollChatPlayout(now_us);
        assert(app.chat_rearm_phase_==Application::ChatRearmPhase::Pending && !app.microphone_uplink_authorized_);
        assert(app.chat_start_obsolete_ack_.request_id==old_id && app.cleanups==cleanup_before+1);
        const auto prepared=app.chat_rearm_prepared_;
        app.chat_audio_prepared_=prepared;app.chat_audio_reset_completed_=2;
        app.AdvanceChatRearm(now_us);assert(!app.chat_rearm_admitted_ && !app.microphone_uplink_authorized_);
        old_send.Release();sending.get();app.protocol_->send_barrier=nullptr;
        app.PollChatPlayout(now_us);assert(!app.chat_playout_ready_ && !app.microphone_uplink_authorized_);
        RetireInterruptWorker(app);DeliverInterruptListen(app);app.AdvanceChatRearm(now_us);
        assert(app.microphone_uplink_authorized_ && app.state==kDeviceStateListening);
    }
    for (bool state_first : {false,true}) {
        Application app;InterruptSetup(app);
        if (state_first) {
            // Exercise the second intake consumer after normal drain delivery.
            auto* root=cJSON_Parse("{\"type\":\"tts\",\"state\":\"stop\",\"drainId\":\"chat:prior\",\"continue_listening\":true,\"listen_mode\":\"realtime\"}");
            app.HandleChatTerminalStop(app.chat_protocol_signals_,1,{1,7},root,now_us);cJSON_Delete(root);
            app.PollChatPlayout(now_us);app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);
            assert(app.chat_playout_ready_);
        }
        const auto old_reservation=app.chat_outbound_reservation_;
        InterruptStop(app);
        if (state_first) app.AdvanceChatRearm(now_us);else app.PollChatPlayout(now_us);
        assert(!app.chat_playout_recovery_ && "owned continuation interrupt must retain listening intent");
        assert(app.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
        assert(!app.chat_playout_ready_ && !app.tts_audio_accepting_ && !app.microphone_uplink_authorized_);
        assert(app.chat_playout_response_.response_generation==2 && app.speaking_generation_==2);
        assert(app.chat_playout_response_.reset_token==2 && app.chat_rearm_owner_.reset_token==2);
        assert(app.cleanups==1 && app.interrupt_resets==1 && app.chat_rearm_prepared_);
        assert(app.chat_rearm_job_.deadline_us==100+ConversationPlayoutController::kTimeoutUs);
        assert(app.chat_start_obsolete_reservation_==old_reservation);
        const auto prepared=app.chat_rearm_prepared_;
        now_us=200;InterruptStop(app);app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(app.cleanups==1 && app.chat_rearm_prepared_==prepared);
        assert(app.chat_rearm_job_.deadline_us==100+ConversationPlayoutController::kTimeoutUs);
        RetireInterruptWorker(app);DeliverInterruptListen(app);
        app.AdvanceChatRearm(now_us);assert(!app.microphone_uplink_authorized_);
        app.chat_audio_prepared_=prepared;app.AdvanceChatRearm(now_us);
        assert(!app.microphone_uplink_authorized_); // Reset completion is still owed.
        app.chat_audio_reset_completed_=2;app.audio_service_.reset_busy=true;
        app.AdvanceChatRearm(now_us);assert(!app.microphone_uplink_authorized_);
        app.audio_service_.reset_busy=false;app.AdvanceChatRearm(now_us);
        assert(app.microphone_uplink_authorized_ && app.state==kDeviceStateListening);
        assert(app.listening_mode_==kListeningModeRealtime && !app.chat_playout_ready_);
        now_us=100+ConversationPlayoutController::kTimeoutUs;
        InterruptStop(app);app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(app.microphone_uplink_authorized_ && app.cleanups==1);
        assert(std::count(app.protocol_->sends.begin(),app.protocol_->sends.end(),int(Kind::ListenStart))==1);
        assert(std::count(app.protocol_->sends.begin(),app.protocol_->sends.end(),int(Kind::DrainAck))==(state_first ? 1 : 0));
        now_us=100;
    }
    {
        Application app;InterruptSetup(app);InterruptStop(app);app.PollChatPlayout(now_us);
        RetireInterruptWorker(app);DeliverInterruptListen(app);
        app.HandleStopListeningEvent();
        app.chat_audio_prepared_=app.chat_rearm_prepared_;app.chat_audio_reset_completed_=2;
        app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(!app.microphone_uplink_authorized_ && app.state==kDeviceStateIdle);
    }
    for (auto result : {Result::Failed,Result::Stale}) {
        Application app;InterruptSetup(app);InterruptStop(app);app.PollChatPlayout(now_us);
        RetireInterruptWorker(app);app.AdvanceChatRearm(now_us);
        app.protocol_->result=result;app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);
        app.chat_audio_prepared_=app.chat_rearm_prepared_;app.chat_audio_reset_completed_=2;
        app.AdvanceChatRearm(now_us);
        assert(!app.microphone_uplink_authorized_ && app.chat_playout_recovery_);
    }
    {
        Application app;InterruptSetup(app);InterruptStop(app);app.PollChatPlayout(now_us);
        RetireInterruptWorker(app);app.AdvanceChatRearm(now_us);
        Barrier send;app.protocol_->send_barrier=&send;
        auto sending=std::async(std::launch::async,[&]{app.chat_outbound_worker_.RunOnce(now_us);});send.Wait();
        now_us=100+ConversationPlayoutController::kTimeoutUs;
        app.AdvanceChatRearm(now_us);assert(app.chat_playout_recovery_ && !app.microphone_uplink_authorized_);
        send.Release();sending.get();app.protocol_->send_barrier=nullptr;
        app.PollChatOutbound();app.AdvanceChatRearm(now_us);
        assert(!app.microphone_uplink_authorized_);now_us=100;
    }
    for (int replacement=0;replacement<6;++replacement) {
        Application app;InterruptSetup(app);InterruptStop(app);app.PollChatPlayout(now_us);
        RetireInterruptWorker(app);DeliverInterruptListen(app);
        if(replacement==0)++app.protocol_generation_;
        if(replacement==1)++app.connect_generation_;
        if(replacement==2) { app.chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();assert(app.chat_protocol_signals_->EnableForSource({2,8})); }
        if(replacement==3)app.protocol_->healthy_epoch=8;
        if(replacement==4)++app.speaking_generation_;
        if(replacement==5)++app.audio_service_.reset_token;
        app.chat_audio_prepared_=app.chat_rearm_prepared_;app.chat_audio_reset_completed_=2;
        app.microphone_uplink_authorized_=false;
        const auto cleanups=app.cleanups;app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(!app.microphone_uplink_authorized_);
        if(replacement>=4)assert(app.cleanups==cleanups);
    }
    {
        Application app;InterruptSetup(app);InterruptStop(app);app.PollChatPlayout(now_us);
        RetireInterruptWorker(app);DeliverInterruptListen(app);
        ++app.speaking_generation_;++app.audio_service_.reset_token;
        app.microphone_uplink_authorized_=true;app.state=kDeviceStateListening;
        const auto cleanups=app.cleanups;
        app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(app.microphone_uplink_authorized_ && app.state==kDeviceStateListening && app.cleanups==cleanups);
    }
    {
        Application app;InterruptSetup(app);InterruptStop(app);app.PollChatPlayout(now_us);
        const auto old_prepared=app.chat_rearm_prepared_;
        receiver_wait=[&]{app.PollChatStart(now_us);};Start(app,{1,7});receiver_wait={};
        assert(app.chat_playout_response_.response_generation==3 && app.chat_playout_response_.reset_token==3);
        app.chat_audio_prepared_=old_prepared;app.chat_audio_reset_completed_=2;
        app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(!app.microphone_uplink_authorized_ && app.chat_rearm_phase_==Application::ChatRearmPhase::None);
    }
    for (int guard=0;guard<10;++guard) {
        Application app;InterruptSetup(app);
        if(guard==0)app.online_intent_=false;
        if(guard==1)app.passive_ws_intent_=true;
        if(guard==2)app.microphone_uplink_authorized_=false;
        if(guard==3)app.state=kDeviceStateIdle;
        if(guard==4)app.audio_service_.reset_busy=true;
        if(guard==5)app.chat_outbound_fault_=true;
        if(guard==6)app.chat_audio_fault_=true;
        if(guard==7)app.protocol_->healthy_epoch=8;
        if(guard==8)app.lesson_asset_sync_quiet_=true;
        InterruptStop(app,guard!=9);app.PollChatPlayout(now_us);app.AdvanceChatRearm(now_us);
        assert(app.chat_playout_recovery_ && !app.microphone_uplink_authorized_);
        assert(app.protocol_->sends.empty() && !app.chat_playout_ready_);
    }
    {
        Application app;InterruptSetup(app);
        InterruptStop(app,true,{2,7});InterruptStop(app,true,{1,8});InterruptStop(app,true,{1,7},2);
        app.PollChatPlayout(now_us);
        assert(!app.cleanups && app.microphone_uplink_authorized_ && !app.chat_playout_recovery_);
    }
    {
        Application app;InterruptSetup(app);
        auto* root=cJSON_Parse("{\"type\":\"tts\",\"state\":\"stop\",\"continue_listening\":true,\"listen_mode\":\"realtime\"}");
        app.HandleChatTerminalStop(app.chat_protocol_signals_,1,{1,7},root,now_us);cJSON_Delete(root);
        app.PollChatPlayout(now_us);
        assert(app.chat_playout_recovery_ && app.protocol_->sends.empty() && !app.microphone_uplink_authorized_);
    }
    {
        Application app;InterruptSetup(app);InterruptStop(app);
        now_us=100+ConversationPlayoutController::kTimeoutUs;app.PollChatPlayout(now_us);
        assert(app.chat_playout_recovery_ && !app.microphone_uplink_authorized_ && app.protocol_->sends.empty());
        now_us=100;
    }
    {
        Application app;InterruptSetup(app);
        auto terminal=[&](const char* id) {
            auto* root=cJSON_CreateObject();cJSON_AddStringToObject(root,"type","tts");cJSON_AddStringToObject(root,"state","stop");
            cJSON_AddStringToObject(root,"drainId",id);cJSON_AddBoolToObject(root,"continue_listening",true);cJSON_AddStringToObject(root,"listen_mode","realtime");
            app.HandleChatTerminalStop(app.chat_protocol_signals_,1,{1,7},root,now_us);cJSON_Delete(root);
        };
        terminal("chat:first");terminal("chat:conflict");InterruptStop(app);app.PollChatPlayout(now_us);
        assert(app.chat_playout_recovery_ && !app.microphone_uplink_authorized_ && app.protocol_->sends.empty());
    }
}
