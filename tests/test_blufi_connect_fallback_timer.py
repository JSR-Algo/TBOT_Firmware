"""Execute the credential fallback across stale and allocation-failure boundaries."""
from pathlib import Path
import subprocess

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def test_fallback_uses_timer_and_preserves_session_ownership(tmp_path):
    source = (ROOT / "main/boards/common/blufi.cpp").read_text()
    body = method(source, "void Blufi::ScheduleStationConnectFallback")
    assert "xTaskCreate" not in body
    fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <new>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_OK 0
#define ESP_TIMER_TASK 0
struct Timer;
using esp_timer_handle_t=Timer*;
struct esp_timer_create_args_t {
 void (*callback)(void*); void* arg; int dispatch_method; const char* name;
};
struct Timer { esp_timer_create_args_t args; };
std::vector<Timer*> timers;
bool fail_create=false,fail_start=false;
int esp_timer_create(const esp_timer_create_args_t* args,Timer** result){
 if(fail_create)return -1;
 *result=new Timer{*args};timers.push_back(*result);return 0;
}
int esp_timer_start_once(Timer*,uint64_t delay){assert(delay==500000);return fail_start?-1:0;}
int esp_timer_delete(Timer* timer){
 for(auto& entry:timers)if(entry==timer)entry=nullptr;
 delete timer;return 0;
}
void fire(){for(auto* timer:timers)if(timer){auto args=timer->args;args.callback(args.arg);}}
struct Application {
 std::vector<std::function<void()>> work;
 static Application& GetInstance(){static Application app;return app;}
 void Schedule(std::function<void()> f){work.push_back(f);}
 void drain(){auto pending=std::move(work);work.clear();for(auto& f:pending)f();}
};
struct Blufi {
 std::atomic<uint32_t> setup_generation_{1};
 std::atomic<bool> m_sta_is_connecting{true},m_wifi_connect_task_started{false};
 uint64_t current_epoch=1;unsigned starts=0;
 void StartStationConnectFromCredentials(const char*,uint64_t epoch){
  if(epoch!=current_epoch||m_wifi_connect_task_started.exchange(true))return;
  ++starts;
 }
 void ScheduleStationConnectFallback(uint64_t);
};
''' + body + r'''
int main(){
 Blufi b;auto& app=Application::GetInstance();
 b.ScheduleStationConnectFallback(1);assert(b.starts==0);fire();assert(b.starts==0);
 app.drain();assert(b.starts==1);
 b.m_wifi_connect_task_started=false;
 b.ScheduleStationConnectFallback(1);fire();++b.setup_generation_;app.drain();assert(b.starts==1);
 b.ScheduleStationConnectFallback(1);++b.setup_generation_;fire();app.drain();assert(b.starts==1);
 b.ScheduleStationConnectFallback(1);b.current_epoch=2;fire();app.drain();assert(b.starts==1);
 b.ScheduleStationConnectFallback(2);b.m_wifi_connect_task_started=true;fire();app.drain();assert(b.starts==1);
 b.m_wifi_connect_task_started=false;b.m_sta_is_connecting=false;
 b.ScheduleStationConnectFallback(2);fire();app.drain();assert(b.starts==1);
 b.m_sta_is_connecting=true;fail_create=true;
 b.ScheduleStationConnectFallback(2);app.drain();assert(b.starts==2);
 fail_create=false;fail_start=true;b.m_wifi_connect_task_started=false;
 b.ScheduleStationConnectFallback(2);++b.setup_generation_;app.drain();assert(b.starts==2);
 b.ScheduleStationConnectFallback(2);app.drain();assert(b.starts==3);
 for(auto* timer:timers)assert(timer==nullptr);
}
'''
    generated = tmp_path / "fallback.cc"
    generated.write_text(fixture)
    binary = tmp_path / "fallback"
    subprocess.run(["c++", "-std=c++20", "-fsanitize=address,undefined",
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
