#ifndef TEST_ESP_TASK_WDT_H
#define TEST_ESP_TASK_WDT_H

inline int g_esp_task_wdt_reset_calls = 0;

inline int esp_task_wdt_reset() {
    ++g_esp_task_wdt_reset_calls;
    return 0;
}

#endif
