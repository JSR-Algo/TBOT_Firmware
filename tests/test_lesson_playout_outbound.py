from pathlib import Path
import subprocess

import pytest
from test_chat_playout_intake import run_terminal_application


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("during_send", [False, True])
def test_actual_full_text_worker_rejects_revoked_response(tmp_path, during_send):
    prefix = (ROOT / "tests/native/chat_outbound_worker_test.cc").read_text().split("using TaskHandle_t")[0]
    body = r'''
struct FullProtocol : TestProtocol {
    std::string wire;
    Result SendChatFullTextIfCurrent(const Job& job,const std::function<bool()>& authorize) override {
        if (!authorize()) return Result::Stale;
        if(send_barrier)send_barrier->Block();
        if (!authorize()) return Result::Stale;
        wire+=*job.full_text;
        return Result::Sent;
    }
};
int main() {
    FullProtocol protocol;ChatOutboundWorker worker;
    ChatOutboundWorker::Activation activation;
    activation.protocol=&protocol;activation.protocol_generation=1;activation.connection_epoch=7;
    activation.generation=worker.AdvanceGeneration();
    activation.pop=[](void*){return std::unique_ptr<AudioStreamPacket>();};
    activation.current_audio=[](void*,const AudioStreamPacket&){return false;};
    activation.notify=[](void*){};
    activation.current_connection=[](void*,ConnectionSource,uint64_t,uint32_t){return true;};
    assert(worker.Publish(activation));
    auto authorization=std::make_shared<std::atomic<bool>>(true);
    Job job;job.kind=Kind::FullText;job.request_id=1;job.generation=activation.generation;
    job.protocol_generation=1;job.connection_epoch=7;job.connect_generation=1;job.source={1,7};
    job.deadline_us=10000100;job.full_text=std::make_shared<const std::string>("obsolete receipt");
    // AUTHORIZATION_ASSIGNMENT
    assert(worker.Submit(job));
    Barrier barrier;
    if(DURING_SEND) {
        protocol.send_barrier=&barrier;
        auto sending=std::async(std::launch::async,[&]{worker.RunOnce(100);});barrier.Wait();
        authorization->store(false);barrier.Release();sending.get();
    } else { authorization->store(false);worker.RunOnce(100); }
    Completion completion;assert(worker.Collect(completion));
    assert(protocol.wire.empty() && completion.result==Result::Stale);
}
'''.replace("DURING_SEND", "true" if during_send else "false")
    if "std::shared_ptr<std::atomic<bool>> authorization;" in (ROOT / "main/chat_outbound_mailbox.h").read_text():
        body = body.replace("// AUTHORIZATION_ASSIGNMENT", "job.authorization=authorization;")
    generated = tmp_path / "worker.cc"
    generated.write_text(prefix + body)
    binary = tmp_path / "worker"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/native_stubs"),
                    "-I", str(Path.home() / "esp/esp-idf/components/json/cJSON"),
                    str(generated), str(ROOT / "main/chat_outbound_worker.cc"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_actual_application_connection_queue_propagates_response_authorization(tmp_path):
    run_terminal_application(tmp_path, "address", 0, extra_tests=r'''
    {
        Application scoped;Setup(scoped);
        auto token=std::make_shared<std::atomic<bool>>(true);
        unsigned emitted=0;
        scoped.protocol_->full_text_send=[&](const Job&,const std::function<bool()>& authorize) {
            if(!authorize())return Result::Stale;
            ++emitted;return Result::Sent;
        };
        assert(scoped.RequestChatConnectionText("playout start",{},0,token));
        assert(scoped.RequestChatConnectionText("playout stop",{},0,token));
        scoped.PollChatConnectionMessages(now_us);
        auto* record=scoped.chat_connection_messages_.Front();
        assert(record && record->authorization==token && record->physical.authorization==token);
        assert(record->submitted);
        token->store(false);
        scoped.chat_outbound_worker_.RunOnce(now_us);
        scoped.PollChatOutbound();scoped.PollChatConnectionMessages(now_us);
        scoped.PollChatConnectionMessages(now_us);
        assert(emitted==0);
        assert(scoped.chat_connection_messages_.Size()==0);
    }
    {
        Application failed;Setup(failed);
        auto token=std::make_shared<std::atomic<bool>>(true);
        failed.protocol_->full_text_send=[](const Job&,const std::function<bool()>& authorize) {
            assert(authorize());return Result::Failed;
        };
        assert(failed.RequestChatConnectionText("playout start",{},0,token));
        failed.PollChatConnectionMessages(now_us);failed.chat_outbound_worker_.RunOnce(now_us);
        failed.PollChatOutbound();assert(!token->load());
        failed.PollChatConnectionMessages(now_us);
        ChatProtocolSignals::Failure failure;
        assert(failed.chat_protocol_signals_->ReadFailure(failure));
        assert(failure.source.source_id==1 && (failure.flags & ChatProtocolSignals::Error));
    }
    {
        Application expired;Setup(expired);
        auto token=std::make_shared<std::atomic<bool>>(true);
        assert(expired.RequestChatConnectionText("playout start",{},100,token));
        expired.PollChatConnectionMessages(10000100);assert(!token->load());
        assert(expired.chat_connection_messages_.Size()==0);
        ChatProtocolSignals::Failure failure;
        assert(expired.chat_protocol_signals_->ReadFailure(failure));
    }
''')
