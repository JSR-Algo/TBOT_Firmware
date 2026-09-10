#include <atomic>
#include <mutex>
#include <functional>
#define private public
#include "chat_playout_intake.h"
#undef private
#include <cassert>
#include <future>
std::function<void()> stop_publish_hook;
int main() {
    ChatPlayoutIntake intake;
    ChatPlayoutIntake::Response response{{1,3},4,5,6,7};
    const auto stamp=intake.Establish(response);
    ChatPlayoutIntake::Capture captured;
    assert(stamp && intake.TryCapture(captured) && captured.stamp==stamp);
    ChatPlayoutIntake::Stop stop;
    stop.capture=captured;stop.received_us=100;stop.reset_epoch=8;stop.reset_captured=true;
    stop.drain_id[0]='x';stop.drain_id_size=1;
    stop.valid=true;
    assert(intake.PublishStop(stop));
    stop.received_us=200; assert(intake.PublishStop(stop));
    ChatPlayoutIntake::Stop owned;
    assert(intake.TryCollect(stamp,owned)==ChatPlayoutIntake::Read::Ready);
    assert(owned.received_us==100 && owned.drain_id[0]=='x');
    stop.interrupt=true;
    assert(!intake.PublishStop(stop));
    assert(intake.TryCollect(stamp,owned)==ChatPlayoutIntake::Read::Ready);
    assert(owned.interrupt && owned.received_us==100);
    stop.interrupt=false;
    response.response_generation=9;
    const auto next=intake.Establish(response);
    assert(next!=stamp && !intake.PublishStop(stop));
    assert(intake.TryCollect(next,owned)==ChatPlayoutIntake::Read::None);
    assert(intake.TryCapture(captured)); stop.capture=captured;
    std::promise<void> held,release;
    auto released=release.get_future().share();
    auto holder=std::async(std::launch::async,[&]{std::lock_guard<std::mutex> lock(intake.mutex_);held.set_value();released.wait();});
    held.get_future().wait();
    assert(!intake.PublishStop(stop));
    assert(intake.TryCollect(next,owned)==ChatPlayoutIntake::Read::Fault);
    release.set_value();holder.get();

    ChatPlayoutIntake initial;
    auto initial_stamp=initial.Establish(response);
    assert(initial.TryCapture(stop.capture));
    std::promise<void> storing,resume;
    auto resumed=resume.get_future().share();
    stop_publish_hook=[&]{storing.set_value();resumed.wait();};
    auto writer=std::async(std::launch::async,[&]{return initial.PublishStop(stop);});
    storing.get_future().wait();
    assert(initial.TryCollect(initial_stamp,owned)==ChatPlayoutIntake::Read::Fault);
    ++response.response_generation;
    const auto successor_stamp=initial.Establish(response);
    assert(initial.TryCollect(successor_stamp,owned)==ChatPlayoutIntake::Read::None);
    resume.set_value();assert(writer.get());stop_publish_hook={};
}
