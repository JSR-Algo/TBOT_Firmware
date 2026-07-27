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
#include <functional>
#include <string>
#include <vector>

enum class LessonVisualApplyResult {
    kApplied,
    kDegraded,
    kRejected,
    kPhaseTimeout,
};

enum class LessonVisualStateKind {
    kTeach,
    kListen,
    kThinking,
    kCorrect,
    kNearMiss,
    kIncorrect,
    kRetry,
    kCelebrate,
    kCompletion,
    kReveal,
};

struct LessonRobotEntrancePlan {
    const char* layout_preset = nullptr;
    bool reduced_motion = false;
};

struct LessonVisualState {
    LessonVisualStateKind kind = LessonVisualStateKind::kTeach;
    bool overlay_available = false;
};

using LessonVisualCompletion =
    std::function<void(LessonVisualApplyResult result, const char* degraded_reason)>;

class LvglDisplay : public Display {
public:
    // true entry == a real image was set; false == cleared (nullptr).
    std::vector<bool> background_calls;
    std::vector<bool> object_calls;
    std::vector<bool> overlay_calls;
    std::vector<int> overlay_bounds;
    std::vector<std::string> teaching_word_calls;
    // true == hide the realtime emoji face (lesson_start); false == restore it
    // (lesson_stop/lesson_error). Lets a test prove the smiley is suppressed for the
    // whole lesson rather than only occluded by overlapping image layers.
    std::vector<bool> lesson_mode_calls;
    int entrance_start_calls = 0;
    int entrance_cancel_calls = 0;
    int visual_state_calls = 0;
    std::string last_entrance_layout;
    LessonVisualStateKind last_visual_state = LessonVisualStateKind::kTeach;
    LessonVisualCompletion pending_entrance_completion;
    LessonVisualCompletion pending_visual_state_completion;

    virtual void SetLessonBackground(std::unique_ptr<LvglImage> image) {
        background_calls.push_back(image != nullptr);
    }
    virtual void SetLessonObject(std::unique_ptr<LvglImage> image) {
        object_calls.push_back(image != nullptr);
    }
    virtual void SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) {
        overlay_calls.push_back(image != nullptr);
    }
    virtual void SetLessonRobotOverlayBounds(int left, int top, int width, int height) {
        overlay_bounds = {left, top, width, height};
    }
    virtual void SetLessonTeachingWord(const char* text) {
        teaching_word_calls.emplace_back(text ? text : "");
    }
    virtual void SetLessonMode(bool active) {
        lesson_mode_calls.push_back(active);
    }
    virtual bool StartLessonRobotEntrance(
        const LessonRobotEntrancePlan& plan, LessonVisualCompletion completion) {
        entrance_start_calls++;
        last_entrance_layout = plan.layout_preset ? plan.layout_preset : "";
        pending_entrance_completion = std::move(completion);
        return true;
    }
    virtual void CancelLessonRobotEntrance() {
        entrance_cancel_calls++;
        pending_entrance_completion = nullptr;
        pending_visual_state_completion = nullptr;
    }
    virtual bool ApplyLessonVisualState(
        const LessonVisualState& state, LessonVisualCompletion completion) {
        visual_state_calls++;
        last_visual_state = state.kind;
        pending_visual_state_completion = std::move(completion);
        return true;
    }
    void CompleteEntrance(LessonVisualApplyResult result = LessonVisualApplyResult::kApplied,
                          const char* reason = nullptr) {
        auto completion = std::move(pending_entrance_completion);
        pending_entrance_completion = nullptr;
        if (completion) completion(result, reason);
    }
    void CompleteVisualState(LessonVisualApplyResult result = LessonVisualApplyResult::kApplied,
                             const char* reason = nullptr) {
        auto completion = std::move(pending_visual_state_completion);
        pending_visual_state_completion = nullptr;
        if (completion) completion(result, reason);
    }
};
