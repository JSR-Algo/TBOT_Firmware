#include "firmware_version_policy.h"

#include <cassert>

int main() {
    ParsedFirmwareVersion parsed;
    assert(ParseStrictFirmwareVersion("2.2.85", &parsed));
    assert(!ParseStrictFirmwareVersion("", &parsed));
    assert(!ParseStrictFirmwareVersion("1..2", &parsed));
    assert(!ParseStrictFirmwareVersion("1.a", &parsed));
    assert(!ParseStrictFirmwareVersion("-1.2", &parsed));
    assert(!ParseStrictFirmwareVersion("1.2 ", &parsed));
    assert(!ParseStrictFirmwareVersion("4294967296.0", &parsed));
    assert(!ParseStrictFirmwareVersion("1.2.3.4.5.6.7.8.9", &parsed));

    auto same_without_url = EvaluateFirmwareResponse("2.2.85", "2.2.85", "", false);
    assert(same_without_url.valid);
    assert(!same_without_url.should_download);

    auto newer_without_url = EvaluateFirmwareResponse("2.2.85", "2.2.86", "", false);
    assert(!newer_without_url.valid);

    auto newer_with_url = EvaluateFirmwareResponse(
        "2.2.85", "2.2.86", "https://esp.example/fw.bin", false);
    assert(newer_with_url.valid);
    assert(newer_with_url.should_download);

    auto forced_without_url = EvaluateFirmwareResponse("2.2.85", "2.2.85", "", true);
    assert(!forced_without_url.valid);

    auto trailing_zero = EvaluateFirmwareResponse("2.2", "2.2.0", "", false);
    assert(trailing_zero.valid);
    assert(!trailing_zero.should_download);

    auto malformed = EvaluateFirmwareResponse(
        "2.2.85", "2.2.not-a-number", "https://esp.example/fw.bin", false);
    assert(!malformed.valid);
    return 0;
}
