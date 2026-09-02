#include "afsk_demod.h"
#include "application.h"
#include "display.h"
#include "ssid_manager.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace audio_wifi_config;

namespace audio_wifi_config {
std::vector<std::vector<float>>& HostInjectedProbabilityFrames();
bool& HostDropDecodedTextAfterProcess();
}

HostDelayControl &HostDelayState() {
    static HostDelayControl state;
    return state;
}

void vTaskDelay(int) {
    auto &state = HostDelayState();
    state.calls += 1;
    if (state.throw_after >= 0 && state.calls >= state.throw_after) {
        throw HostAbortDelay();
    }
}

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "native afsk host test failed: " << message << "\n";
        std::exit(1);
    }
}

std::vector<float> probabilities_for_bits(const std::vector<uint8_t> &bits) {
    std::vector<float> values;
    values.reserve(bits.size());
    for (uint8_t bit : bits) {
        values.push_back(bit ? 0.9f : 0.1f);
    }
    return values;
}

void append_byte_bits(std::vector<uint8_t> &bits, uint8_t value) {
    for (int shift = 7; shift >= 0; --shift) {
        bits.push_back(static_cast<uint8_t>((value >> shift) & 0x1));
    }
}

std::vector<uint8_t> byte_bits(uint8_t value) {
    std::vector<uint8_t> bits;
    append_byte_bits(bits, value);
    return bits;
}

std::vector<uint8_t> framed_bits(
    const std::vector<uint8_t> &start,
    const std::vector<uint8_t> &payload,
    const std::vector<uint8_t> &end
) {
    std::vector<uint8_t> bits;
    bits.insert(bits.end(), start.begin(), start.end());
    bits.insert(bits.end(), start.begin(), start.end());
    bits.insert(bits.end(), payload.begin(), payload.end());
    bits.insert(bits.end(), end.begin(), end.end());
    return bits;
}

std::vector<uint8_t> text_payload_bits(const std::string &text, bool checksum) {
    std::vector<uint8_t> payload;
    for (unsigned char ch : text) {
        append_byte_bits(payload, ch);
    }
    if (checksum) {
        append_byte_bits(payload, AudioDataBuffer::CalculateChecksum(text));
    }
    return payload;
}

std::vector<float> default_framed_probabilities(const std::string &text) {
    return probabilities_for_bits(framed_bits(
        kDefaultStartTransmissionPattern,
        text_payload_bits(text, true),
        kDefaultEndTransmissionPattern
    ));
}

void test_frequency_detector_edges() {
    FrequencyDetector detector(0.25f, 8);
    require(detector.GetAmplitude() == 0.0f, "new detector starts at zero amplitude");
    detector.ProcessSample(1.0f);
    detector.ProcessSample(-0.5f);
    require(detector.GetAmplitude() > 0.0f, "processed detector reports amplitude");
    detector.Reset();
    require(detector.GetAmplitude() == 0.0f, "reset clears amplitude");
}

void test_signal_processor_edges() {
    AudioSignalProcessor warning_processor(6410, 1800, 1500, 100, 8);
    auto warning_result = warning_processor.ProcessAudioSamples(std::vector<float>(96, 0.0f));
    require(!warning_result.empty(), "non-divisible sample-rate processor still produces output");

    AudioSignalProcessor processor(6400, 1800, 1500, 100, 8);
    std::vector<float> samples;
    samples.reserve(96);
    for (int i = 0; i < 96; ++i) {
        float t = static_cast<float>(i) / 6400.0f;
        samples.push_back(std::sin(2.0f * static_cast<float>(M_PI) * 1800.0f * t));
    }
    auto result = processor.ProcessAudioSamples(samples);
    require(!result.empty(), "mark tone produces probabilities after window fills");
}

void test_data_buffer_success_without_checksum() {
    const auto start = byte_bits(0xA5);
    const auto end = byte_bits(0x5A);
    auto payload = byte_bits('A');
    AudioDataBuffer buffer(8, start, end, false);

    bool done = buffer.ProcessProbabilityData(probabilities_for_bits(framed_bits(start, payload, end)), 0.5f);

    require(done, "framed payload without checksum completes");
    require(buffer.decoded_text.has_value(), "decoded text is present");
    require(buffer.decoded_text.value() == "A", "decoded payload matches input byte");
}

void test_data_buffer_checksum_success_and_failures() {
    const auto start = byte_bits(0xA5);
    const auto end = byte_bits(0x5A);
    std::vector<uint8_t> payload = byte_bits('B');
    append_byte_bits(payload, AudioDataBuffer::CalculateChecksum("B"));
    AudioDataBuffer ok(8, start, end, true);
    require(ok.ProcessProbabilityData(probabilities_for_bits(framed_bits(start, payload, end)), 0.5f),
            "checksum-valid payload completes");
    require(ok.decoded_text.value() == "B", "checksum-valid payload decodes");

    std::vector<uint8_t> mismatch = byte_bits('C');
    append_byte_bits(mismatch, static_cast<uint8_t>(AudioDataBuffer::CalculateChecksum("C") + 1));
    AudioDataBuffer bad_checksum(8, start, end, true);
    require(!bad_checksum.ProcessProbabilityData(probabilities_for_bits(framed_bits(start, mismatch, end)), 0.5f),
            "checksum mismatch is rejected");
    require(!bad_checksum.decoded_text.has_value(), "checksum mismatch produces no decoded text");

    AudioDataBuffer too_short(8, start, end, true);
    require(!too_short.ProcessProbabilityData(probabilities_for_bits(framed_bits(start, {}, end)), 0.5f),
            "checksum mode rejects too-short data");
}

