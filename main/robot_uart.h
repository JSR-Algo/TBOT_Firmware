#ifndef ROBOT_UART_H_
#define ROBOT_UART_H_

#include <driver/gpio.h>
#include <driver/uart.h>

#include <string>

class RobotUart {
public:
    bool Initialize();
    bool SendLeftArmRaise();
    bool SendRightArmRaise();
    bool SendLeftArmLower();
    bool SendRightArmLower();
    bool SendBothArmsRaise();
    bool SendBothArmsLower();
    bool SendLeftArmSetPercent(int percent);
    bool SendRightArmSetPercent(int percent);
    bool SendBothArmsSetPercent(int percent);
    bool SendHeadTurnLeft();
    bool SendHeadTurnRight();
    bool SendHeadCenter();
    bool SendHeadSetAngle(int angle);
    bool SendHeadSetPercent(int percent);
    bool SendServoSweep(const std::string& part, const std::string& action, int from, int to, int step, int delay_ms);

private:
    bool SendArmAction(const std::string& part, const std::string& action);
    bool SendPayloadOnProfile(const char* profile_name, uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin,
        bool ready, const std::string& payload, int timeout_ms);
    bool SelectUartProfile(const char* profile_name, uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin);
    bool WaitForAck(uart_port_t port, std::string* ack, int timeout_ms);
    bool initialized_ = false;
    bool primary_ready_ = false;
    bool alt_ready_ = false;
    bool using_alt_profile_ = false;
};

#endif  // ROBOT_UART_H_
