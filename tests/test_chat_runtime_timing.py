from pathlib import Path
import os
import re
import subprocess
import pytest
from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]
CJSON = Path.home() / "esp/esp-idf/components/json/cJSON"


def test_all_recovery_origins_are_fixed_and_unique():
    source = (ROOT / "main/application.cc").read_text()
    start = re.findall(r"(?<!::)RecoverChatStart\(([^;]*?)\);", source)
    playout = re.findall(r"(?<!::)RecoverChatPlayout\(([^;]*?)\);", source)
    assert start and playout
    assert all(re.fullmatch(r"request, 10[1-5]", args) for args in start), start
    assert len(start) == len(set(start)) == 5
    assert all(re.fullmatch(r"2\d\d", args) for args in playout), playout
    assert len(playout) == len(set(playout))
    for name, guard, mutation in [
        ("void Application::RecoverChatStart", "chat_start_failed_serial_ == request.serial) return;", "chat_start_failed_serial_ = request.serial;"),
        ("void Application::RecoverChatPlayout", "if (chat_playout_recovery_) return;", "chat_rearm_voice_intent_ = false;"),
    ]:
        body = method(source, name)
        assert body.index(guard) < body.index('"chat_recovery site=%u"') < body.index(mutation)


def test_scope_placements_and_private_fields():
    app = (ROOT / "main/application.cc").read_text()
    lcd = (ROOT / "main/display/lcd_display.cc").read_text()
    for source, signature, site in [
        (app, "void Application::DispatchIncomingJson", 1),
        (app, "void Application::HandleStateChangedEvent", 2),
        (lcd, "void LcdDisplay::SetEmotion", 3),
    ]:
        body = method(source, signature)
        assert re.search(r"\{\s*(?://[^\n]*\n\s*)?ChatRuntimeTiming timing\(" + str(site), body)
        assert '"chat_slow_scope site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu"' in body
        if site == 3:
            assert body.index("ChatRuntimeTiming timing") < body.index("DisplayLockGuard")
    receiver = method(app, "void Application::HandleChatStart")
    assert '"chat_start_receiver_end site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu"' in receiver
    assert "termination_site = 301" in receiver and "termination_site = 302" in receiver
    callbacks = method(app, "Protocol::SourceCallbacks Application::MakeChatSourceCallbacks")
    assert callbacks.count("chat_inbound_messages_.TrySnapshot()") == 3
    assert callbacks.count('"chat_json_queue available=%u queued=%u outstanding=%u"') == 3
    for line in (app + lcd).splitlines():
        if any(marker in line for marker in ('"chat_slow_scope', '"chat_recovery', '"chat_start_receiver_end', '"chat_json_queue')):
            assert "%s" not in line and "%ll" not in line


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_runtime_timing_and_snapshot(tmp_path, sanitize):
    assert (ROOT / "main/chat_runtime_timing.h").exists(), "Missing scoped runtime timing helper"
    assert "TrySnapshot()" in (ROOT / "main/chat_inbound_messages.h").read_text(), "Missing nonblocking queue snapshot"
    flags = [f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}", "-fno-omit-frame-pointer"]
    obj = tmp_path / "cjson.o"
    subprocess.run(["cc", *flags, "-Wno-deprecated-declarations", "-I", str(CJSON), "-c", str(CJSON / "cJSON.c"), "-o", str(obj)], check=True)
    binary = tmp_path / "timing"
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "main"), "-I", str(CJSON), str(ROOT / "tests/native/chat_runtime_timing_test.cc"),
                    str(obj), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_recovery_and_receiver_logs(tmp_path, sanitize):
    from test_chat_playout_intake import run_terminal_application
    run_terminal_application(tmp_path, sanitize, 0, r'''
    {
        auto count = [](const char* marker) {
            std::lock_guard<std::mutex> lock(runtime_log_mutex);
            return std::count_if(runtime_logs.begin(), runtime_logs.end(),
                [&](const std::string& line) { return line.find(marker) != std::string::npos; });
        };
        now_us=100;
        Application timeout; Setup(timeout);
        runtime_logs.clear();
        receiver_wait=[&] { now_us=250100; };
        Start(timeout); receiver_wait={};
        assert(count("chat_start_receiver_end site=301 elapsed_us_hi=0 elapsed_us_lo=250000")==1);
        assert(count("chat_recovery")==0);
        timeout.PollChatStart(now_us);
        assert(count("chat_recovery site=101")==1);
        for(int i=0;i<5;++i) timeout.PollChatStart(now_us);
        assert(count("chat_recovery")==1);
        ChatStartHandoff::Request stale; assert(timeout.chat_protocol_signals_->start.TryRequest(stale));
        ++stale.connect_generation;
        timeout.RecoverChatStart(stale,102);
        assert(count("chat_recovery")==1);

        now_us=100;
        Application cancelled; Setup(cancelled);
        runtime_logs.clear();
        receiver_wait=[&] { ++cancelled.connect_generation_; };
        Start(cancelled); receiver_wait={};
        assert(count("chat_start_receiver_end site=302 elapsed_us_hi=0 elapsed_us_lo=0")==1);
        cancelled.PollChatStart(now_us);
        assert(count("chat_recovery")==0);

        Application fault; Setup(fault); fault.chat_outbound_fault_=true;
        runtime_logs.clear();
        fault.PollChatPlayout(now_us);
        assert(count("chat_recovery site=207")==1);
        for(int i=0;i<5;++i) { fault.PollChatPlayout(now_us); fault.RecoverChatPlayout(208); }
        assert(count("chat_recovery")==1);

        Application queue; Setup(queue);
        auto* root=cJSON_Parse("{\"type\":\"mcp\",\"text\":\"private\"}");
        for(int i=0;i<4;++i) assert(queue.chat_inbound_messages_.Admit(root,{{1,7},1,1},100,0,"private-session"));
        runtime_logs.clear();
        receiver_wait=[&] { ++now_us; };
        auto callbacks=queue.MakeChatSourceCallbacks(1,queue.chat_protocol_signals_);
        callbacks.json({1,7},root,0,{100,102}); receiver_wait={};
        assert(count("chat_json_queue available=1 queued=4 outstanding=4")==1);
        assert(count("chat_source_fault reason=json_admission")==1);
        assert(runtime_logs.size()==2 && runtime_logs[0].find("chat_json_queue")==0);
        assert(count("private")==0);
        cJSON_Delete(root);
    }
''')


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_actual_dispatch_entry_scope(tmp_path, sanitize):
    source = (ROOT / "main/application.cc").read_text()
    entry = method(source, "void Application::DispatchIncomingJson")
    # Exercise the exact production entry, stopping before the broad JSON router.
    entry = entry[:entry.index("    auto* display =")] + "\n}\n"
    generated = tmp_path / "dispatch.cc"
    generated.write_text(r'''
#include "chat_runtime_timing.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
struct cJSON {};
using ChatRequestContext = int;
uint64_t now_us=100;
uint64_t esp_timer_get_time() { return now_us; }
constexpr const char* TAG="test";
std::vector<std::string> logs;
void Log(const char*,const char* format,...) {
    char line[256];va_list args;va_start(args,format);
    vsnprintf(line,sizeof(line),format,args);va_end(args);logs.emplace_back(line);
}
#define ESP_LOGW(...) Log(__VA_ARGS__)
struct Application {
    uint64_t cost=0;
    bool throws=false;
    bool IsChatRequestCurrent(ChatRequestContext current) {
        now_us+=cost;
        if(throws)throw std::runtime_error("unwind");
        return current!=0;
    }
    void DispatchIncomingJson(const cJSON*,uint64_t,bool,ChatRequestContext);
};
''' + entry + r'''
int main() {
    Application app;
    app.cost=49999;app.DispatchIncomingJson(nullptr,0,true,0);assert(logs.empty());
    app.cost=50000;app.DispatchIncomingJson(nullptr,0,true,0);
    assert(logs.size()==1 && logs.back()=="chat_slow_scope site=1 elapsed_us_hi=0 elapsed_us_lo=50000");
    app.throws=true;
    try { app.DispatchIncomingJson(nullptr,0,true,1); } catch(const std::runtime_error&) {}
    assert(logs.size()==2 && logs.back()==logs.front());
}
''')
    binary = tmp_path / "dispatch"
    flags = [f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}"]
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", *flags,
                    "-I", str(ROOT / "main"), str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_no_resume_idle_is_diagnosed_once(tmp_path):
    from test_chat_playout_intake import run_terminal_application
    run_terminal_application(tmp_path, "address", 0, r'''
    {
        now_us=100;
        Application complete; ReadyForRearm(complete);
        complete.chat_playout_stop_.explicit_manual_stop=true;
        runtime_logs.clear();
        complete.HandleStateChangedEvent();
        assert(complete.state==kDeviceStateIdle);
        assert(runtime_logs.size()==1);
        assert(runtime_logs.front()=="chat_rearm_idle site=401 owned=1 manual=1 continuation=1");
        complete.HandleStateChangedEvent();
        assert(runtime_logs.size()==1);
    }
''')
