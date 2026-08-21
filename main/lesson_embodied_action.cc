#include "lesson_embodied_action.h"

#include <cJSON.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

constexpr double kMaxJsonSafeInteger = 9007199254740991.0;
constexpr std::size_t kMaxIdentityBytes = 128;

struct IntentContract {
    const char* wire_name;
    LessonEmbodiedIntent intent;
    LessonVisualFocusRegion focus;
    LessonEmbodiedPreset preset;
};

constexpr std::array<IntentContract, 17> kIntentContracts{{
    {"REST_WARM", LessonEmbodiedIntent::kRestWarm, LessonVisualFocusRegion::kCenterPrimary,
     {"relaxed", 50, 0, 0, 0, 350, false}},
    {"GREET_SMALL", LessonEmbodiedIntent::kGreetSmall, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 50, 0, 35, 900, 350, true}},
    {"INVITE_CHILD", LessonEmbodiedIntent::kInviteChild, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 50, 20, 20, 800, 350, true}},
    {"PRESENT_CENTER", LessonEmbodiedIntent::kPresentCenter, LessonVisualFocusRegion::kCenterPrimary,
     {"neutral", 50, 20, 20, 900, 400, true}},
    {"PRESENT_LEFT", LessonEmbodiedIntent::kPresentLeft, LessonVisualFocusRegion::kLeftChoice,
     {"neutral", 25, 45, 0, 900, 400, true}},
    {"PRESENT_RIGHT", LessonEmbodiedIntent::kPresentRight, LessonVisualFocusRegion::kRightChoice,
     {"neutral", 75, 0, 45, 900, 400, true}},
    {"LISTEN_STILL", LessonEmbodiedIntent::kListenStill, LessonVisualFocusRegion::kCenterPrimary,
     {"relaxed", 50, 0, 0, 0, 500, false}},
    {"THINK_CURIOUS", LessonEmbodiedIntent::kThinkCurious, LessonVisualFocusRegion::kCenterPrimary,
     {"thinking", 35, 20, 0, 750, 400, true}},
    {"ACKNOWLEDGE_STORY", LessonEmbodiedIntent::kAcknowledgeStory, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 50, 15, 15, 650, 350, true}},
    {"MODEL_WORD", LessonEmbodiedIntent::kModelWord, LessonVisualFocusRegion::kCenterPrimary,
     {"neutral", 50, 15, 15, 750, 400, true}},
    {"ENCOURAGE_SMALL", LessonEmbodiedIntent::kEncourageSmall, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 50, 25, 25, 700, 350, true}},
    {"TRY_DIFFERENT_WAY", LessonEmbodiedIntent::kTryDifferentWay, LessonVisualFocusRegion::kCenterPrimary,
     {"thinking", 65, 10, 10, 700, 400, true}},
    {"CELEBRATE_RECALL", LessonEmbodiedIntent::kCelebrateRecall, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 50, 35, 35, 850, 400, true}},
    {"CELEBRATE_MASTERY", LessonEmbodiedIntent::kCelebrateMastery, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 50, 60, 60, 1000, 450, true}},
    {"COMFORT_CALM", LessonEmbodiedIntent::kComfortCalm, LessonVisualFocusRegion::kCenterPrimary,
     {"relaxed", 50, 10, 10, 650, 400, true}},
    {"PAUSE_CHOICE", LessonEmbodiedIntent::kPauseChoice, LessonVisualFocusRegion::kCenterPrimary,
     {"relaxed", 50, 0, 0, 0, 450, false}},
    {"GOODBYE_SMALL", LessonEmbodiedIntent::kGoodbyeSmall, LessonVisualFocusRegion::kCenterPrimary,
     {"happy", 65, 0, 35, 900, 400, true}},
}};

void SetError(LessonEmbodiedParseError* error, LessonEmbodiedParseError value) {
    if (error != nullptr) *error = value;
}

const cJSON* Field(const cJSON* object, const char* key) {
    return object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, key);
}

