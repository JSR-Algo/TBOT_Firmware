#include "robot_uart.h"
#include "config.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef ROBOT_UART_NUM
#define ROBOT_UART_NUM UART_NUM_1
#endif

#ifndef ROBOT_UART_TX_PIN
#define ROBOT_UART_TX_PIN GPIO_NUM_17
#endif

#ifndef ROBOT_UART_RX_PIN
#define ROBOT_UART_RX_PIN GPIO_NUM_18
#endif

#ifndef ROBOT_UART_BAUD_RATE
#define ROBOT_UART_BAUD_RATE 115200
#endif

#ifndef ROBOT_UART_ALT_NUM
#define ROBOT_UART_ALT_NUM UART_NUM_2
#endif

#ifndef ROBOT_UART_ALT_TX_PIN
#define ROBOT_UART_ALT_TX_PIN GPIO_NUM_NC
#endif

#ifndef ROBOT_UART_ALT_RX_PIN
#define ROBOT_UART_ALT_RX_PIN GPIO_NUM_NC
#endif

#define TAG "RobotUart"

static bool configure_uart(uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (tx_pin == GPIO_NUM_NC || rx_pin == GPIO_NUM_NC) {
        return false;
    }
    uart_config_t uart_config = {
        .baud_rate = ROBOT_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
        },
    };

    esp_err_t err = uart_driver_install(port, 1024, 1024, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install port=%d failed: %s", port, esp_err_to_name(err));
        return false;
    }

    err = uart_param_config(port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config port=%d failed: %s", port, esp_err_to_name(err));
        return false;
    }

    err = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin port=%d failed: %s", port, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "UART ready: port=%d tx=%d rx=%d baud=%d",
        port, tx_pin, rx_pin, ROBOT_UART_BAUD_RATE);
    return true;
}

bool RobotUart::Initialize() {
    if (initialized_) {
        return true;
    }

    primary_ready_ = configure_uart(ROBOT_UART_NUM, ROBOT_UART_TX_PIN, ROBOT_UART_RX_PIN);
    alt_ready_ = configure_uart(ROBOT_UART_ALT_NUM, ROBOT_UART_ALT_TX_PIN, ROBOT_UART_ALT_RX_PIN);
    if (!primary_ready_ && !alt_ready_) {
        ESP_LOGE(TAG, "No robot UART port ready");
        return false;
    }

    initialized_ = true;
    return true;
}

bool RobotUart::SendLeftArmRaise() {
    return SendArmAction("left_arm", "raise");
}

bool RobotUart::SendRightArmRaise() {
    return SendArmAction("right_arm", "raise");
}

bool RobotUart::SendLeftArmLower() {
    return SendArmAction("left_arm", "lower");
}

bool RobotUart::SendRightArmLower() {
    return SendArmAction("right_arm", "lower");
}

bool RobotUart::SendBothArmsRaise() {
    bool left_ok = SendLeftArmRaise();
    bool right_ok = SendRightArmRaise();
    return left_ok && right_ok;
}

bool RobotUart::SendBothArmsLower() {
    bool left_ok = SendLeftArmLower();
    bool right_ok = SendRightArmLower();
    return left_ok && right_ok;
}

bool RobotUart::SendArmAction(const std::string& part, const std::string& action) {
    if (action == "raise") {
        return SendServoSweep(part, action, 0, 60, 2, 20);
    }
    if (action == "lower") {
        return SendServoSweep(part, action, 60, 0, 2, 20);
    }
    ESP_LOGW(TAG, "Unsupported arm action: %s", action.c_str());
    return false;
}

bool RobotUart::SendServoSweep(const std::string& part, const std::string& action, int from, int to, int step, int delay_ms) {
    if (!Initialize()) {
        return false;
    }

    std::string payload = "{\"cmd\":\"servo\",\"part\":\"" + part +
        "\",\"action\":\"" + action + "\",\"from\":" + std::to_string(from) +
        ",\"to\":" + std::to_string(to) +
        ",\"step\":" + std::to_string(step) +
        ",\"delay_ms\":" + std::to_string(delay_ms) + "}\n";

    bool wrote_any = false;
    if (primary_ready_) {
        uart_flush_input(ROBOT_UART_NUM);
        int written = uart_write_bytes(ROBOT_UART_NUM, payload.data(), payload.size());
        wrote_any = wrote_any || written == static_cast<int>(payload.size());
        if (written != static_cast<int>(payload.size())) {
            ESP_LOGW(TAG, "Primary UART write incomplete: %d/%d", written, static_cast<int>(payload.size()));
        }
    }
    if (alt_ready_) {
        uart_flush_input(ROBOT_UART_ALT_NUM);
        int written = uart_write_bytes(ROBOT_UART_ALT_NUM, payload.data(), payload.size());
        wrote_any = wrote_any || written == static_cast<int>(payload.size());
        if (written != static_cast<int>(payload.size())) {
            ESP_LOGW(TAG, "Alt UART write incomplete: %d/%d", written, static_cast<int>(payload.size()));
        }
    }
    if (!wrote_any) {
        ESP_LOGW(TAG, "UART write failed on all robot ports");
        return false;
    }

    ESP_LOGI(TAG, "Sent robot command: %s", payload.c_str());
    std::string ack;
    if (!WaitForAck(&ack, 2500)) {
        ESP_LOGW(TAG, "No UART ACK from servant after command");
        return false;
    }

    ESP_LOGI(TAG, "Received servant ACK: %s", ack.c_str());
    if (ack.find("\"ok\":true") == std::string::npos) {
        ESP_LOGW(TAG, "Servant returned non-OK ACK: %s", ack.c_str());
        return false;
    }
    return true;
}

bool RobotUart::WaitForAck(std::string* ack, int timeout_ms) {
    if (ack == nullptr) {
        return false;
    }
    ack->clear();

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    uint8_t byte = 0;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (primary_ready_) {
            int len = uart_read_bytes(ROBOT_UART_NUM, &byte, 1, pdMS_TO_TICKS(10));
            if (len > 0) {
                if (byte == '\n') {
                    if (ack->find("\"ok\":true") != std::string::npos ||
                        ack->find("\"ok\":false") != std::string::npos) {
                        return true;
                    }
                    ack->clear();
                    continue;
                }
                if (byte != '\r' && ack->size() < 256) {
                    ack->push_back(static_cast<char>(byte));
                }
                continue;
            }
        }

        if (alt_ready_) {
            int len = uart_read_bytes(ROBOT_UART_ALT_NUM, &byte, 1, pdMS_TO_TICKS(10));
            if (len > 0) {
                if (byte == '\n') {
                    if (ack->find("\"ok\":true") != std::string::npos ||
                        ack->find("\"ok\":false") != std::string::npos) {
                        return true;
                    }
                    ack->clear();
                    continue;
                }
                if (byte != '\r' && ack->size() < 256) {
                    ack->push_back(static_cast<char>(byte));
                }
            }
        }
    }

    return false;
}
