#include "esp_ssl.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

static const char *TAG = "EspSsl";

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();
    configASSERT(shutdown_state_.CanDeleteSynchronization());

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void EspSsl::SetTimeout(int timeout_ms) {
    if (timeout_ms > 0) {
        timeout_ms_ = timeout_ms;
    }
}

bool EspSsl::Connect(const std::string& host, int port) {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto prior_disconnect_callback = DoDisconnect(true);
    if (prior_disconnect_callback) {
        lifecycle_lock.unlock();
        prior_disconnect_callback();
        lifecycle_lock.lock();
        if (shutdown_state_.NeedsJoin() || tls_client_ != nullptr) {
            last_error_ = EBUSY;
            return false;
        }
    }
    if (event_group_ == nullptr) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to create TLS synchronization");
        return false;
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = timeout_ms_;

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        esp_tls_error_handle_t last_error;
        if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK) {
            int error_code, error_flags;
            last_error_ = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
            ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        } else {
            last_error_ = -1;
            ESP_LOGE(TAG, "Failed to get TLS error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

    stop_requested_.store(false);
    disconnect_notified_.store(false);
    connected_ = true;
    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT |
                                       ESP_SSL_EVENT_RECEIVE_TASK_START);
    shutdown_state_.TaskStarted();

    TaskHandle_t receive_task = nullptr;
    BaseType_t created = xTaskCreate([](void* arg) {
        EspSsl* ssl = static_cast<EspSsl*>(arg);
        xEventGroupWaitBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_START,
                            pdFALSE, pdFALSE, portMAX_DELAY);
        auto disconnect_callback = ssl->ReceiveTask();
        if (disconnect_callback) {
            disconnect_callback();
        }
        EventGroupHandle_t event_group = ssl->event_group_;
        configASSERT(ssl->shutdown_state_.TaskWillExit());
        ssl->receive_task_handle_.store(nullptr);
        ssl->shutdown_state_.TaskExited();
        xEventGroupSetBits(event_group, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(nullptr);
    }, "ssl_receive", 4096, this, 1, &receive_task);
    if (created != pdPASS) {
        configASSERT(shutdown_state_.TaskWillExit());
        shutdown_state_.TaskExited();
        shutdown_state_.TaskJoined();
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        stop_requested_.store(true);
        connected_ = false;
        receive_task_handle_.store(nullptr);
        disconnect_notified_.store(true);
        return false;
    }

    receive_task_handle_.store(receive_task);
    xEventGroupSetBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_START);
    return true;
}

void EspSsl::Disconnect() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto disconnect_callback = DoDisconnect(true);
    lifecycle_lock.unlock();
    if (disconnect_callback) {
        disconnect_callback();
    }
}

std::function<void()> EspSsl::DoDisconnect(bool wait_for_task) {
    stop_requested_.store(true);
    connected_ = false;
    std::function<void()> disconnect_callback;
    if (!disconnect_notified_.exchange(true)) {
        disconnect_callback = disconnect_callback_;
    }

    if (tls_client_ != nullptr) {
        int sockfd = -1;
        if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
            shutdown(sockfd, SHUT_RDWR);
        }
    }

    if (wait_for_task && shutdown_state_.NeedsJoin()) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT,
                                        pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(kReceiveTaskJoinTimeoutMs));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "TLS receive task failed to exit after socket shutdown");
            abort();
        }
        receive_task_handle_.store(nullptr);
        shutdown_state_.TaskJoined();
    }

    if (tls_client_ != nullptr) {
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
    return disconnect_callback;
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    if (stop_requested_.load() || !connected_.load() || tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    while (total_sent < data.size()) {
        int ret = esp_tls_conn_write(tls_client_, data.data() + total_sent,
                                     data.size() - total_sent);
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        total_sent += ret;
    }
    return total_sent;
}

std::function<void()> EspSsl::ReceiveTask() {
    std::string data;
    std::function<void()> disconnect_callback;
    esp_tls_t* tls_client = tls_client_;
    while (!stop_requested_.load()) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client, data.data(), data.size());
        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
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

int EspSsl::GetLastError() {
    return last_error_;
}
