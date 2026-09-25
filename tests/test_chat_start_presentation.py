"""Exercise START admission while ordinary JSON presentation occupies the app."""
from test_chat_playout_intake import run_terminal_application
import pytest


@pytest.mark.parametrize("arrival", [1, 4])
def test_start_arriving_during_json_batch_is_admitted_before_next_render(tmp_path, arrival):
    run_terminal_application(tmp_path, "address", 0, r'''
    {
        now_us=100;
        Application app;Setup(app);app.state=kDeviceStateListening;
        auto* root=cJSON_Parse("{\"type\":\"stt\",\"text\":\"synthetic\"}");
        for(int i=0;i<4;++i)
            assert(app.chat_inbound_messages_.Admit(root,{{1,7},1,1},100,88));
        ChatStartHandoff::Request start{{1,7},1,1,0,100,0};
        unsigned rendered=0;
        app.json_dispatch=[&](ChatRequestContext){
            if(++rendered==ARRIVAL) {
                start.received_us=now_us;
                assert(app.chat_protocol_signals_->start.Publish(start));
            } else if(rendered==ARRIVAL+1) {
                ChatStartHandoff::Admission admission;
                assert(app.chat_protocol_signals_->start.TryAdmission(start,now_us,admission));
                assert(app.chat_protocol_signals_->start.Confirm(start,now_us));
            }
            now_us+=126000;
        };
        app.PollChatInboundMessages();
        assert(rendered==4);
        if(!app.chat_protocol_signals_->start.Confirmed(start)) {
            ChatStartHandoff::Admission admission;
            assert(app.chat_protocol_signals_->start.TryAdmission(start,now_us,admission));
            assert(app.chat_protocol_signals_->start.Confirm(start,now_us));
        }
        assert(app.chat_protocol_signals_->start.Confirmed(start));
        assert(app.chat_start_failed_serial_!=start.serial);
        assert(!app.chat_inbound_messages_.Outstanding());
        cJSON_Delete(root);
    }
'''.replace("ARRIVAL", str(arrival)))


def test_rearm_polling_does_not_restart_the_same_face(tmp_path):
    def count_rendering(fixture, source, header):
        fixture = "unsigned face_renders=0;\n" + fixture
        return fixture.replace("void SetEmotion(const char*) {}",
                               "void SetEmotion(const char*) { ++face_renders; }")

    run_terminal_application(tmp_path, "address", 0, r'''
    {
        now_us=100;Application app;Setup(app);
        app.state=kDeviceStateListening;
        app.chat_rearm_phase_=Application::ChatRearmPhase::Armed;
        const auto before=face_renders;
        for(int i=0;i<100;++i)app.RenderChatRearm();
        assert(face_renders==before+1);
        app.chat_rearm_phase_=Application::ChatRearmPhase::Pending;
        app.RenderChatRearm();
        app.chat_rearm_phase_=Application::ChatRearmPhase::Armed;
        app.RenderChatRearm();
        assert(face_renders==before+2);
    }
''', fixture_transform=count_rendering)


def test_start_renders_speaking_after_listening_and_admission_poll(tmp_path):
    run_terminal_application(tmp_path, "address", 0, r'''
    {
        now_us=100;Application app;Setup(app);app.state=kDeviceStateListening;
        app.chat_rearm_phase_=Application::ChatRearmPhase::Armed;
        app.RenderChatRearm();
        assert(Board::GetInstance().status=="listening");
        receiver_wait=[&]{
            app.PollChatStart(now_us);
            // Admission can render None before the receiver confirms START.
            app.RenderChatRearm();
        };
        Start(app);receiver_wait={};app.PollChatStart(now_us);
        assert(app.state==kDeviceStateSpeaking);
        assert(app.AdvanceChatRearm(now_us));
        app.RenderChatRearm();
        assert(Board::GetInstance().status=="speaking");
        app.chat_rearm_phase_=Application::ChatRearmPhase::Pending;
        app.RenderChatRearm();assert(Board::GetInstance().status=="wait");
        app.chat_rearm_phase_=Application::ChatRearmPhase::Armed;
        app.state=kDeviceStateListening;app.RenderChatRearm();
        assert(Board::GetInstance().status=="listening");
        app.chat_rearm_owner_.source={9,9};app.state=kDeviceStateSpeaking;
        app.chat_rearm_phase_=Application::ChatRearmPhase::None;
        app.RenderChatRearm();assert(Board::GetInstance().status=="listening");
    }
''')
