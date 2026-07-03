#pragma once
// Host stub for main/display/lvgl_display/lvgl_display.h. Records each lesson-layer
// draw so a test can assert the renderer scheduled the right image (or a clear=nullptr)
// onto the LVGL task. We accept ownership of the unique_ptr exactly like the real
// setters so no leak escapes the recorded draw.
//
// The REAL on-screen draw (pixel pipeline) only runs on ESP32-S3 hardware (CP-7). A
// host fake can prove the lambda was scheduled with the right pointer but cannot
// exercise the real LVGL object tree — documented unreachable boundary.
#include "display.h"
#include "lvgl_image.h"

#include <memory>
#include <vector>

class LvglDisplay : public Display {
public:
    // true entry == a real image was set; false == cleared (nullptr).
    std::vector<bool> background_calls;
    std::vector<bool> object_calls;
    std::vector<bool> overlay_calls;
    // true == hide the realtime emoji face (lesson_start); false == restore it
    // (lesson_stop/lesson_error). Lets a test prove the smiley is suppressed for the
    // whole lesson rather than only occluded by overlapping image layers.
    std::vector<bool> lesson_mode_calls;

    virtual void SetLessonBackground(std::unique_ptr<LvglImage> image) {
        background_calls.push_back(image != nullptr);
    }
    virtual void SetLessonObject(std::unique_ptr<LvglImage> image) {
        object_calls.push_back(image != nullptr);
    }
    virtual void SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) {
        overlay_calls.push_back(image != nullptr);
    }
    virtual void SetLessonMode(bool active) {
        lesson_mode_calls.push_back(active);
    }
};
