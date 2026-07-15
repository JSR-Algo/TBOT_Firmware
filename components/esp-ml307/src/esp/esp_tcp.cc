#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <cstdlib>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();
    configASSERT(shutdown_state_.CanDeleteSynchronization());

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto prior_disconnect_callback = DoDisconnect(true);
    if (prior_disconnect_callback) {
        lifecycle_lock.unlock();
        prior_disconnect_callback();
        lifecycle_lock.lock();
        if (shutdown_state_.NeedsJoin() || tcp_fd_.load() != -1) {
            last_error_ = EBUSY;
            return false;
        }
    }
    if (event_group_ == nullptr) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to create TCP synchronization");
        return false;
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }
    tcp_fd_.store(fd);

    int ret = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(fd);
        tcp_fd_.store(-1);
        return false;
    }

    stop_requested_.store(false);
    disconnect_notified_.store(false);
    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT |
                                       ESP_TCP_EVENT_RECEIVE_TASK_START);
    shutdown_state_.TaskStarted();
    TaskHandle_t receive_task = nullptr;
    BaseType_t created = xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        xEventGroupWaitBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_START,
                            pdFALSE, pdFALSE, portMAX_DELAY);
        auto disconnect_callback = tcp->ReceiveTask();
        if (disconnect_callback) {
            disconnect_callback();
        }
        EventGroupHandle_t event_group = tcp->event_group_;
        configASSERT(tcp->shutdown_state_.TaskWillExit());
        tcp->receive_task_handle_.store(nullptr);
        tcp->shutdown_state_.TaskExited();
        xEventGroupSetBits(event_group, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task);
    if (created != pdPASS) {
        configASSERT(shutdown_state_.TaskWillExit());
        shutdown_state_.TaskExited();
        shutdown_state_.TaskJoined();
        shutdown(fd, SHUT_RDWR);
        close(fd);
        tcp_fd_.store(-1);
        stop_requested_.store(true);
        connected_ = false;
        receive_task_handle_.store(nullptr);
        disconnect_notified_.store(true);
        return false;
    }
    receive_task_handle_.store(receive_task);
    xEventGroupSetBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_START);
    return true;
}

void EspTcp::Disconnect() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto disconnect_callback = DoDisconnect(true);
    lifecycle_lock.unlock();
    if (disconnect_callback) {
        disconnect_callback();
    }
}

std::function<void()> EspTcp::DoDisconnect(bool wait_for_task) {
    stop_requested_.store(true);
    connected_ = false;
    std::function<void()> disconnect_callback;
    if (!disconnect_notified_.exchange(true)) {
        disconnect_callback = disconnect_callback_;
    }

    int fd = tcp_fd_.load();
    if (fd != -1) {
        shutdown(fd, SHUT_RDWR);
    }

    if (wait_for_task && shutdown_state_.NeedsJoin()) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                                        pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(kReceiveTaskJoinTimeoutMs));
        if (bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT) {
            receive_task_handle_.store(nullptr);
            shutdown_state_.TaskJoined();
        } else {
            ESP_LOGE(TAG, "Receive task failed to exit after socket shutdown");
            abort();
        }
    }

    {
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        fd = tcp_fd_.exchange(-1);
        if (fd != -1) {
            close(fd);
        }
    }

    return disconnect_callback;
}

int EspTcp::Send(const std::string& data) {
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    if (stop_requested_.load() || !connected_.load()) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int fd = tcp_fd_.load();
        if (fd == -1) {
            return -1;
        }
        int ret = send(fd, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

std::function<void()> EspTcp::ReceiveTask() {
    std::string data;
    std::function<void()> disconnect_callback;
    while (!stop_requested_.load()) {
        data.resize(1500);
        int fd = tcp_fd_.load();
        if (fd == -1) {
            break;
        }
        int ret = recv(fd, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            stop_requested_.store(true);
            connected_ = false;
            if (!disconnect_notified_.exchange(true)) {
                disconnect_callback = disconnect_callback_;
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
    return disconnect_callback;
}

int EspTcp::GetLastError() {
    return last_error_;
}
