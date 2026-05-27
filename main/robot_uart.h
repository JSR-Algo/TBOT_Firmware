#ifndef ROBOT_UART_H_
#define ROBOT_UART_H_

#include <string>

class RobotUart {
public:
    bool Initialize();
    bool SendLeftArmRaise();
    bool SendServoSweep(const std::string& part, int from, int to, int step, int delay_ms);

private:
    bool WaitForAck(std::string* ack, int timeout_ms);
    bool initialized_ = false;
    bool primary_ready_ = false;
    bool alt_ready_ = false;
};

#endif  // ROBOT_UART_H_
