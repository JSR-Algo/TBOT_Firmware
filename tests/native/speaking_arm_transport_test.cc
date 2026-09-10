#include "speaking_arm_transport.h"
#include <cassert>
#include <functional>
#include <thread>
#include <vector>

struct FakeUart {
    bool ready = true, idle = true;
    size_t free = 1024;
    std::function<void()> select = [] {};
    std::vector<std::string> lines;
    bool Ready() { return ready; }
    bool Idle() { return idle; }
    size_t BufferSpace() { return free; }
    bool SelectProfile() { select(); return true; }
    bool Write(const std::string& payload) { lines.push_back(payload); return true; }
};

int main() {
    std::recursive_mutex mutex;
    FakeUart uart;
    bool owner = true;
    auto send = [&](bool left) { return TrySpeakingArmWrite(mutex, left, 20, uart, [&] { return owner; }); };
    assert(send(true));
    assert(uart.lines.back() == "{\"cmd\":\"servo\",\"part\":\"left_arm\",\"action\":\"set_percent\",\"from\":0,\"to\":12,\"step\":2,\"delay_ms\":20}\n");
    assert(send(false));
    assert(uart.lines.back() == "{\"cmd\":\"servo\",\"part\":\"right_arm\",\"action\":\"set_percent\",\"from\":60,\"to\":48,\"step\":2,\"delay_ms\":20}\n");
    uart.idle = false; assert(!send(true)); uart.idle = true;
    uart.ready = false; assert(!send(true)); uart.ready = true;
    uart.free = 128; assert(!send(true)); uart.free = 1024;
    mutex.lock();
    std::thread busy([&] { assert(!send(true)); });
    busy.join();
    mutex.unlock();
    uart.select = [&] { owner = false; };
    assert(!send(true)); // Explicit owner changes after lock/profile selection.
    assert(uart.lines.size() == 2);
}
