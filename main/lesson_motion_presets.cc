#include "lesson_motion_presets.h"

#include "robot_uart.h"

#include <cstdint>
#include <cstring>

#include <esp_timer.h>

namespace {

constexpr uint64_t kBoundedPresetDurationUs = 1500ULL * 1000ULL;

class LessonMotionController {
public:
    LessonMotionResult Dispatch(RobotUart& robot_uart, const char* preset) {
        if (!IsKnownPreset(preset)) return LessonMotionResult::kDegraded;

        robot_uart_ = &robot_uart;
        ++generation_;
        if (rest_timer_ != nullptr) esp_timer_stop(rest_timer_);

        bool sent = true;
        bool bounded = true;
        if (std::strcmp(preset, "rest") == 0) {
            bounded = false;
            sent = SendRest();
        } else if (std::strcmp(preset, "teach") == 0) {
            sent = robot_uart.SendRightArmRaise();
            sent = robot_uart.SendHeadCenter() && sent;
        } else if (std::strcmp(preset, "presentLeft") == 0) {
            sent = robot_uart.SendLeftArmRaise();
            sent = robot_uart.SendRightArmLower() && sent;
            sent = robot_uart.SendHeadTurnLeft() && sent;
        } else if (std::strcmp(preset, "presentRight") == 0) {
            sent = robot_uart.SendRightArmRaise();
            sent = robot_uart.SendLeftArmLower() && sent;
            sent = robot_uart.SendHeadTurnRight() && sent;
        } else if (std::strcmp(preset, "listen") == 0) {
            sent = SendRest();
        } else if (std::strcmp(preset, "thinking") == 0) {
            sent = robot_uart.SendLeftArmRaise();
            sent = robot_uart.SendRightArmLower() && sent;
            sent = robot_uart.SendHeadTurnLeft() && sent;
        } else if (std::strcmp(preset, "encourage") == 0) {
            sent = robot_uart.SendBothArmsRaise();
            sent = robot_uart.SendHeadCenter() && sent;
        } else if (std::strcmp(preset, "tryAgain") == 0) {
            sent = robot_uart.SendBothArmsLower();
            sent = robot_uart.SendHeadTurnRight() && sent;
        } else if (std::strcmp(preset, "celebrate") == 0) {
            sent = robot_uart.SendBothArmsRaise();
            sent = robot_uart.SendHeadCenter() && sent;
        } else if (std::strcmp(preset, "goodbye") == 0) {
            sent = robot_uart.SendRightArmRaise();
            sent = robot_uart.SendLeftArmLower() && sent;
            sent = robot_uart.SendHeadTurnRight() && sent;
        }

        if (bounded && !ScheduleRest()) sent = false;
        return sent ? LessonMotionResult::kApplied : LessonMotionResult::kDegraded;
    }

private:
    static bool IsKnownPreset(const char* preset) {
        if (preset == nullptr) return false;
        return std::strcmp(preset, "rest") == 0 || std::strcmp(preset, "teach") == 0 ||
               std::strcmp(preset, "presentLeft") == 0 ||
               std::strcmp(preset, "presentRight") == 0 ||
               std::strcmp(preset, "listen") == 0 ||
               std::strcmp(preset, "thinking") == 0 ||
               std::strcmp(preset, "encourage") == 0 ||
               std::strcmp(preset, "tryAgain") == 0 ||
               std::strcmp(preset, "celebrate") == 0 ||
               std::strcmp(preset, "goodbye") == 0;
    }

    static void RestTimerCallback(void* arg) {
        auto* self = static_cast<LessonMotionController*>(arg);
        const uint32_t callback_generation = self->armed_generation_;
        self->RestIfCurrent(callback_generation);
    }

    void RestIfCurrent(uint32_t callback_generation) {
        if (callback_generation != generation_ || robot_uart_ == nullptr) return;
        SendRest();
    }

    bool SendRest() {
        bool sent = robot_uart_->SendBothArmsLower();
        return robot_uart_->SendHeadCenter() && sent;
    }

    bool ScheduleRest() {
        if (rest_timer_ == nullptr) {
            const esp_timer_create_args_t args = {
                .callback = RestTimerCallback,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "lesson_motion_rest",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &rest_timer_) != ESP_OK) return false;
        }
        armed_generation_ = generation_;
        return esp_timer_start_once(rest_timer_, kBoundedPresetDurationUs) == ESP_OK;
    }

    RobotUart* robot_uart_ = nullptr;
    esp_timer_handle_t rest_timer_ = nullptr;
    uint32_t generation_ = 0;
    uint32_t armed_generation_ = 0;
};

LessonMotionController g_lesson_motion_controller;

}  // namespace

LessonMotionResult DispatchLessonMotionPreset(RobotUart& robot_uart, const char* preset) {
    return g_lesson_motion_controller.Dispatch(robot_uart, preset);
}
