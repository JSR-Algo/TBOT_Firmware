#include "backend_recovery_window.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    BackendRecoveryWindow window(60000);

    Require(!window.ShouldEnterWifiConfig(1000),
            "first backend failure starts the recovery window");
    Require(!window.ShouldEnterWifiConfig(60999),
            "fallback must not fire before 60 seconds");
    Require(window.ShouldEnterWifiConfig(61000),
            "continuous backend failure enters WiFi config at 60 seconds");
    Require(!window.ShouldEnterWifiConfig(62000),
            "fallback is emitted once until reset");

    window.Reset();
    Require(!window.ShouldEnterWifiConfig(120000),
            "successful recovery resets the failure window");
    Require(!window.ShouldEnterWifiConfig(179999),
            "a new window receives its full 60-second budget");
    Require(window.ShouldEnterWifiConfig(180000),
            "the new failure window expires independently");

    std::cout << "backend_recovery_window_test: PASS\n";
    return 0;
}