const char* StringField(const cJSON* object, const char* key) {
    const cJSON* value = Field(object, key);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

template <std::size_t N>
bool HasExactFields(const cJSON* object, const std::array<std::string_view, N>& expected) {
    if (!cJSON_IsObject(object)) return false;
    std::array<bool, N> seen{};
    std::size_t count = 0;
    for (const cJSON* item = object->child; item != nullptr; item = item->next) {
        if (item->string == nullptr) return false;
        std::size_t match = N;
        for (std::size_t index = 0; index < N; ++index) {
            if (expected[index] == item->string) {
                match = index;
                break;
            }
        }
        if (match == N || seen[match]) return false;
        seen[match] = true;
        ++count;
    }
    return count == N;
}

bool ExactPositiveInteger(const cJSON* object, const char* key, std::uint64_t* output) {
    const cJSON* value = Field(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble <= 0 || value->valuedouble > kMaxJsonSafeInteger ||
        std::floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    if (output != nullptr) *output = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}

bool ExactNonNegativeInteger(const cJSON* object, const char* key, std::uint64_t* output) {
    const cJSON* value = Field(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > kMaxJsonSafeInteger ||
        std::floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    if (output != nullptr) *output = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}

bool ValidIdentity(const char* value) {
    if (value == nullptr) return false;
    const std::size_t length = std::strlen(value);
    if (length == 0 || length > kMaxIdentityBytes) return false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x21 || byte > 0x7e) return false;
    }
    return true;
}

bool MatchesContext(const char* value, const char* expected) {
    return ValidIdentity(value) && ValidIdentity(expected) && std::strcmp(value, expected) == 0;
}

const IntentContract* FindIntent(const char* wire_name) {
    if (wire_name == nullptr) return nullptr;
    for (const auto& contract : kIntentContracts) {
        if (std::strcmp(contract.wire_name, wire_name) == 0) return &contract;
    }
    return nullptr;
}

const IntentContract& FindIntent(LessonEmbodiedIntent intent) {
    for (const auto& contract : kIntentContracts) {
        if (contract.intent == intent) return contract;
    }
    return kIntentContracts.front();
}

bool ParseEnvelope(
    const cJSON* root,
    const LessonEmbodiedParseContext& context,
    const char* expected_type,
    std::uint64_t* sequence,
    LessonEmbodiedParseError* error) {
    constexpr std::array<std::string_view, 6> kEnvelopeFields = {
        "type", "assignmentId", "sessionId", "stepId", "sequence", "body",
    };
    if (!HasExactFields(root, kEnvelopeFields) ||
        !cJSON_IsObject(Field(root, "body")) ||
        StringField(root, "type") == nullptr ||
        std::strcmp(StringField(root, "type"), expected_type) != 0 ||
        !ExactNonNegativeInteger(root, "sequence", sequence)) {
        SetError(error, LessonEmbodiedParseError::kMalformedEnvelope);
        return false;
    }
    if (!MatchesContext(StringField(root, "assignmentId"), context.assignment_id) ||
        !MatchesContext(StringField(root, "sessionId"), context.session_id) ||
        !MatchesContext(StringField(root, "stepId"), context.step_id)) {
        SetError(error, LessonEmbodiedParseError::kContextMismatch);
        return false;
    }
    return true;
}

}  // namespace

bool ParseLessonEmbodiedActionFrame(
    const cJSON* root,
    const LessonEmbodiedParseContext& context,
    LessonEmbodiedAction* output,
    LessonEmbodiedParseError* error) {
    if (output == nullptr) {
        SetError(error, LessonEmbodiedParseError::kMalformedBody);
        return false;
    }
    if (context.assessment_window_open) {
        SetError(error, LessonEmbodiedParseError::kAssessmentWindowOpen);
        return false;
    }

    std::uint64_t sequence = 0;
    if (!ParseEnvelope(root, context, "lesson_embodied_action", &sequence, error)) return false;

    constexpr std::array<std::string_view, 5> kBodyFields = {
        "actionId", "actionGeneration", "intent", "visualFocusRegion", "listenWindowPolicy",
    };
    const cJSON* body = Field(root, "body");
    std::uint64_t generation = 0;
    const char* action_id = StringField(body, "actionId");
    if (!HasExactFields(body, kBodyFields) || !ValidIdentity(action_id) ||
        !ExactPositiveInteger(body, "actionGeneration", &generation)) {
        SetError(error, LessonEmbodiedParseError::kMalformedBody);
        return false;
    }
    if (generation <= context.last_action_generation) {
        SetError(error, LessonEmbodiedParseError::kStaleGeneration);
        return false;
    }

    const IntentContract* intent = FindIntent(StringField(body, "intent"));
    if (intent == nullptr) {
        SetError(error, LessonEmbodiedParseError::kUnknownIntent);
        return false;
    }
    const char* focus = StringField(body, "visualFocusRegion");
    if (focus == nullptr || std::strcmp(focus, LessonVisualFocusRegionWireName(intent->focus)) != 0) {
        SetError(error, LessonEmbodiedParseError::kInvalidFocusRegion);
        return false;
    }
    const char* policy = StringField(body, "listenWindowPolicy");
    if (policy == nullptr || std::strcmp(policy, "complete_before_listening") != 0) {
        SetError(error, LessonEmbodiedParseError::kInvalidListenWindowPolicy);
        return false;
    }

    output->assignment_id = StringField(root, "assignmentId");
    output->session_id = StringField(root, "sessionId");
    output->step_id = StringField(root, "stepId");
    output->sequence = sequence;
    output->action_id = action_id;
    output->action_generation = generation;
    output->intent = intent->intent;
    output->visual_focus_region = intent->focus;
    output->listen_window_policy = LessonListenWindowPolicy::kCompleteBeforeListening;
    SetError(error, LessonEmbodiedParseError::kNone);
    return true;
}

