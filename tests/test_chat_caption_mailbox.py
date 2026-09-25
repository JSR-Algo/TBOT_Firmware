"""Bounded caption ownership, UTF-8 truncation, and concurrent UI/RX progress."""
import os
import subprocess

import pytest

from test_chat_json_admission import ROOT
from test_protocol_work_lifetime import method


@pytest.mark.parametrize("sanitize", ["address"] + (["thread"] if os.environ.get("CHAT_APPLICATION_TSAN") == "1" else []))
def test_latest_caption_and_ui_timer(tmp_path, sanitize):
    lcd = (ROOT / "main/display/lcd_display.cc").read_text()
    fixture = r'''
#include "chat_caption_mailbox.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <string>
#include <thread>
#define ESP_LOGW(...) ((void)0)
struct Application {
    ChatCaptionMailbox captions;
    static Application& GetInstance() {static Application app;return app;}
    bool TakeChatCaption(ChatCaptionMailbox::Message& message) {return captions.TryTake(message);}
};
struct lv_timer_t {void (*callback)(lv_timer_t*);void* data;};
bool fail_timer=false;
lv_timer_t* lv_timer_create(void (*callback)(lv_timer_t*),unsigned period,void* data) {
    assert(period==100);return fail_timer ? nullptr : new lv_timer_t{callback,data};
}
void* lv_timer_get_user_data(lv_timer_t* timer) {return timer->data;}
struct LcdDisplay {
    std::atomic<bool> lesson_mode_active_{false};
    lv_timer_t* chat_caption_timer_=nullptr;
    std::string rendered,role;
    std::function<void()> drawing;
    void StartChatCaptionTimer();
    void SetChatMessage(const char* who,const char* text) {
        if(drawing)drawing();role=who;rendered=text;
    }
    void Tick() {chat_caption_timer_->callback(chat_caption_timer_);}
    ~LcdDisplay() {delete chat_caption_timer_;}
};
// PRODUCTION_TIMER
int main() {
    auto& mailbox=Application::GetInstance().captions;
    const ChatPlayoutIntake::Response owner{{1,7},1,1,2,3};
    ChatCaptionMailbox::Message result;
    assert(!mailbox.TryTake(result));
    // Every character boundary around the byte cap, including 2/3/4-byte UTF-8.
    for(const char* glyph : {"a","\xc3\xa1","\xe1\xbb\x9d","\xf0\x9f\x91\x8b"}) {
        for(size_t prefix=760;prefix<775;++prefix) {
            std::string text(prefix,'x');text+=glyph;text+="z";
            assert(mailbox.Publish(owner,true,text.c_str()));
            assert(mailbox.TryTake(result));
            size_t expected=std::min(text.size(),ChatCaptionMailbox::kTextCapacity-1);
            while(expected && (static_cast<unsigned char>(text[expected]) & 0xc0)==0x80)--expected;
            assert(std::string(result.text)==text.substr(0,expected));
        }
    }
    std::atomic<bool> finished{false};
    auto receiver=std::async(std::launch::async,[&]{
        for(unsigned n=1;n<=10000;++n) {
            auto current=owner;current.response_generation=n;
            mailbox.Publish(current,true,std::to_string(n).c_str());
        }
        finished=true;
    });
    while(!finished)if(mailbox.TryTake(result))
        assert(std::stoul(result.text)==result.owner.response_generation);
    receiver.get();
    LcdDisplay display;display.StartChatCaptionTimer();assert(display.chat_caption_timer_);
    mailbox.Publish(owner,false,"question");display.Tick();
    assert(display.rendered=="question" && display.role=="user");
    std::promise<void> entered,release;
    auto released=release.get_future();
    display.drawing=[&]{entered.set_value();released.wait();};
    mailbox.Publish(owner,true,"first");
    auto ui=std::async(std::launch::async,[&]{display.Tick();});
    entered.get_future().wait();
    // Rendering is stalled, yet a full caption burst completes on RX.
    for(int n=0;n<300;++n)assert(mailbox.Publish(owner,true,std::to_string(n).c_str()));
    release.set_value();ui.get();display.drawing={};
    assert(display.rendered=="first");
    display.Tick();assert(display.rendered=="299" && display.role=="assistant");
    assert(!mailbox.TryTake(result));
    display.lesson_mode_active_=true;
    mailbox.Publish(owner,true,"lesson must keep ownership");display.Tick();
    assert(display.rendered=="299");
    fail_timer=true;LcdDisplay failed;failed.StartChatCaptionTimer();assert(!failed.chat_caption_timer_);
}
'''
    source = tmp_path / "captions.cc"
    source.write_text(fixture.replace("// PRODUCTION_TIMER", method(lcd, "void LcdDisplay::StartChatCaptionTimer")))
    binary = tmp_path / "captions"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
        f"-fsanitize={'address,undefined' if sanitize == 'address' else 'thread'}",
        "-fno-omit-frame-pointer", "-I", str(ROOT / "main"), str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
