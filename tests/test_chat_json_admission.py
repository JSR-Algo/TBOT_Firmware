from pathlib import Path
import os
import subprocess
import pytest
from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]
CJSON = Path.home() / "esp/esp-idf/components/json/cJSON"


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_json_admission(tmp_path, sanitize):
    source = (ROOT / "main/application.cc").read_text()
    fixture = (ROOT / "tests/native/chat_json_admission_test.cc").read_text()
    names = ["void Application::PollChatInboundMessages", "bool Application::IsChatConnectionCurrent",
             "bool Application::IsChatRequestCurrent", "void Application::FailChatRequest",
             "Protocol::SourceCallbacks Application::MakeChatSourceCallbacks"]
    generated = tmp_path / "admission.cc"
    generated.write_text(fixture.replace("// PRODUCTION_METHODS", "\n".join(method(source, name) for name in names)))
    flags = [f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}", "-fno-omit-frame-pointer"]
    obj = tmp_path / "cjson.o"
    subprocess.run(["cc", *flags, "-Wno-deprecated-declarations", "-I", str(CJSON), "-c", str(CJSON / "cJSON.c"), "-o", str(obj)], check=True)
    binary = tmp_path / "admission"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "main"), "-I", str(CJSON), str(generated), str(obj), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_application_progress_with_receiver_lease(tmp_path, sanitize):
    from test_chat_playout_intake import run_terminal_application
    run_terminal_application(tmp_path, sanitize, 0, r'''
    {
        now_us=100;
        Application app;Setup(app);
        ConnectionInboundGate gate;
        const auto epoch=gate.BeginConnection();
        auto* root=cJSON_Parse("{\"type\":\"mcp\",\"text\":\"held-gate\"}");
        for(int i=0;i<4;++i)
            assert(app.chat_inbound_messages_.Admit(root,{{1,7},1,1},100,88,"session"));
        ChatRequestContext async_tool;
        unsigned dispatched=0;
        app.json_dispatch=[&](ChatRequestContext context){
            ++dispatched;
            if(!async_tool)async_tool=std::move(context);
        };
        assert(app.RequestChatConnectionText("stalled-reply"));
        app.PollChatConnectionMessages(now_us);
        Barrier transport;
        app.protocol_->send_barrier=&transport;
        auto sending=std::async(std::launch::async,[&]{app.chat_outbound_worker_.RunOnce(now_us);});
        transport.Wait();
        Barrier receiver_paused;
        receiver_wait=[&]{
            assert(gate.CurrentThreadHasLease());
            receiver_paused.Block();
        };
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        auto receiving=std::async(std::launch::async,[&]{
            auto lease=gate.Acquire(epoch);assert(lease);
            callbacks.json({1,7},root,88,{100,0});
        });
        receiver_paused.Wait();
        assert(gate.TryAcquire(epoch).status()==ConnectionInboundGate::LeaseStatus::Busy);
        app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        assert(dispatched==4 && async_tool && app.chat_inbound_messages_.Outstanding()==1);
        assert(sending.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
        receiver_paused.Release();
        assert(receiving.wait_for(std::chrono::seconds(2))==std::future_status::ready);
        receiving.get();receiver_wait={};
        assert(app.chat_protocol_signals_->MatchesSource({1,7}));
        assert(app.chat_inbound_messages_.Outstanding()==2);
        app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        assert(dispatched==5 && app.chat_inbound_messages_.Outstanding()==1);
        assert(sending.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
        async_tool.reset();assert(!app.chat_inbound_messages_.Outstanding());
        transport.Release();sending.get();app.protocol_->send_barrier=nullptr;
        app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        cJSON_Delete(root);
    }
''')


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_same_gate_selected_transport_retry(tmp_path, sanitize):
    from test_chat_playout_intake import run_terminal_application
    transport = (ROOT / "main/protocols/websocket_protocol.cc").read_text()
    definitions = r'''
namespace Lang {namespace Strings {const char* SERVER_ERROR="error";}}
namespace json_transport {
struct WebsocketProtocol {
    ConnectionInboundGate inbound_gate_;
    uint32_t websocket_connection_epoch_=0;
    ConnectionSource websocket_source_;
    struct Socket {
        ConnectionInboundGate& gate;
        unsigned writes=0;
        bool Send(const std::string&) {assert(gate.CurrentThreadHasLease());++writes;return true;}
    } socket{inbound_gate_};
    Socket* websocket_=&socket;
    struct {void OnPingSent(uint32_t) {}} passive_liveness_;
    bool IsAudioChannelOpened() const {return true;}
    void SetError(const std::string&,ConnectionSource) {assert(false);}
    Result SendChatFullTextIfCurrent(const Job&,const std::function<bool()>&);
    bool SendTextForSource(const std::string&,ConnectionSource);
    WebsocketProtocol() {
        for(int i=0;i<7;++i)websocket_connection_epoch_=inbound_gate_.BeginConnection();
        websocket_source_={1,websocket_connection_epoch_};
    }
};
'''
    definitions += method(transport, "ChatOutboundMailbox::Result WebsocketProtocol::SendChatFullTextIfCurrent")
    definitions += method(transport, "bool WebsocketProtocol::SendTextForSource") + "\n}\n"
    run_terminal_application(tmp_path, sanitize, 0, r'''
    {
        now_us=100;Application app;Setup(app);
        json_transport::WebsocketProtocol transport;
        app.protocol_->full_text_send=[&](const Job& job,const std::function<bool()>& current){
            return transport.SendChatFullTextIfCurrent(job,current);
        };
        auto* root=cJSON_Parse("{\"type\":\"mcp\",\"text\":\"same-gate\"}");
        for(int i=0;i<4;++i)assert(app.chat_inbound_messages_.Admit(root,{{1,7},1,1},100,88));
        assert(app.RequestChatConnectionText("reply"));app.PollChatConnectionMessages(now_us);
        Barrier waiting;receiver_wait=[&]{assert(transport.inbound_gate_.CurrentThreadHasLease());waiting.Block();};
        auto callbacks=app.MakeChatSourceCallbacks(1,app.chat_protocol_signals_);
        auto receiver=std::async(std::launch::async,[&]{
            auto lease=transport.inbound_gate_.Acquire(7);assert(lease);
            callbacks.json({1,7},root,88,{100,0});
        });
        waiting.Wait();
        assert(app.chat_outbound_worker_.RunOnce(now_us));
        assert(!transport.socket.writes);
        Completion completion;assert(!app.chat_outbound_worker_.Collect(completion));
        app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        assert(!app.chat_inbound_messages_.Outstanding());
        waiting.Release();receiver.get();receiver_wait={};
        assert(app.chat_protocol_signals_->MatchesSource({1,7}));
        assert(app.chat_outbound_worker_.RunOnce(now_us));
        assert(transport.socket.writes==1);
        app.PollChatOutboundEvents(MAIN_EVENT_CHAT_OUTBOUND);
        assert(!app.chat_inbound_messages_.Outstanding() && !app.chat_connection_messages_.Size());
        cJSON_Delete(root);
    }
''', definitions)
