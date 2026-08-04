#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if TBOT_NATIVE_COVERAGE
#else
#include "application.h"
#include "lesson_cinematic_evidence.h"
#endif

#define TAG "main"

#if !TBOT_NATIVE_COVERAGE
static void AllocationFailureHook(size_t requested_size, uint32_t caps,
                                  const char* function_name) {
    esp_rom_printf("heap_alloc_failed size=%u caps=0x%08x function=%s\n",
                   static_cast<unsigned>(requested_size),
                   static_cast<unsigned>(caps),
                   function_name != nullptr ? function_name : "unknown");
}
#endif

#if TBOT_NATIVE_COVERAGE
int TbotAfskDemodHostCoverageMain();
extern "C" void esp_gcov_dump(void);

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "TBOT native coverage: running AFSK harness");
    int result = TbotAfskDemodHostCoverageMain();
    ESP_LOGI(TAG, "TBOT native coverage: ready to dump GCOV data");
    esp_gcov_dump();
    ESP_LOGI(TAG, "TBOT native coverage: GCOV data dumped, result=%d", result);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#else
extern "C" void app_main(void)
{
    tbot::LessonCinematicEvidenceBoot();

    // ESP-IDF's INFO logs include raw station/BLE identifiers. Keep warnings
    // while preventing SSIDs, BSSIDs, MACs, and assigned IPs from reaching
    // production serial logs.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("BLE_INIT", ESP_LOG_WARN);

    esp_err_t heap_hook_err =
        heap_caps_register_failed_alloc_callback(AllocationFailureHook);
    if (heap_hook_err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to register allocation failure diagnostics: %s",
                 esp_err_to_name(heap_hook_err));
    }

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
#endif
