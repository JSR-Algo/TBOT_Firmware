#ifndef _ESP_TCP_H_
#define _ESP_TCP_H_

#include "tcp.h"
#include "esp_tcp_shutdown_state.h"

#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#define ESP_TCP_EVENT_RECEIVE_TASK_EXIT 1
#define ESP_TCP_EVENT_RECEIVE_TASK_START 2

class EspTcp : public Tcp {
public:
    EspTcp();
    ~EspTcp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    int GetLastError() override;

private:
    static constexpr uint32_t kReceiveTaskJoinTimeoutMs = 1000;

    std::atomic<int> tcp_fd_{-1};
    std::atomic<bool> stop_requested_{true};
    std::atomic<bool> disconnect_notified_{true};
    std::mutex lifecycle_mutex_;
    std::mutex send_mutex_;
    EventGroupHandle_t event_group_ = nullptr;
    std::atomic<TaskHandle_t> receive_task_handle_{nullptr};
    int last_error_ = 0;
    EspTcpShutdownState shutdown_state_;

    std::function<void()> ReceiveTask();
    // 内部断开处理函数
    // wait_for_task: 是否等待接收任务退出（主动断开为true，被动断开为false）
    std::function<void()> DoDisconnect(bool wait_for_task);
};

#endif // _ESP_TCP_H_
