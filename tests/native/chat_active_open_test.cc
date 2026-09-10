void TestActiveOpenGreeting() {
    for (bool expire : {false, true}) {
        now_us=100;transport_fixture::now_us=100;
        Application app;
        app.InitializeChatOutboundWorker();
        app.exercise_source_failure=true;
        app.chat_cleanup_enabled_=true;
        app.state=kDeviceStateConnecting;app.connect_in_flight_=true;
        app.reconnect_resume_listening_=true;
        const auto open_reservation=app.protocol_work_lifetime_.Reserve();assert(open_reservation);
        transport_fixture::WebsocketProtocol socket_protocol;
        auto candidate=std::make_unique<transport_fixture::WebSocket>();
        auto* socket=candidate.get();
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        socket_protocol.source_callbacks_.opened=callbacks.opened;
        socket_protocol.source_callbacks_.adopted=callbacks.adopted;
        socket_protocol.source_callbacks_.error=callbacks.error;
        unsigned delivered=0,waits=0;
        socket_protocol.source_callbacks_.json=[&](ConnectionSource source,const cJSON* root,uint64_t epoch,ConnectionReceipt receipt) {
            ++delivered;
            assert(app.state==kDeviceStateListening && app.microphone_uplink_authorized_);
            assert(app.chat_rearm_delivery_ && app.chat_rearm_delivery_->result==Result::Sent);
            assert(receipt.received_us==100 && receipt.admission_deadline_us==250100);
            callbacks.json(source,root,epoch,receipt);
        };
        auto source=socket_protocol.Attach(socket,99);
        app.protocol_->healthy_epoch=source.connection_epoch;
        const char* hello="{\"type\":\"hello\"}";
        socket->data(hello,strlen(hello),false);
        transport_fixture::publication_wait=[&] {
            assert(socket_protocol.inbound_gate_.TryAcquire(source.connection_epoch));
            ++waits;
            if(waits==1) {
                assert(socket_protocol.Publish(std::move(candidate)));
                app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
                assert(app.chat_protocol_signals_->MatchesSource(source));
                assert(!callbacks.adopted(source));
                app.audio_service_.reset_busy=true;
                CompleteActiveOpen(app,open_reservation);
                assert(!app.connect_in_flight_ && app.watchdog_cancels==1);
            }
            app.HandleStateChangedEvent();
            if(waits==2) {
                app.protocol_->result=Result::Busy;
                app.chat_outbound_worker_.RunOnce(now_us);
                app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
                app.HandleStateChangedEvent();
                assert(!callbacks.adopted(source) && !app.microphone_uplink_authorized_);
            }
            if(waits==3) {
                app.protocol_->result=Result::Sent;
                Barrier network;
                app.protocol_->send_barrier=&network;
                auto sending=std::async(std::launch::async,[&]{app.chat_outbound_worker_.RunOnce(now_us);});
                network.Wait();
                app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
                app.HandleStateChangedEvent();
                assert(!callbacks.adopted(source) && !app.microphone_uplink_authorized_);
                network.Release();sending.get();app.protocol_->send_barrier=nullptr;
                app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
                app.HandleStateChangedEvent();
                assert(!callbacks.adopted(source) && !app.microphone_uplink_authorized_);
            }
            if(waits==4) {
                if(expire) { now_us=250100;transport_fixture::now_us=250100;return; }
                app.chat_audio_prepared_=app.chat_rearm_prepared_;
                app.audio_service_.reset_busy=false;
                app.chat_audio_reset_completed_=app.audio_service_.reset_token;
                app.HandleStateChangedEvent();
                assert(callbacks.adopted(source) && app.state==kDeviceStateListening);
            }
            assert(waits<=4);
        };
        receiver_wait=[&] { app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND); };
        const char* greeting="{\"type\":\"tts\",\"state\":\"start\"}";
        socket->data(greeting,strlen(greeting),false);
        transport_fixture::publication_wait={};receiver_wait={};
        assert(waits==4 && delivered==(expire ? 0U : 1U));
        if(expire) {
            assert(!callbacks.adopted(source));
            ChatProtocolSignals::Failure failure;
            assert(app.chat_protocol_signals_->ReadFailure(failure));
            assert(failure.source.source_id==source.source_id && failure.flags & ChatProtocolSignals::Error);
            app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
            assert(app.state==kDeviceStateIdle && app.backend_offline_ && !app.microphone_uplink_authorized_);
        } else {
            app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
            assert(app.state==kDeviceStateSpeaking && app.speaking_arm_dispatch_.begins==1);
        }
    }
}
