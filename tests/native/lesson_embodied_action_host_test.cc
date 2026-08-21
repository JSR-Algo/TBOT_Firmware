#include "lesson_embodied_action.h"
#include <cJSON.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "lesson embodied action host test FAILED: " << message << "\n";
        std::exit(1);
    }
}

cJSON* Parse(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    Check(root != nullptr, "test JSON parses");
    return root;
}

std::string ActionFrame(
    const char* intent = "PRESENT_LEFT",
    const char* focus = "focus.left.choice",
    const char* generation = "1",
    const char* extra_envelope = "",
    const char* extra_body = "") {
    return std::string("{") +
        "\"type\":\"lesson_embodied_action\"," +
        "\"assignmentId\":\"assignment-1\"," +
        "\"sessionId\":\"session-1\"," +
        "\"stepId\":\"cat-meaning-left-right-01\"," +
        "\"sequence\":17," +
        "\"body\":{" +
            "\"actionId\":\"session-1:course-decision-1\"," +
            "\"actionGeneration\":" + generation + "," +
            "\"intent\":\"" + intent + "\"," +
            "\"visualFocusRegion\":\"" + focus + "\"," +
            "\"listenWindowPolicy\":\"complete_before_listening\"" + extra_body +
        "}" + extra_envelope + "}";
}

LessonEmbodiedParseContext Context() {
    return {"assignment-1", "session-1", "cat-meaning-left-right-01", 0, false};
}

void TestAcceptedIntentAndPresetContract() {
    struct Expected {
        const char* intent;
        const char* focus;
        LessonEmbodiedIntent parsed;
    } expected[] = {
        {"REST_WARM", "focus.center.primary", LessonEmbodiedIntent::kRestWarm},
        {"GREET_SMALL", "focus.center.primary", LessonEmbodiedIntent::kGreetSmall},
        {"INVITE_CHILD", "focus.center.primary", LessonEmbodiedIntent::kInviteChild},
        {"PRESENT_CENTER", "focus.center.primary", LessonEmbodiedIntent::kPresentCenter},
        {"PRESENT_LEFT", "focus.left.choice", LessonEmbodiedIntent::kPresentLeft},
        {"PRESENT_RIGHT", "focus.right.choice", LessonEmbodiedIntent::kPresentRight},
        {"LISTEN_STILL", "focus.center.primary", LessonEmbodiedIntent::kListenStill},
        {"THINK_CURIOUS", "focus.center.primary", LessonEmbodiedIntent::kThinkCurious},
        {"ACKNOWLEDGE_STORY", "focus.center.primary", LessonEmbodiedIntent::kAcknowledgeStory},
        {"MODEL_WORD", "focus.center.primary", LessonEmbodiedIntent::kModelWord},
        {"ENCOURAGE_SMALL", "focus.center.primary", LessonEmbodiedIntent::kEncourageSmall},
        {"TRY_DIFFERENT_WAY", "focus.center.primary", LessonEmbodiedIntent::kTryDifferentWay},
        {"CELEBRATE_RECALL", "focus.center.primary", LessonEmbodiedIntent::kCelebrateRecall},
        {"CELEBRATE_MASTERY", "focus.center.primary", LessonEmbodiedIntent::kCelebrateMastery},
        {"COMFORT_CALM", "focus.center.primary", LessonEmbodiedIntent::kComfortCalm},
        {"PAUSE_CHOICE", "focus.center.primary", LessonEmbodiedIntent::kPauseChoice},
        {"GOODBYE_SMALL", "focus.center.primary", LessonEmbodiedIntent::kGoodbyeSmall},
    };

    for (const auto& item : expected) {
        cJSON* root = Parse(ActionFrame(item.intent, item.focus));
        LessonEmbodiedAction action;
        LessonEmbodiedParseError error = LessonEmbodiedParseError::kMalformedEnvelope;
        Check(ParseLessonEmbodiedActionFrame(root, Context(), &action, &error),
              "frozen intent parses");
        Check(error == LessonEmbodiedParseError::kNone, "successful parse clears error");
        Check(action.intent == item.parsed, "wire intent maps to exact enum");
        Check(action.visual_focus_region ==
                  (std::string(item.focus) == "focus.left.choice"
                       ? LessonVisualFocusRegion::kLeftChoice
                       : std::string(item.focus) == "focus.right.choice"
                           ? LessonVisualFocusRegion::kRightChoice
                           : LessonVisualFocusRegion::kCenterPrimary),
              "authored focus maps exactly");
        const auto& preset = ResolveLessonEmbodiedPreset(action.intent);
        Check(preset.head_percent >= 0 && preset.head_percent <= 100,
              "trusted head percentage is bounded");
        Check(preset.left_arm_percent >= 0 && preset.left_arm_percent <= 100,
              "trusted left arm percentage is bounded");
        Check(preset.right_arm_percent >= 0 && preset.right_arm_percent <= 100,
              "trusted right arm percentage is bounded");
        Check(std::string(LessonEmbodiedIntentWireName(action.intent)) == item.intent,
              "enum maps back to frozen wire name");
        cJSON_Delete(root);
    }
}