void test_data_buffer_overflow_and_nonterminal_paths() {
    const auto start = byte_bits(0xA5);
    const auto end = byte_bits(0x5A);
    AudioDataBuffer overflow(1, start, end, false);
    std::vector<uint8_t> bits;
    bits.insert(bits.end(), start.begin(), start.end());
    bits.insert(bits.end(), start.begin(), start.end());
    for (int i = 0; i < 16; ++i) {
        bits.push_back(1);
    }
    require(!overflow.ProcessProbabilityData(probabilities_for_bits(bits), 0.5f),
            "overflow path clears and returns false");

    AudioDataBuffer waiting(8, start, end, false);
    require(!waiting.ProcessProbabilityData(probabilities_for_bits(start), 0.5f),
            "single start marker only enters waiting state");
}

void test_receive_loop_abort_edges() {
    Application app;
    WifiManager wifi;
    Display display;
    HostDelayState() = HostDelayControl{0, 2};
    try {
        ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 1);
        require(false, "idle receive loop should abort through delay stub");
    } catch (const HostAbortDelay &) {
        require(HostDelayState().calls == 2, "idle branch looped once before abort");
    }

    app.state = kDeviceStateWifiConfiguring;
    app.audio_service.next_read_ok = false;
    HostDelayState() = HostDelayControl{0, 2};
    try {
        ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 1);
        require(false, "read-failure receive loop should abort through delay stub");
    } catch (const HostAbortDelay &) {
        require(HostDelayState().calls == 2, "read failure branch looped once before abort");
    }
}

void test_receive_loop_decoded_payload_edges() {
    Application app;
    app.state = kDeviceStateWifiConfiguring;
    WifiManager wifi;
    Display display;
    SsidManager::GetInstance().Reset();

    auto &frames = HostInjectedProbabilityFrames();
    frames.clear();

    app.audio_service.next_read_ok = true;
    app.audio_service.next_audio = {1, 2, 3, 4, 5, 6, 7, 8};
    HostDelayState() = HostDelayControl{0, 2};
    try {
        ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 1);
        require(false, "empty probabilities should keep looping until delay abort");
    } catch (const HostAbortDelay &) {
        require(!wifi.stop_config_ap_called, "empty probabilities do not stop config AP");
    }

    frames.push_back(default_framed_probabilities("drop-me"));
    frames.push_back(default_framed_probabilities("ssid-only"));
    frames.push_back(default_framed_probabilities("Home\nsecret"));

    HostDropDecodedTextAfterProcess() = true;
    app.audio_service.next_read_ok = true;
    app.audio_service.next_audio = {1, 2, 3, 4, 5, 6, 7, 8};
    HostDelayState() = HostDelayControl{0, 1};
    try {
        ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 1);
        require(false, "dropped decoded text should keep looping until delay abort");
    } catch (const HostAbortDelay &) {
        require(!wifi.stop_config_ap_called, "dropped decoded text does not stop config AP");
    }

    app.audio_service.next_read_ok = true;
    app.audio_service.next_audio = {1, 10, 2, 20, 3, 30, 4, 40, 5, 50, 6, 60};
    HostDelayState() = HostDelayControl{0, 1};
    try {
        ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 2);
        require(false, "missing-newline payload should continue to delay abort");
    } catch (const HostAbortDelay &) {
        require(!wifi.stop_config_ap_called, "missing-newline payload does not stop config AP");
        require(SsidManager::GetInstance().saved.empty(), "missing-newline payload saves no credentials");
    }

    SsidManager::GetInstance().next_add_result = SsidMutationResult::kBusy;
    app.audio_service.next_read_ok = true;
    app.audio_service.next_audio = {1, 2, 3, 4, 5, 6, 7, 8};
    HostDelayState() = HostDelayControl{0, 1};
    try {
        ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 1);
        require(false, "busy credential save should continue to delay abort");
    } catch (const HostAbortDelay &) {
        require(!wifi.stop_config_ap_called, "busy credential save does not stop config AP");
        require(SsidManager::GetInstance().saved.empty(), "busy credential save persists nothing");
    }

    frames.push_back(default_framed_probabilities("Home\nsecret"));
    app.audio_service.next_read_ok = true;
    app.audio_service.next_audio = {1, 2, 3, 4, 5, 6, 7, 8};
    HostDelayState() = HostDelayControl{0, -1};
    ReceiveWifiCredentialsFromAudio(&app, &wifi, &display, 1);

    require(wifi.stop_config_ap_called, "valid decoded payload stops config AP");
    require(display.last_role == "system", "valid decoded payload updates display role");
    require(display.last_message == "Home", "display receives only SSID");
    require(SsidManager::GetInstance().saved.size() == 1, "valid payload saves one credential");
    require(SsidManager::GetInstance().saved[0].first == "Home", "saved SSID matches");
    require(SsidManager::GetInstance().saved[0].second == "secret", "saved password matches");
}

}  // namespace

#ifdef TBOT_ESP_NATIVE_COVERAGE
int TbotAfskDemodHostCoverageMain() {
#else
int main() {
#endif
    test_frequency_detector_edges();
    test_signal_processor_edges();
    test_data_buffer_success_without_checksum();
    test_data_buffer_checksum_success_and_failures();
    test_data_buffer_overflow_and_nonterminal_paths();
    test_receive_loop_abort_edges();
    test_receive_loop_decoded_payload_edges();
    return 0;
}
