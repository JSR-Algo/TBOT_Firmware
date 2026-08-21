#ifndef LESSON_EMBODIED_ACTION_H_
#define LESSON_EMBODIED_ACTION_H_

#include <cstdint>
#include <functional>
#include <string>

struct cJSON;

enum class LessonEmbodiedIntent {
    kRestWarm,
    kGreetSmall,
    kInviteChild,
    kPresentCenter,
    kPresentLeft,
    kPresentRight,
    kListenStill,
    kThinkCurious,
    kAcknowledgeStory,
    kModelWord,
    kEncourageSmall,
    kTryDifferentWay,
    kCelebrateRecall,
    kCelebrateMastery,
    kComfortCalm,
    kPauseChoice,
    kGoodbyeSmall,
};

enum class LessonVisualFocusRegion {
    kCenterPrimary,
    kLeftChoice,
    kRightChoice,
};

enum class LessonListenWindowPolicy {
    kCompleteBeforeListening,
};

struct LessonEmbodiedPreset {
    const char* face;
    int head_percent;
    int left_arm_percent;
    int right_arm_percent;
    std::uint32_t hold_ms;
    std::uint32_t settle_before_listen_ms;
    bool return_to_rest;
};

struct LessonEmbodiedAction {
    std::string assignment_id;
    std::string session_id;
    std::string step_id;
    std::uint64_t sequence = 0;
    std::string action_id;
    std::uint64_t action_generation = 0;
    LessonEmbodiedIntent intent = LessonEmbodiedIntent::kRestWarm;
    LessonVisualFocusRegion visual_focus_region = LessonVisualFocusRegion::kCenterPrimary;
    LessonListenWindowPolicy listen_window_policy =
        LessonListenWindowPolicy::kCompleteBeforeListening;
};

struct LessonEmbodiedCancel {
    std::string assignment_id;
    std::string session_id;
    std::string step_id;
    std::uint64_t sequence = 0;
    std::string action_id;
    std::uint64_t action_generation = 0;
};

struct LessonEmbodiedParseContext {
    const char* assignment_id = nullptr;
    const char* session_id = nullptr;
    const char* step_id = nullptr;
    std::uint64_t last_action_generation = 0;
    bool assessment_window_open = false;
};

enum class LessonEmbodiedParseError {
    kNone,
    kMalformedEnvelope,
    kContextMismatch,
    kMalformedBody,
    kUnknownIntent,
    kInvalidFocusRegion,
    kInvalidListenWindowPolicy,
    kStaleGeneration,
    kAssessmentWindowOpen,
};

struct LessonRuntimeToken {
    std::uint64_t runtime_generation = 0;
};

enum class LessonEmbodiedMotionResult {
    kApplied,
    kDegraded,
    kRejected,
};

using LessonEmbodiedServoCommand = std::function<bool(int)>;

bool ParseLessonEmbodiedActionFrame(
    const cJSON* root,
    const LessonEmbodiedParseContext& context,
    LessonEmbodiedAction* output,
    LessonEmbodiedParseError* error = nullptr);
bool ParseLessonEmbodiedCancelFrame(
    const cJSON* root,
    const LessonEmbodiedParseContext& context,
    LessonEmbodiedCancel* output,
    LessonEmbodiedParseError* error = nullptr);

const char* LessonEmbodiedIntentWireName(LessonEmbodiedIntent intent);
const char* LessonVisualFocusRegionWireName(LessonVisualFocusRegion region);
const LessonEmbodiedPreset& ResolveLessonEmbodiedPreset(LessonEmbodiedIntent intent);
bool IsLessonRuntimeTokenAuthorized(
    bool lesson_runtime_active,
    std::uint64_t active_runtime_generation,
    const LessonRuntimeToken& token);
std::uint64_t NextLessonRuntimeGeneration(
    std::uint64_t current_generation,
    bool was_active,
    bool will_be_active);
LessonEmbodiedMotionResult ApplyLessonEmbodiedPresetCommands(
    const LessonEmbodiedPreset& preset,
    const LessonEmbodiedServoCommand& set_head_percent,
    const LessonEmbodiedServoCommand& set_left_arm_percent,
    const LessonEmbodiedServoCommand& set_right_arm_percent);

#endif  // LESSON_EMBODIED_ACTION_H_