void ExpectActionRejected(
    const std::string& json,
    LessonEmbodiedParseError expected_error,
    LessonEmbodiedParseContext context = Context()) {
    cJSON* root = Parse(json);
    LessonEmbodiedAction action;
    LessonEmbodiedParseError error = LessonEmbodiedParseError::kNone;
    Check(!ParseLessonEmbodiedActionFrame(root, context, &action, &error),
          "invalid action frame is rejected");
    Check(error == expected_error, "invalid action reports expected category");
    cJSON_Delete(root);
}

void TestClosedSchemaAndValidation() {
    ExpectActionRejected(ActionFrame("WAVE_CUSTOM", "focus.center.primary"),
                         LessonEmbodiedParseError::kUnknownIntent);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.center.primary"),
                         LessonEmbodiedParseError::kInvalidFocusRegion);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "true"),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1.5"),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1", "", ",\"angle\":90"),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1", "", ",\"speed\":5"),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1", "", ",\"servo\":\"head\""),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1", "", ",\"joint\":\"arm\""),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1", "", ",\"percentage\":20"),
                         LessonEmbodiedParseError::kMalformedBody);
    ExpectActionRejected(ActionFrame("PRESENT_LEFT", "focus.left.choice", "1", ",\"extra\":true"),
                         LessonEmbodiedParseError::kMalformedEnvelope);

    std::string malformed_policy = ActionFrame();
    const std::string valid_policy = "complete_before_listening";
    malformed_policy.replace(malformed_policy.find(valid_policy), valid_policy.size(), "overlap_listening");
    ExpectActionRejected(malformed_policy, LessonEmbodiedParseError::kInvalidListenWindowPolicy);

    auto stale = Context();
    stale.last_action_generation = 1;
    ExpectActionRejected(ActionFrame(), LessonEmbodiedParseError::kStaleGeneration, stale);
    auto assessment = Context();
    assessment.assessment_window_open = true;
    ExpectActionRejected(ActionFrame(), LessonEmbodiedParseError::kAssessmentWindowOpen, assessment);
    auto mismatch = Context();
    mismatch.session_id = "other-session";
    ExpectActionRejected(ActionFrame(), LessonEmbodiedParseError::kContextMismatch, mismatch);

    ExpectActionRejected(
        "{\"type\":\"lesson_embodied_action\",\"assignmentId\":\"assignment-1\","
        "\"sessionId\":\"session-1\",\"stepId\":\"cat-meaning-left-right-01\","
        "\"sequence\":17,\"body\":{\"actionId\":\"\",\"actionGeneration\":1,"
        "\"intent\":\"PRESENT_LEFT\",\"visualFocusRegion\":\"focus.left.choice\","
        "\"listenWindowPolicy\":\"complete_before_listening\"}}",
        LessonEmbodiedParseError::kMalformedBody);
}

