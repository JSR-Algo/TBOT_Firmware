#pragma once
// Host stub mirroring tests/native_stubs/esp_log.h: log macros are no-ops on the
// host. Log-emission lines are not meaningful statement coverage (documented
// exception, same class as the afsk harness).
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
