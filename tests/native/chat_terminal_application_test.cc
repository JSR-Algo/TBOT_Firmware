bool Application::CompletePendingProtocolWork() { return false; }
void Setup(Application& app) {
    app.InitializeChatOutboundWorker();
    assert(app.ActivateChatOutbound(7)==Result::Sent);
    assert(app.chat_protocol_signals_->EnableForSource({1,7}));
    assert(app.EstablishChatPlayoutResponse({{1,7},1,1,1,1}));
}
void Stop(Application& app,const std::string& id="chat:one",bool interrupt=false,ConnectionSource source={1,7}) {
    auto* root=cJSON_CreateObject();
    cJSON_AddStringToObject(root,"type","tts");cJSON_AddStringToObject(root,"state","stop");
    cJSON_AddStringToObject(root,"drainId",id.c_str());
    cJSON_AddBoolToObject(root,"continue_listening",!interrupt);
    cJSON_AddStringToObject(root,"listen_mode",interrupt ? "manual" : "realtime");
    if(interrupt)cJSON_AddStringToObject(root,"reason","interrupt");
    app.HandleChatTerminalStop(app.chat_protocol_signals_,1,source,root);
    cJSON_Delete(root);
}
void Start(Application& app, ConnectionSource source={1,7}) {
    auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
    auto* root=cJSON_CreateObject();
    cJSON_AddStringToObject(root,"type","tts");cJSON_AddStringToObject(root,"state","start");
    callbacks.json(source,root,0,{now_us,0});
    cJSON_Delete(root);
}
void ReadyForRearm(Application& app) {
    Setup(app);app.state=kDeviceStateSpeaking;
    app.online_intent_=true;app.microphone_uplink_authorized_=true;
    Stop(app);app.PollChatPlayout(now_us);app.chat_outbound_worker_.RunOnce(now_us);app.PollChatPlayout(now_us);
}
int main() {
    now_us=100;
    {
        Application json;Setup(json);
        auto callbacks=json.MakeChatSourceCallbacks(1,json.chat_protocol_signals_);
        auto* frame=cJSON_Parse("{\"type\":\"stt\",\"text\":\"owned\"}");
        callbacks.json({1,7},frame,88,{now_us,0});cJSON_Delete(frame);
        assert(json.chat_inbound_messages_.Outstanding()==1);
        json.PollChatInboundMessages();assert(json.dispatched_text=="owned" && json.dispatched_epoch==88);
    }
    {
        Application connection;Setup(connection);
        auto id=connection.RequestChatConnectionText("{\"payload\":\""+std::string(2500,'x')+"\"}");
        assert(id);connection.PollChatConnectionMessages(now_us);
        assert(connection.chat_connection_messages_.Front()->submitted);
        connection.chat_outbound_worker_.RunOnce(now_us);connection.PollChatOutbound();
        connection.PollChatConnectionMessages(now_us);
        assert(!connection.chat_connection_messages_.Size());
        connection.protocol_->result=Result::Stale;
        assert(connection.RequestChatConnectionText("retry"));connection.PollChatConnectionMessages(now_us);
        const auto first_request=connection.chat_connection_messages_.Front()->physical.request_id;
        const auto deadline=connection.chat_connection_messages_.Front()->deadline_us;
        connection.chat_outbound_worker_.RunOnce(now_us);connection.PollChatOutbound();
        connection.protocol_->result=Result::Sent;connection.PollChatConnectionMessages(now_us);
        assert(connection.chat_connection_messages_.Front()->physical.request_id!=first_request);
        assert(connection.chat_connection_messages_.Front()->deadline_us==deadline);
        connection.chat_outbound_worker_.RunOnce(now_us);connection.PollChatOutbound();connection.PollChatConnectionMessages(now_us);
        assert(!connection.chat_connection_messages_.Size());
    }
    {
        for (auto result : {Result::Sent,Result::Failed,Result::Busy}) {
            Application unpair;Setup(unpair);auto context=std::make_shared<ChatInboundMessage>();
            context->owner={{1,7},1,1};context->received_us=now_us;context->deadline_us=now_us+10000000ULL;
            auto* request=cJSON_Parse("{\"request_id\":\"unpair:1\"}");
            unpair.BeginChatUnpair(request,context);cJSON_Delete(request);
            assert(!unpair.repairs && unpair.chat_unpair_context_==context);
            unpair.protocol_->result=result;unpair.PollChatConnectionMessages(now_us);
            unpair.PollChatUnpair(now_us);assert(!unpair.repairs);
            unpair.chat_outbound_worker_.RunOnce(now_us);unpair.PollChatOutbound();
            unpair.PollChatConnectionMessages(now_us);unpair.PollChatUnpair(now_us);
            if(result==Result::Busy){
                assert(!unpair.repairs);unpair.PollChatUnpair(now_us+10000000ULL);
            }
            assert(unpair.repairs==1);
        }
    }
    {
        Application manual_interrupt;Setup(manual_interrupt);manual_interrupt.state=kDeviceStateSpeaking;
        manual_interrupt.microphone_uplink_authorized_=true;
        manual_interrupt.chat_protocol_signals_->start_audio=manual_interrupt.chat_playout_response_;
        manual_interrupt.HandleStartListeningEvent();
        assert(!manual_interrupt.tts_audio_accepting_ && !manual_interrupt.microphone_uplink_authorized_);
        assert(manual_interrupt.chat_control_intents_.Size()==1);
        assert(manual_interrupt.chat_rearm_mode_==kListeningModeManualStop);
        manual_interrupt.HandleChatAudio(manual_interrupt.chat_protocol_signals_,1,{1,7},std::make_unique<AudioStreamPacket>());
        assert(manual_interrupt.audio_service_.received_generations.empty());
        manual_interrupt.PollChatControls(now_us);
        Barrier transport;manual_interrupt.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{manual_interrupt.chat_outbound_worker_.RunOnce(now_us);});transport.Wait();
        manual_interrupt.HandleStateChangedEvent();assert(!manual_interrupt.chat_rearm_admitted_);
        transport.Release();sending.get();manual_interrupt.protocol_->send_barrier=nullptr;
        manual_interrupt.PollChatPlayout(now_us);manual_interrupt.HandleStateChangedEvent();
        assert(manual_interrupt.chat_rearm_admitted_ && !manual_interrupt.microphone_uplink_authorized_);
        manual_interrupt.chat_outbound_worker_.RunOnce(now_us);manual_interrupt.PollChatPlayout(now_us);
        manual_interrupt.HandleStateChangedEvent();assert(!manual_interrupt.microphone_uplink_authorized_);
        manual_interrupt.chat_audio_prepared_=manual_interrupt.chat_rearm_prepared_;
        manual_interrupt.HandleStateChangedEvent();assert(manual_interrupt.microphone_uplink_authorized_);
        assert((manual_interrupt.protocol_->sends==std::vector<int>{int(Kind::Abort),int(Kind::ListenStart)}));
    }
    {
        for(bool mode_entry : {false,true}) {
            Application user;Setup(user);user.state=kDeviceStateIdle;user.passive_ws_intent_=true;user.online_intent_=false;
            if(mode_entry)user.SetListeningMode(kListeningModeManualStop);else user.HandleStartListeningEvent();
            assert(!user.passive_ws_intent_ && user.online_intent_);
            assert(user.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
            assert(user.chat_rearm_mode_==kListeningModeManualStop && user.protocol_->sends.empty());
            for(int guard=0;guard<3;++guard) {
                Application blocked;Setup(blocked);blocked.state=kDeviceStateIdle;blocked.passive_ws_intent_=true;blocked.online_intent_=false;
                if(guard==0)blocked.lesson_asset_sync_quiet_=true;
                if(guard==1)blocked.state=kDeviceStateWifiConfiguring;
                if(guard==2)blocked.protocol_.reset();
                if(mode_entry)blocked.SetListeningMode(kListeningModeManualStop);else blocked.HandleStartListeningEvent();
                assert(blocked.passive_ws_intent_ && !blocked.online_intent_ && !blocked.chat_control_intents_.Size());
            }
        }
    }
    {
        Application change_pending;Setup(change_pending);change_pending.state=kDeviceStateIdle;
        change_pending.SetListeningMode(kListeningModeRealtime);
        change_pending.HandleStartListeningEvent();
        assert(change_pending.chat_rearm_mode_==kListeningModeManualStop);
        assert(change_pending.chat_control_intents_.Size()==1);
        change_pending.HandleStartListeningEvent();assert(change_pending.chat_control_intents_.Size()==1);
    }
    {
        Application ack_abort;Setup(ack_abort);ack_abort.state=kDeviceStateSpeaking;ack_abort.online_intent_=true;
        Stop(ack_abort);ack_abort.PollChatPlayout(now_us);assert(ack_abort.chat_playout_ack_admitted_);
        Barrier transport;ack_abort.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{ack_abort.chat_outbound_worker_.RunOnce(now_us);});transport.Wait();
        ack_abort.HandleChatAbort(kAbortReasonNone,true);
        assert(ack_abort.chat_control_intents_.Size()==2);
        transport.Release();sending.get();ack_abort.protocol_->send_barrier=nullptr;
        ack_abort.PollChatPlayout(now_us);ack_abort.PollChatControls(now_us);
        ack_abort.chat_outbound_worker_.RunOnce(now_us);ack_abort.PollChatPlayout(now_us);
        ack_abort.HandleStateChangedEvent();assert(ack_abort.chat_rearm_admitted_ && !ack_abort.chat_playout_recovery_);
        assert((ack_abort.protocol_->sends==std::vector<int>{int(Kind::DrainAck),int(Kind::Abort)}));
    }
    {
        Application passive_wake;Setup(passive_wake);passive_wake.state=kDeviceStateIdle;passive_wake.passive_ws_intent_=true;
        passive_wake.HandleWakeWordDetectedEvent();
        assert(!passive_wake.passive_ws_intent_ && passive_wake.online_intent_);
        assert(passive_wake.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
        Application finish_connect;Setup(finish_connect);finish_connect.state=kDeviceStateConnecting;
        finish_connect.FinishWakeWordInvoke("hello");
        assert(finish_connect.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
    }
    {
        Application expired_control;Setup(expired_control);
        assert(expired_control.RequestChatControl(Kind::Wake,0,"old"));
        now_us=10000100;expired_control.PollChatControls(now_us);
        expired_control.chat_outbound_worker_.RunOnce(now_us);expired_control.PollChatOutbound();
        expired_control.PollChatControls(now_us);
        assert(!expired_control.chat_control_intents_.Size());now_us=100;
    }
    {
        Application before_response;before_response.InitializeChatOutboundWorker();
        assert(before_response.ActivateChatOutbound(7)==Result::Sent);
        assert(before_response.chat_protocol_signals_->EnableForSource({1,7}));
        before_response.audio_service_.reset_token=0;before_response.chat_audio_reset_serial_=0;
        before_response.state=kDeviceStateListening;
        before_response.HandleStopListeningEvent();before_response.HandleStateChangedEvent();
        assert(before_response.state==kDeviceStateIdle && before_response.protocol_->sends.empty());
        assert(before_response.chat_control_intents_.Size()==1);
    }
    {
        Application terminal_abort;ReadyForRearm(terminal_abort);
        terminal_abort.chat_audio_reset_serial_=2;terminal_abort.audio_service_.reset_token=2;
        assert(terminal_abort.HandleChatAbort(kAbortReasonNone,true));
        terminal_abort.chat_audio_reset_completed_=2;
        terminal_abort.PollChatPlayout(now_us);
        assert(!terminal_abort.chat_playout_begun_ && !terminal_abort.chat_playout_recovery_);
        terminal_abort.PollChatControls(now_us);terminal_abort.chat_outbound_worker_.RunOnce(now_us);terminal_abort.PollChatPlayout(now_us);
        assert(terminal_abort.protocol_->sends.back()==int(Kind::Abort));
    }
    {
        Application expired_active;Setup(expired_active);
        assert(expired_active.RequestChatControl(Kind::Abort));expired_active.PollChatControls(now_us);
        auto reservation=expired_active.chat_outbound_reservation_;
        Barrier transport;expired_active.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{expired_active.chat_outbound_worker_.RunOnce(now_us);});transport.Wait();
        now_us=10000100;expired_active.PollChatControls(now_us);expired_active.PollChatControls(now_us);
        assert(expired_active.chat_control_intents_.Size()==1 && expired_active.chat_outbound_reservation_==reservation);
        transport.Release();sending.get();expired_active.protocol_->send_barrier=nullptr;
        expired_active.chat_outbound_worker_.RunOnce(now_us);expired_active.PollChatOutbound();expired_active.PollChatControls(now_us);
        assert(!expired_active.chat_control_intents_.Size() && !expired_active.chat_outbound_reservation_);
        now_us=100;
    }
    {
        Application audio_event;Setup(audio_event);audio_event.SelectedSendAudioEvent();
        assert(audio_event.protocol_->sends.empty());
        Application watchdog;Setup(watchdog);watchdog.state=kDeviceStateListening;watchdog.listening_mode_=kListeningModeAutoStop;
        watchdog.listening_started_ms_=1;watchdog.last_listening_activity_ms_=1;now_us=2000000;
        watchdog.HandleListeningWatchdogTick();assert(watchdog.chat_control_intents_.Size()==1 && watchdog.cues==1);
        assert(watchdog.protocol_->sends.empty());
        Application timeout;Setup(timeout);timeout.state=kDeviceStateSpeaking;timeout.listening_mode_=kListeningModeAutoStop;
        timeout.HandleSpeakingTimeout(0);assert(!timeout.chat_control_intents_.Size());
        timeout.last_speaking_activity_ms_=1999;timeout.HandleSpeakingTimeout(1);assert(timeout.timeout_rearms==1);
        timeout.last_speaking_activity_ms_=1;timeout.HandleSpeakingTimeout(1);
        assert(timeout.chat_control_intents_.Size()==1 && timeout.cues==1 && timeout.protocol_->sends.empty());
        now_us=100;
    }
    {
        Application manual;Setup(manual);manual.state=kDeviceStateIdle;
        manual.HandleStartListeningEvent();assert(manual.chat_listen_origin_==Application::ChatListenOrigin::User);
        assert(manual.protocol_->sends.empty());
        Application detected;Setup(detected);detected.state=kDeviceStateIdle;
        detected.HandleWakeWordDetectedEvent();assert(detected.protocol_->sends.empty());
        Application continued;Setup(continued);continued.state=kDeviceStateIdle;
        continued.ContinueWakeWordInvoke("hello");assert(continued.protocol_->sends.empty());
        Application finished;Setup(finished);finished.state=kDeviceStateIdle;
        finished.FinishWakeWordInvoke("hello");assert(finished.protocol_->sends.empty());
        Application aborted;Setup(aborted);aborted.state=kDeviceStateSpeaking;
        aborted.AbortSpeaking(kAbortReasonNone);assert(aborted.protocol_->sends.empty());
    }
    {
        Application wake;Setup(wake);wake.state=kDeviceStateIdle;
        assert(wake.HandleChatWake("hello"));
        assert(wake.chat_rearm_mode_==kListeningModeAutoStop);
#if CONFIG_SEND_WAKE_WORD_DATA
        assert(wake.chat_control_intents_.Size()==1);
#else
        assert(wake.chat_control_intents_.Size()==0);
#endif
        for(int guard=0;guard<3;++guard) {
            Application quiet;Setup(quiet);quiet.state=kDeviceStateIdle;
            if(guard==0)quiet.lesson_asset_sync_quiet_=true;
            if(guard==1)quiet.protocol_.reset();
            if(guard==2)quiet.state=kDeviceStateWifiConfiguring;
            assert(quiet.HandleChatWake("hello"));assert(!quiet.chat_control_intents_.Size());
            assert(quiet.chat_rearm_phase_==Application::ChatRearmPhase::None);
        }
    }
    {
        Application source_replaced;Setup(source_replaced);
        assert(source_replaced.RequestChatControl(Kind::Abort));source_replaced.PollChatControls(now_us);
        source_replaced.chat_protocol_signals_->Disable();
        assert(source_replaced.chat_protocol_signals_->EnableForSource({2,7}));
        source_replaced.PollChatControls(now_us);
        assert(source_replaced.chat_outbound_generation_==0);
        assert(source_replaced.chat_control_intents_.Front()->outcome==ChatControlIntents::Outcome::Superseded);
        source_replaced.chat_outbound_worker_.RunOnce(now_us);source_replaced.PollChatOutbound();
        source_replaced.PollChatControls(now_us);assert(!source_replaced.chat_control_intents_.Size());
        assert(source_replaced.protocol_->sends.empty());
    }
    {
        Application fresh_after_recovery;Setup(fresh_after_recovery);
        fresh_after_recovery.RecoverChatPlayout();fresh_after_recovery.state=kDeviceStateIdle;
        assert(fresh_after_recovery.BeginChatListen(kListeningModeRealtime,Application::ChatListenOrigin::User));
        assert(fresh_after_recovery.chat_playout_response_.response_generation==fresh_after_recovery.speaking_generation_.load());
        assert(fresh_after_recovery.audio_service_.IsCurrentChatPlaybackReset(fresh_after_recovery.chat_playout_response_.reset_token));
        fresh_after_recovery.chat_outbound_worker_.RunOnce(now_us);
        fresh_after_recovery.PollChatPlayout(now_us);fresh_after_recovery.HandleStateChangedEvent();
        assert(fresh_after_recovery.chat_outbound_generation_!=0);
        assert(fresh_after_recovery.chat_rearm_admitted_);
    }
    {
        Application connecting;Setup(connecting);connecting.state=kDeviceStateConnecting;
        assert(connecting.BeginChatListen(kListeningModeRealtime,Application::ChatListenOrigin::User));
        assert(connecting.state==kDeviceStateConnecting);
        connecting.HandleStateChangedEvent();assert(connecting.chat_rearm_admitted_);
        connecting.chat_audio_prepared_=connecting.chat_rearm_prepared_;
        connecting.chat_outbound_worker_.RunOnce(now_us);connecting.PollChatPlayout(now_us);
        connecting.HandleStateChangedEvent();assert(connecting.state==kDeviceStateListening);
    }
    {
        Application pending_abort;Setup(pending_abort);pending_abort.state=kDeviceStateIdle;
        assert(pending_abort.BeginChatListen(kListeningModeRealtime,Application::ChatListenOrigin::User));
        pending_abort.HandleStateChangedEvent();assert(pending_abort.chat_rearm_admitted_);
        const auto active_id=pending_abort.chat_rearm_job_.request_id;
        Barrier transport;pending_abort.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{pending_abort.chat_outbound_worker_.RunOnce(now_us);});transport.Wait();
        assert(pending_abort.HandleChatAbort(kAbortReasonNone,true));
        assert(pending_abort.chat_control_intents_.Size()==2);
        assert(pending_abort.chat_control_intents_.Front()->job.request_id==active_id);
        assert(pending_abort.chat_control_intents_.Front()->admitted);
        assert(pending_abort.chat_rearm_job_.request_id==0);
        pending_abort.PollChatPlayout(now_us);pending_abort.HandleStateChangedEvent();
        assert(!pending_abort.microphone_uplink_authorized_ && !pending_abort.chat_rearm_admitted_);
        transport.Release();sending.get();pending_abort.protocol_->send_barrier=nullptr;
        pending_abort.PollChatPlayout(now_us);pending_abort.PollChatControls(now_us);
        pending_abort.chat_outbound_worker_.RunOnce(now_us);pending_abort.PollChatPlayout(now_us);
        pending_abort.HandleStateChangedEvent();assert(pending_abort.chat_rearm_admitted_);
        assert(pending_abort.chat_rearm_job_.request_id!=active_id);
        pending_abort.chat_audio_prepared_=pending_abort.chat_rearm_prepared_;
        pending_abort.chat_outbound_worker_.RunOnce(now_us);pending_abort.PollChatPlayout(now_us);
        pending_abort.HandleStateChangedEvent();
        assert(pending_abort.microphone_uplink_authorized_ && pending_abort.state==kDeviceStateListening);
        assert((pending_abort.protocol_->sends==std::vector<int>{int(Kind::ListenStart),int(Kind::Abort),int(Kind::ListenStart)}));
    }
    {
        Application active_capacity;Setup(active_capacity);active_capacity.state=kDeviceStateIdle;
        assert(active_capacity.BeginChatListen(kListeningModeRealtime,Application::ChatListenOrigin::User));
        assert(active_capacity.RequestChatControl(Kind::Wake,0,"one"));
        assert(active_capacity.RequestChatControl(Kind::Wake,0,"two"));
        assert(active_capacity.RequestChatControl(Kind::Wake,0,"three"));
        assert(!active_capacity.RequestChatControl(Kind::Wake,0,"fifth"));
        assert(active_capacity.chat_playout_recovery_);
    }
    {
        Application controls;Setup(controls);controls.state=kDeviceStateListening;
        controls.HandleStopListeningEvent();
        assert(controls.protocol_->sends.empty() && controls.chat_control_intents_.Size()==1);
        assert(controls.RequestChatControl(Kind::Wake,0,"hello"));
        assert(controls.RequestChatControl(Kind::Abort));
        assert(controls.RequestChatControl(Kind::ListenStop));
        assert(controls.chat_control_intents_.Size()==4);
        assert(!controls.RequestChatControl(Kind::Wake,0,"overflow"));
        assert(controls.cleanups>=2 && !controls.microphone_uplink_authorized_);
    }
    {
        Application fifo;Setup(fifo);fifo.state=kDeviceStateListening;
        fifo.online_intent_=true;fifo.microphone_uplink_authorized_=true;
        fifo.HandleStopListeningEvent();assert(fifo.RequestChatControl(Kind::Wake,0,"hello"));
        assert(fifo.BeginChatListen(kListeningModeRealtime,Application::ChatListenOrigin::Wake));
        fifo.HandleStateChangedEvent();assert(!fifo.chat_rearm_admitted_);
        fifo.PollChatControls(now_us);
        Barrier transport;fifo.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{fifo.chat_outbound_worker_.RunOnce(now_us);});transport.Wait();
        fifo.HandleStateChangedEvent();assert(!fifo.chat_rearm_admitted_);
        auto received=fifo.chat_listen_received_us_;fifo.BeginChatListen(kListeningModeRealtime,Application::ChatListenOrigin::Wake);
        assert(fifo.chat_listen_received_us_==received);
        transport.Release();sending.get();fifo.protocol_->send_barrier=nullptr;fifo.PollChatOutbound();
        fifo.PollChatControls(now_us);fifo.chat_outbound_worker_.RunOnce(now_us);fifo.PollChatOutbound();
        fifo.HandleStateChangedEvent();assert(fifo.chat_rearm_admitted_);
        fifo.chat_outbound_worker_.RunOnce(now_us);fifo.PollChatPlayout(now_us);
        assert((fifo.protocol_->sends==std::vector<int>{int(Kind::ListenStop),int(Kind::Wake),int(Kind::ListenStart)}));
    }
    {
        Application old_abort;Setup(old_abort);old_abort.state=kDeviceStateSpeaking;
        assert(old_abort.HandleChatAbort(kAbortReasonNone,false));old_abort.PollChatControls(now_us);
        Barrier transport;old_abort.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{old_abort.chat_outbound_worker_.RunOnce(now_us);});transport.Wait();
        receiver_wait=[&]{old_abort.PollChatStart(now_us);};Start(old_abort);receiver_wait={};
        assert(old_abort.chat_control_intents_.Front()->outcome==ChatControlIntents::Outcome::Superseded);
        transport.Release();sending.get();old_abort.protocol_->send_barrier=nullptr;
        for(int i=0;i<6;++i){old_abort.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);old_abort.chat_outbound_worker_.RunOnce(now_us);}
        assert(old_abort.protocol_->sends.size()==1);
        assert(old_abort.chat_playout_response_.response_generation==2);
        assert(!old_abort.chat_control_intents_.Size());
    }
    {
        Application cancelled;ReadyForRearm(cancelled);cancelled.HandleStateChangedEvent();
        cancelled.state=kDeviceStateIdle;
        cancelled.chat_audio_prepared_=cancelled.chat_rearm_prepared_;
        cancelled.chat_outbound_worker_.RunOnce(now_us);cancelled.PollChatPlayout(now_us);
        cancelled.HandleStateChangedEvent();
        assert(cancelled.state==kDeviceStateIdle && !cancelled.microphone_uplink_authorized_);
        assert(Board::GetInstance().status!="listening");
    }
    {
        Application stopped;ReadyForRearm(stopped);stopped.HandleStateChangedEvent();
        stopped.chat_audio_prepared_=stopped.chat_rearm_prepared_;
        stopped.chat_outbound_worker_.RunOnce(now_us);stopped.PollChatPlayout(now_us);stopped.HandleStateChangedEvent();
        assert(stopped.microphone_uplink_authorized_);
        stopped.HandleStopListeningEvent();stopped.HandleStateChangedEvent();
        assert(!stopped.microphone_uplink_authorized_ && stopped.state==kDeviceStateIdle);
        assert(Board::GetInstance().status!="listening");
    }
    {
        Application rearm;Setup(rearm);rearm.state=kDeviceStateSpeaking;
        rearm.online_intent_=true;rearm.microphone_uplink_authorized_=true;
        Stop(rearm);rearm.PollChatPlayout(now_us);
        rearm.chat_outbound_worker_.RunOnce(now_us);rearm.PollChatPlayout(now_us);
        assert(rearm.chat_playout_ready_);
        rearm.HandleStateChangedEvent();
        assert(rearm.chat_rearm_admitted_);
        auto listen_id=rearm.chat_rearm_job_.request_id;
        assert(Board::GetInstance().status=="wait" && rearm.state==kDeviceStateSpeaking);
        assert(!rearm.microphone_uplink_authorized_);
        rearm.HandleStateChangedEvent();assert(rearm.chat_rearm_job_.request_id==listen_id);
        rearm.chat_outbound_worker_.RunOnce(now_us);rearm.PollChatPlayout(now_us);
        rearm.HandleStateChangedEvent();assert(!rearm.microphone_uplink_authorized_);
        rearm.chat_audio_prepared_=rearm.chat_rearm_prepared_;
        rearm.HandleStateChangedEvent();
        assert(rearm.chat_rearm_phase_==Application::ChatRearmPhase::Armed);
        assert(rearm.microphone_uplink_authorized_ && rearm.state==kDeviceStateListening);
        assert(Board::GetInstance().status=="listening" && !rearm.chat_playout_ready_);
        now_us=20000000;Stop(rearm);rearm.PollChatPlayout(now_us);rearm.HandleStateChangedEvent();
        assert(rearm.microphone_uplink_authorized_ && rearm.cleanups==1);
        rearm.online_intent_=false;rearm.HandleStateChangedEvent();
        assert(!rearm.microphone_uplink_authorized_ && rearm.state==kDeviceStateIdle);
        assert(Board::GetInstance().status=="idle" && rearm.cleanups==2);
        now_us=100;
    }
    {
        Application busy;ReadyForRearm(busy);
        std::promise<void> held,release;auto released=release.get_future().share();
        auto holder=std::async(std::launch::async,[&]{std::lock_guard<std::mutex> lock(busy.chat_outbound_worker_.mailbox_.mutex_);held.set_value();released.wait();});
        held.get_future().wait();busy.HandleStateChangedEvent();
        auto id=busy.chat_rearm_job_.request_id;assert(id && !busy.chat_rearm_admitted_);
        busy.HandleStateChangedEvent();assert(busy.chat_rearm_job_.request_id==id && !busy.chat_rearm_admitted_);
        release.set_value();holder.get();busy.HandleStateChangedEvent();
        assert(busy.chat_rearm_admitted_ && busy.chat_rearm_job_.request_id==id && busy.cleanups==1);
    }
    {
        Application replacement;ReadyForRearm(replacement);replacement.HandleStateChangedEvent();
        auto id=replacement.chat_rearm_job_.request_id;
        Barrier send;replacement.protocol_->send_barrier=&send;
        auto sending=std::async(std::launch::async,[&]{replacement.chat_outbound_worker_.RunOnce(now_us);});send.Wait();
        receiver_wait=[&]{replacement.PollChatStart(now_us);};Start(replacement);receiver_wait={};
        assert(replacement.chat_start_obsolete_ack_.request_id==id);
        assert(replacement.chat_rearm_phase_==Application::ChatRearmPhase::None);
        replacement.chat_audio_prepared_=1;
        send.Release();sending.get();replacement.protocol_->send_barrier=nullptr;
        replacement.PollChatPlayout(now_us);replacement.chat_outbound_worker_.RunOnce(now_us);replacement.PollChatPlayout(now_us);
        replacement.HandleStateChangedEvent();assert(!replacement.microphone_uplink_authorized_ && replacement.cleanups==1);
        Stop(replacement,"chat:replacement");replacement.PollChatPlayout(now_us);
        for(int i=0;i<3 && !replacement.chat_playout_ready_;++i) {
            replacement.chat_outbound_worker_.RunOnce(now_us);replacement.PollChatPlayout(now_us);
        }
        assert(replacement.chat_playout_ready_);
        replacement.HandleStateChangedEvent();
        assert(replacement.chat_rearm_admitted_ && replacement.chat_rearm_phase_==Application::ChatRearmPhase::Pending);
    }
    {
        for(int loss=0;loss<5;++loss) {
            Application transfer;ReadyForRearm(transfer);transfer.HandleStateChangedEvent();
            assert(transfer.chat_rearm_voice_intent_);
            if(loss==0)assert(transfer.SelectChatProtocolSource({2,7},1,1));
            if(loss==1)transfer.online_intent_=false;
            if(loss==2)transfer.passive_ws_intent_=true;
            if(loss==3)transfer.lesson_runtime_active_=true;
            if(loss==4)transfer.state=kDeviceStateIdle;
            receiver_wait=[&]{transfer.PollChatStart(now_us);};
            Start(transfer,loss==0?ConnectionSource{2,7}:ConnectionSource{1,7});receiver_wait={};
            assert(!transfer.chat_rearm_voice_intent_);
        }
    }
    {
        for(auto mode : {kListeningModeManualStop,kListeningModeAutoStop,kListeningModeRealtime}) {
            Application matrix;ReadyForRearm(matrix);
            matrix.listening_mode_=mode;matrix.chat_playout_stop_.continue_listening=false;
            matrix.HandleStateChangedEvent();
            assert(matrix.chat_rearm_admitted_==(mode==kListeningModeRealtime));
            if(mode!=kListeningModeRealtime) {
                assert(matrix.state==kDeviceStateIdle && matrix.chat_rearm_phase_==Application::ChatRearmPhase::IdleComplete);
                now_us=20000000;matrix.PollChatPlayout(now_us);matrix.HandleStateChangedEvent();
                assert(matrix.cleanups==1);now_us=100;
            }
        }
        for(bool realtime : {false,true}) {
            Application mode;ReadyForRearm(mode);mode.chat_playout_stop_.realtime=realtime;
            mode.HandleStateChangedEvent();
            assert(mode.chat_rearm_job_.argument==(realtime?kListeningModeRealtime:kListeningModeAutoStop));
        }
        for(int reason=0;reason<3;++reason) {
            Application denied;ReadyForRearm(denied);
            if(reason==0)denied.passive_ws_intent_=true;
            if(reason==1)denied.online_intent_=false;
            if(reason==2)denied.microphone_uplink_authorized_=false;
            denied.HandleStateChangedEvent();assert(!denied.chat_rearm_admitted_ && !denied.microphone_uplink_authorized_);
        }
        Application lesson;ReadyForRearm(lesson);lesson.lesson_runtime_active_=true;
        assert(!lesson.AdvanceChatRearm(now_us) && !lesson.chat_rearm_admitted_);
    }
    {
        for(auto result : {Result::Failed,Result::Stale}) {
            Application failure;ReadyForRearm(failure);failure.HandleStateChangedEvent();
            failure.protocol_->result=result;failure.chat_outbound_worker_.RunOnce(now_us);failure.PollChatPlayout(now_us);
            failure.HandleStateChangedEvent();assert(failure.state==kDeviceStateIdle && !failure.microphone_uplink_authorized_);
        }
        Application changed;ReadyForRearm(changed);changed.HandleStateChangedEvent();
        changed.chat_audio_prepared_=changed.chat_rearm_prepared_;
        changed.chat_outbound_worker_.RunOnce(now_us);changed.PollChatPlayout(now_us);
        changed.protocol_->healthy_epoch=8;changed.HandleStateChangedEvent();
        assert(!changed.microphone_uplink_authorized_ && changed.state==kDeviceStateIdle);
    }
    {
        Application terminal_race;Setup(terminal_race);terminal_race.state=kDeviceStateSpeaking;
        terminal_race.online_intent_=true;terminal_race.microphone_uplink_authorized_=true;
        Stop(terminal_race);terminal_race.PollChatPlayout(now_us);terminal_race.chat_outbound_worker_.RunOnce(now_us);terminal_race.PollChatPlayout(now_us);
        terminal_race.HandleStateChangedEvent();terminal_race.chat_audio_prepared_=terminal_race.chat_rearm_prepared_;
        terminal_race.chat_outbound_worker_.RunOnce(now_us);terminal_race.PollChatPlayout(now_us);
        Stop(terminal_race,"chat:one",true);
        terminal_race.HandleStateChangedEvent();
        assert(!terminal_race.microphone_uplink_authorized_ && terminal_race.state==kDeviceStateIdle);
    }
    {
        Application prepared;Setup(prepared);prepared.state=kDeviceStateSpeaking;
        prepared.online_intent_=true;prepared.microphone_uplink_authorized_=true;
        Stop(prepared);prepared.PollChatPlayout(now_us);prepared.chat_outbound_worker_.RunOnce(now_us);prepared.PollChatPlayout(now_us);
        prepared.HandleStateChangedEvent();prepared.chat_audio_prepared_=prepared.chat_rearm_prepared_;
        prepared.HandleStateChangedEvent();assert(!prepared.microphone_uplink_authorized_);
        prepared.chat_outbound_worker_.RunOnce(now_us);prepared.PollChatPlayout(now_us);
        prepared.HandleStateChangedEvent();assert(prepared.microphone_uplink_authorized_);
    }
    {
        Application deadline;Setup(deadline);deadline.state=kDeviceStateSpeaking;
        deadline.online_intent_=true;deadline.microphone_uplink_authorized_=true;
        Stop(deadline);deadline.PollChatPlayout(now_us);deadline.chat_outbound_worker_.RunOnce(now_us);deadline.PollChatPlayout(now_us);
        deadline.HandleStateChangedEvent();deadline.chat_audio_prepared_=deadline.chat_rearm_prepared_;
        Barrier send;deadline.protocol_->send_barrier=&send;
        auto sender=std::async(std::launch::async,[&]{deadline.chat_outbound_worker_.RunOnce(now_us);});send.Wait();
        now_us=100+ConversationPlayoutController::kTimeoutUs;
        deadline.HandleStateChangedEvent();assert(!deadline.microphone_uplink_authorized_ && deadline.state==kDeviceStateIdle);
        send.Release();sender.get();deadline.PollChatOutbound();
        deadline.HandleStateChangedEvent();assert(!deadline.microphone_uplink_authorized_);
        // A retired recovery event cannot paint or change a newly selected source.
        assert(deadline.SelectChatProtocolSource({2,7},1,1));deadline.state=kDeviceStateListening;
        Board::GetInstance().status="successor";deadline.HandleStateChangedEvent();
        assert(deadline.state==kDeviceStateListening && Board::GetInstance().status=="successor");
        now_us=100;
    }
    {
        Application start;Setup(start);
        start.lesson_asset_sync_quiet_=true;
        auto quiet_callbacks=start.MakeChatSourceCallbacks(1,start.chat_protocol_signals_);
        auto* quiet_frame=cJSON_Parse("{\"type\":\"tts\",\"state\":\"start\"}");
        receiver_wait=[&]{start.PollChatStart(now_us);};
        quiet_callbacks.json({1,7},quiet_frame,1,{now_us,0});cJSON_Delete(quiet_frame);
        receiver_wait={};
        ChatStartHandoff::Request quiet_request;
        assert(!start.chat_protocol_signals_->start.TryRequest(quiet_request));
        start.lesson_asset_sync_quiet_=false;
        start.state=kDeviceStateListening;start.aborted_=true;
        auto callbacks=start.MakeChatSourceCallbacks(1,start.chat_protocol_signals_);
        assert(callbacks.audio);
        receiver_wait=[&]{start.PollChatStart(now_us);};
        Start(start);
        receiver_wait={};
        assert(start.chat_playout_response_.response_generation==2);
        assert(start.chat_playout_response_.reset_token==2 && start.cleanups==0);
        callbacks.audio({1,7},std::make_unique<AudioStreamPacket>());
        assert(start.audio_service_.received_generations==std::vector<uint32_t>{2});
        assert(start.audio_service_.received_tokens==std::vector<uint32_t>{2});
        now_us=1000000;start.PollChatStart(now_us);
        assert(start.state==kDeviceStateSpeaking && !start.aborted_);
        assert(start.speaking_arm_dispatch_.begins==1 && start.timeout_rearms==1);
        assert(start.last_speaking_activity_ms_==1000);
        start.PollChatStart(now_us);assert(start.speaking_arm_dispatch_.begins==1 && start.timeout_rearms==1);
        assert(start.cleanups==0); // Confirmed admission is not a 250ms stream lifetime.
        callbacks.audio({2,7},std::make_unique<AudioStreamPacket>());
        assert(start.audio_service_.received_generations.size()==1);
        start.chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
        assert(start.SelectChatProtocolSource({2,7},1,1));
        receiver_wait=[&]{start.PollChatStart(now_us);};
        Start(start,{2,7});receiver_wait={};
        assert(start.chat_playout_response_.source.source_id==2);
        assert(start.chat_playout_response_.response_generation==3);
        now_us=100;
    }
    {
        Application sealed;Setup(sealed);
        receiver_wait=[&]{sealed.PollChatStart(now_us);};Start(sealed);receiver_wait={};
        auto callbacks=sealed.MakeChatSourceCallbacks(1,sealed.chat_protocol_signals_);
        callbacks.audio({1,7},std::make_unique<AudioStreamPacket>());
        Stop(sealed,"chat:sealed");
        callbacks.audio({1,7},std::make_unique<AudioStreamPacket>());
        assert(sealed.audio_service_.received_generations.size()==1);
        now_us=200;Stop(sealed,"chat:sealed");
        ChatPlayoutIntake::Stop original;
        assert(sealed.chat_protocol_signals_->intake.TryCollect(sealed.chat_playout_stamp_,original)==ChatPlayoutIntake::Read::Ready);
        assert(original.received_us==100 && !original.conflict);
        Stop(sealed,"chat:conflict");
        assert(sealed.chat_protocol_signals_->intake.TryCollect(sealed.chat_playout_stamp_,original)==ChatPlayoutIntake::Read::Ready);
        assert(original.received_us==100 && original.conflict);
        receiver_wait=[&]{sealed.PollChatStart(now_us);};Start(sealed);receiver_wait={};
        Stop(sealed,"chat:old-source",false,{2,7});
        callbacks.audio({1,7},std::make_unique<AudioStreamPacket>());
        assert((sealed.audio_service_.received_generations==std::vector<uint32_t>{2,3}));
        auto old_signals=sealed.chat_protocol_signals_;
        sealed.chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
        assert(sealed.SelectChatProtocolSource({2,7},1,1));
        receiver_wait=[&]{sealed.PollChatStart(now_us);};Start(sealed,{2,7});receiver_wait={};
        auto new_callbacks=sealed.MakeChatSourceCallbacks(1,sealed.chat_protocol_signals_);
        auto* old_stop=cJSON_Parse("{\"type\":\"tts\",\"state\":\"stop\",\"drainId\":\"chat:old\"}");
        callbacks.json({1,7},old_stop,0,{now_us,0});cJSON_Delete(old_stop);
        new_callbacks.audio({2,7},std::make_unique<AudioStreamPacket>());
        assert((sealed.audio_service_.received_generations==std::vector<uint32_t>{2,3,4}));
        now_us=100;
    }
    {
        Application threaded;Setup(threaded);
        auto receiver=std::async(std::launch::async,[&]{Start(threaded);});
        ChatStartHandoff::Request pending;
        while(!threaded.chat_protocol_signals_->start.TryRequest(pending)) std::this_thread::yield();
        threaded.PollChatStart(now_us);
        assert(receiver.wait_for(std::chrono::seconds(2))==std::future_status::ready);
        receiver.get();
        assert(threaded.chat_playout_response_.response_generation==2 && threaded.cleanups==0);
    }
    {
        Application timeout;Setup(timeout);
        receiver_wait=[&]{now_us=250100;};
        Start(timeout);receiver_wait={};
        timeout.PollChatStart(now_us);
        assert(timeout.cleanups==1 && !timeout.tts_audio_accepting_);
        timeout.PollChatStart(now_us);assert(timeout.cleanups==1);
        now_us=100;
    }
    {
        Application late;Setup(late);
        receiver_wait=[&]{late.PollChatStart(now_us);now_us=250100;};
        Start(late);receiver_wait={};
        assert(late.chat_playout_response_.response_generation==2);
        late.PollChatStart(now_us);assert(late.cleanups==1);
        now_us=100;
    }
    {
        Application exhausted;Setup(exhausted);
        exhausted.chat_playout_stamp_=0;
        exhausted.chat_protocol_signals_->start.serial_=UINT32_MAX-1;
        Start(exhausted);
        exhausted.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        ChatProtocolSignals::Failure failure;
        assert(exhausted.chat_protocol_signals_->ReadFailure(failure));
        assert(failure.source.source_id==1 && (failure.flags & ChatProtocolSignals::Error));
        assert(!exhausted.chat_protocol_signals_->MatchesSource({1,7}));
    }
    {
        Application failed;Setup(failed);failed.chat_playback_fault_=true;
        failed.PollChatPlayout(now_us);assert(failed.cleanups==1);
    }
    {
        Application replacement;Setup(replacement);Stop(replacement);replacement.PollChatPlayout(now_us);
        Barrier old_ack;replacement.protocol_->send_barrier=&old_ack;
        auto sending=std::async(std::launch::async,[&]{replacement.chat_outbound_worker_.RunOnce(now_us);});
        old_ack.Wait();
        auto reservation=replacement.chat_outbound_reservation_;
        auto old_id=replacement.chat_playout_ack_.request_id;
        receiver_wait=[&]{replacement.PollChatStart(now_us);};
        Start(replacement);Start(replacement);Start(replacement);
        receiver_wait={};
        assert(replacement.chat_playout_response_.response_generation==4);
        assert(replacement.chat_start_obsolete_reservation_==reservation);
        assert(replacement.chat_start_obsolete_ack_.request_id==old_id);
        assert(replacement.cleanups==0 && replacement.audio_service_.current);
        old_ack.Release();sending.get();replacement.protocol_->send_barrier=nullptr;
        replacement.PollChatPlayout(now_us);
        replacement.chat_outbound_worker_.RunOnce(now_us);
        replacement.PollChatPlayout(now_us);
        assert(replacement.chat_start_obsolete_reservation_==0);
        assert(replacement.chat_outbound_generation_ && replacement.cleanups==0);
    }
    for (bool error : {true,false}) {
        Application before_stop;Setup(before_stop);
        auto before_callbacks=before_stop.MakeChatSourceCallbacks(1,before_stop.chat_protocol_signals_);
        if(error)before_callbacks.error({1,7},"before stop");else before_callbacks.closed({1,7});
        before_stop.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        assert(before_stop.chat_playout_recovery_ && before_stop.cleanups==1 && !before_stop.audio_service_.current);
        before_stop.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);assert(before_stop.cleanups==1);
    }
    Application audio_before_stop;Setup(audio_before_stop);
    auto first_audio=std::make_unique<AudioStreamPacket>();first_audio->capture_tag.chat_scope=true;
    audio_before_stop.audio_service_.queue.push_back(std::move(first_audio));
    audio_before_stop.protocol_->result=Result::Failed;
    audio_before_stop.chat_outbound_worker_.RunOnce(now_us);
    audio_before_stop.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(audio_before_stop.chat_playout_recovery_ && audio_before_stop.cleanups==1);
    Application normal;Setup(normal);Stop(normal);
    normal.PollChatPlayout(now_us);
    assert(normal.chat_playout_ack_admitted_ && !normal.chat_playout_ready_);
    normal.chat_outbound_worker_.RunOnce(now_us);
    normal.PollChatPlayout(now_us);
    assert(normal.chat_playout_ready_ && normal.cleanups==0);
    assert(normal.chat_playout_stop_.received_us==100 && normal.chat_playout_stop_.continue_listening);
    auto current_callbacks=normal.MakeChatSourceCallbacks(1,normal.chat_protocol_signals_);
    current_callbacks.error({1,7},"current error after ready");
    normal.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(!normal.chat_playout_ready_ && normal.cleanups==1);
    normal.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);assert(normal.cleanups==1);

    Application closed_ack;Setup(closed_ack);Stop(closed_ack);closed_ack.PollChatPlayout(now_us);
    auto closed_callbacks=closed_ack.MakeChatSourceCallbacks(1,closed_ack.chat_protocol_signals_);
    closed_callbacks.closed({1,7});closed_ack.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(closed_ack.chat_playout_recovery_ && closed_ack.cleanups==1);

    Application fresh;Setup(fresh);
    auto stale_callbacks=fresh.MakeChatSourceCallbacks(1,fresh.chat_protocol_signals_);
    stale_callbacks.error({1,7},"old fault");fresh.PollChatProtocolSignals();
    assert(fresh.chat_protocol_fault_);
    fresh.chat_protocol_signals_->Disable();
    assert(fresh.chat_protocol_signals_->EnableForSource({2,7}));
    assert(fresh.EstablishChatPlayoutResponse({{2,7},1,1,1,1}));
    stale_callbacks.closed({1,7});Stop(fresh,"chat:new",false,{2,7});
    fresh.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(fresh.cleanups==0 && fresh.chat_playout_ack_admitted_);

    Application disconnected;Setup(disconnected);Stop(disconnected);
    disconnected.chat_protocol_signals_->Disable();disconnected.PollChatPlayout(now_us);
    assert(disconnected.cleanups==1 && !disconnected.audio_service_.current);

    Application late_audio;Setup(late_audio);Stop(late_audio);late_audio.PollChatPlayout(now_us);
    late_audio.chat_outbound_worker_.RunOnce(now_us);late_audio.PollChatPlayout(now_us);
    assert(late_audio.chat_playout_ready_);
    auto failed_audio=std::make_unique<AudioStreamPacket>();failed_audio->capture_tag.chat_scope=true;
    late_audio.audio_service_.queue.push_back(std::move(failed_audio));late_audio.protocol_->result=Result::Failed;
    late_audio.chat_outbound_worker_.RunOnce(now_us);late_audio.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
    assert(!late_audio.chat_playout_ready_ && late_audio.cleanups==1);

    Application blocked;Setup(blocked);Stop(blocked);
    blocked.PollChatPlayout(now_us);
    Barrier ack;blocked.protocol_->send_barrier=&ack;
    auto sender=std::async(std::launch::async,[&]{blocked.chat_outbound_worker_.RunOnce(now_us);});
    ack.Wait();now_us=100+ConversationPlayoutController::kTimeoutUs;
    blocked.PollChatPlayout(now_us);
    assert(blocked.chat_playout_recovery_ && !blocked.chat_playout_ready_ && blocked.cleanups==1);
    assert(!blocked.audio_service_.current);
    ack.Release();sender.get();blocked.PollChatPlayout(now_us);
    assert(!blocked.chat_playout_ready_);

    now_us=100;
    Application replacing;Setup(replacing);Stop(replacing);replacing.PollChatPlayout(now_us);
    assert(!replacing.EstablishChatPlayoutResponse({{1,7},1,1,1,1}));

    Application fault;Setup(fault);Stop(fault);fault.chat_outbound_fault_=true;
    fault.PollChatPlayout(now_us);assert(fault.chat_playout_recovery_ && !fault.chat_playout_ready_);

    Application expired;Setup(expired);Stop(expired);
    now_us=100+ConversationPlayoutController::kTimeoutUs;expired.PollChatPlayout(now_us);
    assert(expired.chat_playout_recovery_ && expired.audio_service_.snapshot_calls==0);

    now_us=100;
    Application mic;Setup(mic);Stop(mic);
    auto packet=std::make_unique<AudioStreamPacket>();packet->capture_tag.chat_scope=true;
    mic.audio_service_.queue.push_back(std::move(packet));
    Barrier microphone;mic.audio_service_.pop_barrier=&microphone;
    auto microphone_worker=std::async(std::launch::async,[&]{mic.chat_outbound_worker_.RunOnce(now_us);});
    microphone.Wait();mic.PollChatPlayout(now_us);
    now_us=100+ConversationPlayoutController::kTimeoutUs;mic.PollChatPlayout(now_us);
    assert(mic.chat_playout_recovery_ && !mic.chat_playout_ready_);
    microphone.Release();microphone_worker.get();

    now_us=100;
    Application microphone_send;Setup(microphone_send);Stop(microphone_send);
    auto mic_packet=std::make_unique<AudioStreamPacket>();mic_packet->capture_tag.chat_scope=true;
    microphone_send.audio_service_.queue.push_back(std::move(mic_packet));
    Barrier mic_send;microphone_send.protocol_->send_barrier=&mic_send;
    auto mic_sender=std::async(std::launch::async,[&]{microphone_send.chat_outbound_worker_.RunOnce(now_us);});
    mic_send.Wait();microphone_send.PollChatPlayout(now_us);
    now_us=100+ConversationPlayoutController::kTimeoutUs;microphone_send.PollChatPlayout(now_us);
    assert(microphone_send.chat_playout_recovery_ && !microphone_send.chat_playout_ready_);
    mic_send.Release();mic_sender.get();

    now_us=100;
    Application snapshot_busy;Setup(snapshot_busy);snapshot_busy.audio_service_.reset_busy=true;
    Stop(snapshot_busy);snapshot_busy.audio_service_.reset_busy=false;
    snapshot_busy.PollChatPlayout(now_us);
    assert(snapshot_busy.chat_playout_recovery_ && snapshot_busy.protocol_->sends.empty());

    Application locked;Setup(locked);Stop(locked);
    std::promise<void> held,release;auto released=release.get_future().share();
    auto holder=std::async(std::launch::async,[&]{std::lock_guard<std::mutex> lock(locked.chat_protocol_signals_->intake.mutex_);held.set_value();released.wait();});
    held.get_future().wait();locked.PollChatPlayout(now_us);
    assert(locked.chat_playout_recovery_);release.set_value();holder.get();

    Application duplicate;Setup(duplicate);Stop(duplicate);
    now_us=9999999;Stop(duplicate);duplicate.audio_service_.snapshot_busy=true;
    duplicate.PollChatPlayout(now_us);
    now_us=100+ConversationPlayoutController::kTimeoutUs;duplicate.PollChatPlayout(now_us);
    assert(duplicate.chat_playout_recovery_ && duplicate.chat_playout_stop_.received_us==100);

    now_us=100;
    Application interrupted;Setup(interrupted);Stop(interrupted);interrupted.PollChatPlayout(now_us);
    Stop(interrupted,"",true);interrupted.PollChatPlayout(now_us);
    assert(interrupted.chat_playout_recovery_ && !interrupted.chat_playout_ready_);
    assert(interrupted.chat_playout_stop_.interrupt && interrupted.chat_playout_stop_.explicit_manual_stop);

    Application invalid;Setup(invalid);Stop(invalid,"chat:"+std::string(124,'x'));invalid.PollChatPlayout(now_us);
    assert(invalid.chat_playout_recovery_);
    Application maximum;Setup(maximum);Stop(maximum,"chat:"+std::string(123,'x'));maximum.PollChatPlayout(now_us);
    assert(maximum.chat_playout_ack_.payload_size==128);

    Application old;Setup(old);Stop(old);++old.connect_generation_;old.PollChatPlayout(now_us);
    assert(old.cleanups==1 && !old.chat_playout_ready_ && !old.audio_service_.current);
    Application successor;Setup(successor);Stop(successor);
    ++successor.connect_generation_;++successor.speaking_generation_;successor.PollChatPlayout(now_us);
    assert(successor.cleanups==0 && !successor.chat_playout_ready_);

    Application retired;Setup(retired);Stop(retired);retired.PollChatPlayout(now_us);
    assert(!retired.EstablishChatPlayoutResponse({{1,7},1,1,1,1}));
    retired.RetireChatOutbound();
    retired.chat_outbound_worker_.RunOnce(now_us);retired.PollChatOutbound();
    assert(retired.chat_outbound_reservation_==0);
    assert(retired.EstablishChatPlayoutResponse({{1,7},1,1,1,1}));
}