void TestCancelContract() {
    const std::string valid =
        "{\"type\":\"lesson_embodied_cancel\",\"assignmentId\":\"assignment-1\","
        "\"sessionId\":\"session-1\",\"stepId\":\"cat-meaning-left-right-01\","
        "\"sequence\":18,\"body\":{\"actionId\":\"session-1:course-decision-1\","
        "\"actionGeneration\":1}}";
    cJSON* root = Parse(valid);
    LessonEmbodiedCancel cancel;
    LessonEmbodiedParseError error = LessonEmbodiedParseError::kMalformedEnvelope;
    Check(ParseLessonEmbodiedCancelFrame(root, Context(), &cancel, &error),
          "frozen cancel frame parses");
    Check(cancel.sequence == 18 && cancel.action_generation == 1,
          "cancel identity fields remain exact integers");
    cJSON_Delete(root);

    root = Parse(valid);
    cJSON_AddNumberToObject(cJSON_GetObjectItem(root, "body"), "angle", 90);
    Check(!ParseLessonEmbodiedCancelFrame(root, Context(), &cancel, &error),
          "cancel rejects raw servo fields");
    cJSON_Delete(root);

    root = Parse(valid);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(root, "body"), "actionGeneration",
                              cJSON_CreateBool(true));
    Check(!ParseLessonEmbodiedCancelFrame(root, Context(), &cancel, &error),
          "cancel rejects boolean generation");
    cJSON_Delete(root);
}

void TestRuntimeAuthorityAndMotionFailure() {
    LessonRuntimeToken active{7};
    Check(IsLessonRuntimeTokenAuthorized(true, 7, active), "active exact token authorizes");
    Check(!IsLessonRuntimeTokenAuthorized(false, 7, active), "inactive runtime rejects token");
    Check(!IsLessonRuntimeTokenAuthorized(true, 8, active), "stale token is rejected");
    Check(!IsLessonRuntimeTokenAuthorized(true, 0, LessonRuntimeToken{}),
          "zero token is never valid");
    Check(NextLessonRuntimeGeneration(7, false, true) == 8,
          "lesson start rotates runtime authority");
    Check(NextLessonRuntimeGeneration(8, true, false) == 9,
          "lesson stop rotates runtime authority");
    Check(NextLessonRuntimeGeneration(9, true, true) == 9,
          "repeated active call does not rotate authority");
    Check(NextLessonRuntimeGeneration(9, false, false) == 9,
          "repeated inactive call does not rotate authority");

    std::vector<std::string> calls;
    int fail_call = 0;
    auto command = [&](const char* part) {
        return [&, part](int percent) {
            calls.emplace_back(std::string(part) + ":" + std::to_string(percent));
            return fail_call == 0 || static_cast<int>(calls.size()) != fail_call;
        };
    };
    const auto& preset = ResolveLessonEmbodiedPreset(LessonEmbodiedIntent::kPresentLeft);
    Check(ApplyLessonEmbodiedPresetCommands(
              preset, command("head"), command("left"), command("right")) ==
              LessonEmbodiedMotionResult::kApplied,
          "all servo commands apply");
    Check(calls.size() == 3 && calls[0] == "head:25" &&
              calls[1] == "left:45" && calls[2] == "right:0",
          "preset percentages resolve only inside firmware");

    calls.clear();
    fail_call = 2;
    Check(ApplyLessonEmbodiedPresetCommands(
              preset, command("head"), command("left"), command("right")) ==
              LessonEmbodiedMotionResult::kDegraded,
          "one-servo failure degrades the action");
    Check(calls.size() == 3, "partial failure still commands every safe target");

    calls.clear();
    fail_call = 0;
    Check(ApplyLessonEmbodiedPresetCommands(
              ResolveLessonEmbodiedPreset(LessonEmbodiedIntent::kRestWarm),
              command("head"), command("left"), command("right")) ==
              LessonEmbodiedMotionResult::kApplied,
          "rest pose applies");
    Check(calls.size() == 3 && calls[0] == "head:50" &&
              calls[1] == "left:0" && calls[2] == "right:0",
          "rest centers head and lowers both arms");
}

}  // namespace

int main() {
    TestAcceptedIntentAndPresetContract();
    TestClosedSchemaAndValidation();
    TestCancelContract();
    TestRuntimeAuthorityAndMotionFailure();
    std::cout << "lesson embodied action host test OK (" << checks << " checks)\n";
    return 0;
}
