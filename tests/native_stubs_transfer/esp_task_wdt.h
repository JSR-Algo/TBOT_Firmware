#ifndef TEST_ESP_TASK_WDT_H
#define TEST_ESP_TASK_WDT_H

#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 0x105

inline int g_esp_task_wdt_status_result = ESP_ERR_NOT_FOUND;
inline int g_esp_task_wdt_reset_calls = 0;

inline int esp_task_wdt_status(void*) {
    return g_esp_task_wdt_status_result;
}

inline int esp_task_wdt_reset() {
    ++g_esp_task_wdt_reset_calls;
    return 0;
}

#endif
