#include "course_mode_hil_console.h"

#include "application.h"
#include "boards/common/board.h"
#include "course_mode_hil_diagnostic.h"
#include "esp_build_identity.h"
#include "system_info.h"

#include <esp_console.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <array>
#include <limits>
#include <memory>
#include <string>

namespace {

RTC_DATA_ATTR std::uint32_t hil_boot_count = 0;

const char* ResetReasonToken(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "powerOn";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interruptWatchdog";
        case ESP_RST_TASK_WDT: return "taskWatchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deepSleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

bool InitializeBootIdentity(CourseModeHilIdentity* identity) {
    if (identity == nullptr || hil_boot_count == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::array<unsigned char, 16> random {};
    esp_fill_random(random.data(), random.size());
    bool any_nonzero = false;
    for (unsigned char value : random) any_nonzero = any_nonzero || value != 0;
    if (!any_nonzero) return false;
    constexpr char kHex[] = "0123456789abcdef";
    identity->boot_id.resize(32);
    for (std::size_t index = 0; index < random.size(); ++index) {
        identity->boot_id[index * 2] = kHex[random[index] >> 4U];
        identity->boot_id[index * 2 + 1] = kHex[random[index] & 0x0FU];
    }
    // RTC slow memory survives the commanded software restart; power loss resets it.
    identity->boot_count = ++hil_boot_count;
    identity->reset_reason = ResetReasonToken(esp_reset_reason());
    return identity->boot_count != 0;
}

CourseModeHilDiagnostic BuildDiagnostic(const CourseModeHilIdentity& boot_identity) {
    CourseModeHilDiagnosticCallbacks callbacks;
    callbacks.identity = [boot_identity] {
        EspBuildIdentity build;
        std::string error;
        if (!ReadRunningEspBuildIdentity(&build, &error)) return CourseModeHilIdentity{};
        return CourseModeHilIdentity{
            Board::GetInstance().GetUuid(), SystemInfo::GetChipModelName(), build.app_sha256,
            boot_identity.boot_id, boot_identity.reset_reason, boot_identity.boot_count};
    };
    callbacks.tft_test_pattern = [] {
        return Application::GetInstance().RunCourseModeHilTftPattern();
    };
    callbacks.sd_read_cache = [](const std::string& path, const std::string& sha256) {
        return Application::GetInstance().RunCourseModeHilSdRead(path, sha256);
    };
    callbacks.audio_drain = [] {
        return Application::GetInstance().RunCourseModeHilAudioDrain();
    };
    callbacks.safe_motion = [](int duration_ms, bool require_rest) {
        return require_rest &&
               Application::GetInstance().RunCourseModeHilSafeMotion(duration_ms);
    };
    callbacks.stop_and_rest = [] {
        return Application::GetInstance().RunCourseModeHilStopAndRest();
    };
    callbacks.reboot = [] {
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }, "course_hil_reboot", 2048, nullptr, 2, nullptr);
    };
    return CourseModeHilDiagnostic(std::move(callbacks));
}

int HandleCommand(void* context, int argc, char** argv) {
    auto* diagnostic = static_cast<CourseModeHilDiagnostic*>(context);
    if (diagnostic == nullptr) return 1;
    std::string command;
    if (!DecodeCourseModeHilConsoleArgv(argc, argv, &command)) return 1;
    const std::string response = diagnostic->HandleLine(command);
    std::printf("%s\n", response.c_str());
    std::fflush(stdout);
    return 0;
}

}  // namespace

bool StartCourseModeHilConsole() {
    static std::unique_ptr<CourseModeHilDiagnostic> diagnostic;
    static esp_console_repl_t* repl = nullptr;
    if (diagnostic != nullptr || repl != nullptr) return false;
    CourseModeHilIdentity boot_identity;
    if (!InitializeBootIdentity(&boot_identity)) return false;
    diagnostic = std::make_unique<CourseModeHilDiagnostic>(BuildDiagnostic(boot_identity));
    const esp_console_cmd_t command = {
        .command = "course_mode_hil",
        .help = "Run nonce-bound Course Mode physical HIL probe",
        .hint = nullptr,
        .func = nullptr,
        .argtable = nullptr,
        .func_w_context = HandleCommand,
        .context = diagnostic.get(),
    };
    if (esp_console_cmd_register(&command) != ESP_OK) return false;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.max_cmdline_length = kCourseModeHilMaxEncodedBytes + 32;
    repl_config.prompt = "";
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t device_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(
            &device_config, &repl_config, &repl) != ESP_OK) return false;
#else
    esp_console_dev_uart_config_t device_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&device_config, &repl_config, &repl) != ESP_OK) return false;
#endif
    return esp_console_start_repl(repl) == ESP_OK;
}
