#include "audio/opus_encoder_serialization.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "opus encoder serialization test failed: " << message << "\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    std::atomic<bool> contender_started{false};
    std::atomic<bool> contender_acquired{false};

    auto lifetime_lease = OpusEncoderSerialization::Acquire();
    std::thread contender([&]() {
        contender_started.store(true, std::memory_order_release);
        auto process_lease = OpusEncoderSerialization::Acquire();
        contender_acquired.store(true, std::memory_order_release);
    });

    while (!contender_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(!contender_acquired.load(std::memory_order_acquire),
            "a process lease must wait for an encoder lifetime lease");

    lifetime_lease.unlock();
    contender.join();
    Require(contender_acquired.load(std::memory_order_acquire),
            "the waiting process lease must acquire after lifetime release");
}
