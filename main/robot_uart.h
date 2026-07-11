#ifndef ROBOT_UART_H_
#define ROBOT_UART_H_

#include <driver/gpio.h>
#include <driver/uart.h>

#include <functional>
#include <string>

// Su kien nhap tru tuong tu slave (2 nut TTP223) gui qua UART.
enum class RobotInputEvent {
    SlaveReady,
    LeftClick,
    RightClick,
    BothClick,
    MenuHold,    // giu ca 2 nut 3s
    RightHold,   // giu RIGHT 3s -> doi Wi-Fi (giu claim)
};

class RobotUart {
public:
    bool Initialize();

    // Dang ky callback nhan su kien nut tu slave. Callback chay trong task doc UART:
    // phai nhe (chi marshal sang main task, khong dung LVGL truc tiep).
    void SetEventCallback(std::function<void(RobotInputEvent)> cb) { event_cb_ = std::move(cb); }

    // Gui mot dong dieu khien text toi slave (vd "MODE:GAME"). Best-effort, khong
    // dung duong gui servo JSON.
    bool SendControlLine(const std::string& line);

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
        bool ready, const std::string& payload);
    bool SelectUartProfile(const char* profile_name, uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin);
    void StartAckReader();
    static void AckReaderTask(void* arg);
    void HandleReaderLine(const char* line);
    bool initialized_ = false;
    bool primary_ready_ = false;
    bool alt_ready_ = false;
    bool using_alt_profile_ = false;
    bool ack_reader_started_ = false;
    std::function<void(RobotInputEvent)> event_cb_;
};

#endif  // ROBOT_UART_H_