bool ParseLessonEmbodiedCancelFrame(
    const cJSON* root,
    const LessonEmbodiedParseContext& context,
    LessonEmbodiedCancel* output,
    LessonEmbodiedParseError* error) {
    if (output == nullptr) {
        SetError(error, LessonEmbodiedParseError::kMalformedBody);
        return false;
    }
    std::uint64_t sequence = 0;
    if (!ParseEnvelope(root, context, "lesson_embodied_cancel", &sequence, error)) return false;

    constexpr std::array<std::string_view, 2> kBodyFields = {
        "actionId", "actionGeneration",
    };
    const cJSON* body = Field(root, "body");
    std::uint64_t generation = 0;
    const char* action_id = StringField(body, "actionId");
    if (!HasExactFields(body, kBodyFields) || !ValidIdentity(action_id) ||
        !ExactPositiveInteger(body, "actionGeneration", &generation)) {
        SetError(error, LessonEmbodiedParseError::kMalformedBody);
        return false;
    }

    output->assignment_id = StringField(root, "assignmentId");
    output->session_id = StringField(root, "sessionId");
    output->step_id = StringField(root, "stepId");
    output->sequence = sequence;
    output->action_id = action_id;
    output->action_generation = generation;
    SetError(error, LessonEmbodiedParseError::kNone);
    return true;
}

const char* LessonEmbodiedIntentWireName(LessonEmbodiedIntent intent) {
    return FindIntent(intent).wire_name;
}

const char* LessonVisualFocusRegionWireName(LessonVisualFocusRegion region) {
    switch (region) {
    case LessonVisualFocusRegion::kLeftChoice: return "focus.left.choice";
    case LessonVisualFocusRegion::kRightChoice: return "focus.right.choice";
    case LessonVisualFocusRegion::kCenterPrimary: return "focus.center.primary";
    }
    return "focus.center.primary";
}

const LessonEmbodiedPreset& ResolveLessonEmbodiedPreset(LessonEmbodiedIntent intent) {
    return FindIntent(intent).preset;
}

bool IsLessonRuntimeTokenAuthorized(
    bool lesson_runtime_active,
    std::uint64_t active_runtime_generation,
    const LessonRuntimeToken& token) {
    return lesson_runtime_active && active_runtime_generation != 0 &&
           token.runtime_generation == active_runtime_generation;
}

std::uint64_t NextLessonRuntimeGeneration(
    std::uint64_t current_generation,
    bool was_active,
    bool will_be_active) {
    if (was_active == will_be_active) return current_generation;
    if (current_generation == std::numeric_limits<std::uint64_t>::max()) return 1;
    return current_generation + 1;
}

LessonEmbodiedMotionResult ApplyLessonEmbodiedPresetCommands(
    const LessonEmbodiedPreset& preset,
    const LessonEmbodiedServoCommand& set_head_percent,
    const LessonEmbodiedServoCommand& set_left_arm_percent,
    const LessonEmbodiedServoCommand& set_right_arm_percent) {
    if (!set_head_percent || !set_left_arm_percent || !set_right_arm_percent) {
        return LessonEmbodiedMotionResult::kDegraded;
    }
    const bool head_ok = set_head_percent(preset.head_percent);
    const bool left_ok = set_left_arm_percent(preset.left_arm_percent);
    const bool right_ok = set_right_arm_percent(preset.right_arm_percent);
    return head_ok && left_ok && right_ok
        ? LessonEmbodiedMotionResult::kApplied
        : LessonEmbodiedMotionResult::kDegraded;
}
