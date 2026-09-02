#pragma once

template <typename... Args>
void HostEspLog(Args&&...) {}

#define ESP_LOGE(...) HostEspLog(__VA_ARGS__)
#define ESP_LOGW(...) HostEspLog(__VA_ARGS__)
#define ESP_LOGI(...) HostEspLog(__VA_ARGS__)
