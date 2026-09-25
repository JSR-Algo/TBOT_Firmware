from pathlib import Path
import os
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_OUTBOUND_TSAN") == "1" else []))
def test_connection_messages(tmp_path, sanitize):
    binary = tmp_path / "connection"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
                    "-I", str(ROOT / "main"), str(ROOT / "tests/native/chat_connection_messages_test.cc"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)

def test_actual_full_text_worker(tmp_path):
    prefix=(ROOT / "tests/native/chat_outbound_worker_test.cc").read_text().split("using TaskHandle_t")[0]
    body=r'''
struct FullProtocol : TestProtocol {
    std::string wire;
    Result SendChatFullTextIfCurrent(const Job& job,const std::function<bool()>& authorize) override {
        if (!authorize()) return Result::Stale;
        if(send_barrier)send_barrier->Block();
        if (!authorize()) return Result::Stale;
        wire+=*job.full_text;
        return result;
    }
};
int main(){
    FullProtocol protocol;ChatOutboundWorker worker;
    bool source_current=true;
    ChatOutboundWorker::Activation activation;
    activation.protocol=&protocol;activation.protocol_generation=1;activation.connection_epoch=7;
    activation.generation=worker.AdvanceGeneration();activation.context=&source_current;
    activation.pop=[](void*){return std::unique_ptr<AudioStreamPacket>();};
    activation.current_audio=[](void*,const AudioStreamPacket&){return false;};
    activation.notify=[](void*){};
    activation.current_connection=[](void* p,ConnectionSource,uint64_t,uint32_t){return *static_cast<bool*>(p);};
    assert(worker.Publish(activation));
    Job job;job.kind=Kind::FullText;job.request_id=1;job.generation=activation.generation;
    job.protocol_generation=1;job.connection_epoch=7;job.connect_generation=1;job.source={1,7};
    job.deadline_us=10000100;job.full_text=std::make_shared<const std::string>(2500,'x');
    assert(worker.Submit(job));
    Barrier barrier;protocol.send_barrier=&barrier;
    auto sending=std::async(std::launch::async,[&]{worker.RunOnce(100);});barrier.Wait();
    worker.AdvanceGeneration();barrier.Release();sending.get();
    Completion completion;assert(worker.Collect(completion));
    assert(completion.stale && completion.result==Result::Sent && protocol.wire.size()==2500);
    worker.RunOnce(100);assert(worker.TakeRetired());
    activation.generation=worker.AdvanceGeneration();assert(worker.Publish(activation));
    job.generation=activation.generation;job.request_id=2;assert(worker.Submit(job));
    worker.AdvanceGeneration();worker.RunOnce(100);
    assert(!worker.Collect(completion) && worker.TakeRetired());
    assert(protocol.wire.size()==2500);
}
'''
    generated=tmp_path / "worker.cc";generated.write_text(prefix+body)
    binary=tmp_path / "worker"
    subprocess.run(["c++","-std=c++17","-pthread","-fsanitize=address,undefined","-I",str(ROOT / "main"),
                    "-I",str(ROOT / "tests/native_stubs"),"-I",str(Path.home() / "esp/esp-idf/components/json/cJSON"),
                    str(generated),str(ROOT / "main/chat_outbound_worker.cc"),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
