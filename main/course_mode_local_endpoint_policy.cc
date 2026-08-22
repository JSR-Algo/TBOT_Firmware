#include "course_mode_local_endpoint_policy.h"

#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#endif

#include <array>
#include <cstdint>

namespace {

bool IsAsciiUrl(std::string_view url) {
    if (url.empty() || url.size() > 2048) return false;
    for (const unsigned char byte : url) {
        if (byte < 0x21 || byte > 0x7e || byte == '\\') return false;
    }
    return true;
}

bool IsValidPort(std::string_view port) {
    if (port.empty() || port.size() > 5) return false;
    unsigned value = 0;
    for (const char ch : port) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<unsigned>(ch - '0');
    }
    return value >= 1 && value <= 65535;
}

bool IsPrivateIpv4(std::string_view host) {
    std::array<unsigned, 4> octets{};
    size_t start = 0;
    for (size_t index = 0; index < octets.size(); ++index) {
        const size_t end = index + 1 == octets.size() ? host.size() : host.find('.', start);
        if (end == std::string_view::npos || end == start || end - start > 3) return false;
        if (end - start > 1 && host[start] == '0') return false;
        unsigned value = 0;
        for (size_t cursor = start; cursor < end; ++cursor) {
            const char ch = host[cursor];
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + static_cast<unsigned>(ch - '0');
        }
        if (value > 255) return false;
        octets[index] = value;
        start = end + 1;
    }
    return start == host.size() + 1 &&
           (octets[0] == 10 ||
            (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
            (octets[0] == 192 && octets[1] == 168));
}

bool IsUlaIpv6(std::string_view host) {
    if (host.empty() || host.size() >= INET6_ADDRSTRLEN) return false;
    std::array<char, INET6_ADDRSTRLEN> text{};
    for (size_t index = 0; index < host.size(); ++index) text[index] = host[index];
    std::array<uint8_t, 16> address{};
    return inet_pton(AF_INET6, text.data(), address.data()) == 1 &&
           (address[0] & 0xfe) == 0xfc;
}

bool IsValidLocalUrl(std::string_view url, std::string_view scheme) {
    if (!IsAsciiUrl(url) || url.size() < scheme.size() ||
        url.substr(0, scheme.size()) != scheme) {
        return false;
    }
    const size_t authority_start = scheme.size();
    const size_t authority_end = url.find('/', authority_start);
    const std::string_view authority = url.substr(
        authority_start,
        authority_end == std::string_view::npos ? url.size() - authority_start
                                                : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        url.find('?') != std::string_view::npos || url.find('#') != std::string_view::npos) {
        return false;
    }

    std::string_view host;
    std::string_view port;
    bool has_port = false;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(1, close - 1);
        const std::string_view suffix = authority.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') return false;
            has_port = true;
            port = suffix.substr(1);
        }
        if (!IsUlaIpv6(host)) return false;
    } else {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            if (authority.find(':', colon + 1) != std::string_view::npos) return false;
            has_port = true;
            port = authority.substr(colon + 1);
        }
        if (!IsPrivateIpv4(host)) return false;
    }
    return !has_port || IsValidPort(port);
}

}  // namespace

bool IsValidCourseModeOtaUrl(std::string_view url) {
    return IsValidLocalUrl(url, "http://");
}

bool IsValidCourseModeWebsocketUrl(std::string_view url) {
    return IsValidLocalUrl(url, "ws://");
}
