#include "course_mode_local_endpoint_policy.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void RequireOtaRejected(std::string_view url) {
    if (IsValidCourseModeOtaUrl(url)) {
        std::cerr << "FAIL: unsafe OTA URL accepted: " << url << "\n";
        std::exit(1);
    }
}

void RequireWebsocketRejected(std::string_view url) {
    if (IsValidCourseModeWebsocketUrl(url)) {
        std::cerr << "FAIL: unsafe WebSocket URL accepted: " << url << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    Require(IsValidCourseModeOtaUrl("http://10.0.0.5/tbot/ota/"), "private IPv4 OTA");
    Require(IsValidCourseModeOtaUrl("http://192.168.100.209:8003/tbot/ota/"),
            "private IPv4 OTA with port");
    Require(IsValidCourseModeOtaUrl("http://[fd12:3456::7]:8003/tbot/ota/"),
            "ULA IPv6 OTA");
    Require(IsValidCourseModeWebsocketUrl("ws://172.31.255.254/tbot/v1/"),
            "private IPv4 WebSocket");
    Require(IsValidCourseModeWebsocketUrl("ws://[fc00::1]:8000/tbot/v1/"),
            "ULA IPv6 WebSocket");

    for (const std::string_view url : {
             "", "https://10.0.0.5/tbot/ota/", "ftp://10.0.0.5/tbot/ota/",
             "http://example.com/tbot/ota/", "http://8.8.8.8/tbot/ota/",
             "http://127.0.0.1/tbot/ota/", "http://169.254.1.2/tbot/ota/",
             "http://224.0.0.1/tbot/ota/", "http://user:pass@10.0.0.5/tbot/ota/",
             "http://10.0.0.5/tbot/ota/?token=x", "http://10.0.0.5/tbot/ota/#frag",
             "http://10.0.0.5:0/tbot/ota/", "http://10.0.0.5:65536/tbot/ota/",
             "http://10.0.0.5:/tbot/ota/", "http://010.0.0.5/tbot/ota/",
             "http://[fe80::1]/tbot/ota/", "http://[::1]/tbot/ota/",
             "http://[fd12::zz]/tbot/ota/", "http://fd12::1/tbot/ota/",
             "http://10.0.0.5/tbot\\ota/", "http://10.0.0.5/tbot/ota/\nleak",
             "http://10.0.0.5/tbot/ota/\xC3\xA9",
         }) {
        RequireOtaRejected(url);
    }

    for (const std::string_view url : {
             "", "wss://10.0.0.5/tbot/v1/", "http://10.0.0.5/tbot/v1/",
             "ws://example.com/tbot/v1/", "ws://1.1.1.1/tbot/v1/",
             "ws://127.0.0.1/tbot/v1/", "ws://169.254.3.4/tbot/v1/",
             "ws://user@10.0.0.5/tbot/v1/", "ws://10.0.0.5/tbot/v1/?x=1",
             "ws://10.0.0.5/tbot/v1/#frag", "ws://10.0.0.5:abc/tbot/v1/",
             "ws://[fe80::1]/tbot/v1/", "ws://[2001:db8::1]/tbot/v1/",
             "ws://10.0.0.5/tbot/v1/\tbad",
         }) {
        RequireWebsocketRejected(url);
    }

    std::cout << "PASS: course-mode local endpoint policy\n";
    return 0;
}
