#pragma once

#include <string>
#include <vector>

class RobotUart {
public:
    std::vector<std::string> calls;
    bool send_ok = true;
    bool head_ok = true;
    bool left_ok = true;
    bool right_ok = true;

    void Reset() { calls.clear(); send_ok = head_ok = left_ok = right_ok = true; }
    bool SendLeftArmRaise() { return Record("left_arm_raise"); }
    bool SendRightArmRaise() { return Record("right_arm_raise"); }
    bool SendLeftArmLower() { return Record("left_arm_lower"); }
    bool SendRightArmLower() { return Record("right_arm_lower"); }
    bool SendBothArmsRaise() { return Record("both_arms_raise"); }
    bool SendBothArmsLower() { return Record("both_arms_lower"); }
    bool SendHeadTurnLeft() { return Record("head_turn_left"); }
    bool SendHeadTurnRight() { return Record("head_turn_right"); }
    bool SendHeadCenter() { return Record("head_center"); }
    bool SendHeadSetPercent(int percent) {
        calls.emplace_back("head_percent:" + std::to_string(percent));
        return send_ok && head_ok;
    }
    bool SendLeftArmSetPercent(int percent) {
        calls.emplace_back("left_percent:" + std::to_string(percent));
        return send_ok && left_ok;
    }
    bool SendRightArmSetPercent(int percent) {
        calls.emplace_back("right_percent:" + std::to_string(percent));
        return send_ok && right_ok;
    }

private:
    bool Record(const char* call) { calls.emplace_back(call); return send_ok; }
};
