#ifndef _ESP_SSL_H_
#define _ESP_SSL_H_

#include "tcp.h"
#include "esp_tcp_shutdown_state.h"
#include <esp_tls.h>

#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#define ESP_SSL_EVENT_RECEIVE_TASK_EXIT 1
#define ESP_SSL_EVENT_RECEIVE_TASK_START 2

class EspSsl : public Tcp {
public:
    EspSsl();
    ~EspSsl();

    void SetTimeout(int timeout_ms) override;
    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    int GetLastError() override;

private:
    static constexpr uint32_t kReceiveTaskJoinTimeoutMs = 1000;

    esp_tls_t* tls_client_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    std::atomic<TaskHandle_t> receive_task_handle_{nullptr};
    std::atomic<bool> stop_requested_{true};
    std::atomic<bool> disconnect_notified_{true};
    std::mutex lifecycle_mutex_;
    std::mutex send_mutex_;
    int timeout_ms_ = 30000;
    int last_error_ = 0;
    EspSslShutdownState shutdown_state_;

    std::function<void()> DoDisconnect(bool wait_for_task);
    std::function<void()> ReceiveTask();
};

#endif // _ESP_SSL_H_
