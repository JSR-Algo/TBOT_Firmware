#pragma once

#include <string>
#include <vector>

class RobotUart {
public:
    std::vector<std::string> calls;
    bool send_ok = true;

    void Reset() { calls.clear(); send_ok = true; }
    bool SendLeftArmRaise() { return Record("left_arm_raise"); }
    bool SendRightArmRaise() { return Record("right_arm_raise"); }
    bool SendLeftArmLower() { return Record("left_arm_lower"); }
    bool SendRightArmLower() { return Record("right_arm_lower"); }
    bool SendBothArmsRaise() { return Record("both_arms_raise"); }
    bool SendBothArmsLower() { return Record("both_arms_lower"); }
    bool SendHeadTurnLeft() { return Record("head_turn_left"); }
    bool SendHeadTurnRight() { return Record("head_turn_right"); }
    bool SendHeadCenter() { return Record("head_center"); }

private:
    bool Record(const char* call) { calls.emplace_back(call); return send_ok; }
};
