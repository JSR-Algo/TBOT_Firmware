#include <cJSON.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>

#include "lesson_asset_cache_evict.h"
#include "lesson_asset_storage_coordinator.h"
#include "lesson_storage_hil_controller.h"

std::string CallLessonStorageHilStatusForTest(int schema_version);

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string Key() {
    return "hil-task14/v1-" + std::string(kLessonAssetCacheChecksumHexBytes, 'a');
}

void ArmController() {
    LessonStorageHilController::GetInstance().Reset();
    const LessonStorageHilArmRequest request{
        Key(), LessonStorageHilOperation::kEvict,
        LessonStorageHilCheckpoint::kBeforeFirstUnlink,
        LessonStorageHilAction::kFail, 0, 0, 0};
    const auto result = LessonStorageHilController::GetInstance().Arm(request);
    Expect(result.armed, "controller arm failed");
}

std::string CallWithoutBlocking(int schema_version) {
    auto future = std::async(std::launch::async, [schema_version]() {
        return CallLessonStorageHilStatusForTest(schema_version);
    });
    const auto ready = future.wait_for(std::chrono::milliseconds(200));
    Expect(ready == std::future_status::ready, "status blocked on SD guard");
    return future.get();
}

cJSON* ParsePayload(const std::string& rendered) {
    cJSON* response = cJSON_Parse(rendered.c_str());
    Expect(response != nullptr, "status response did not parse");
    cJSON* content = cJSON_GetObjectItem(response, "content");
    cJSON* item = cJSON_GetArrayItem(content, 0);
    cJSON* text = cJSON_GetObjectItem(item, "text");
    Expect(cJSON_IsString(text), "status response text missing");
    cJSON* payload = cJSON_Parse(text->valuestring);
    cJSON_Delete(response);
    Expect(payload != nullptr, "status payload did not parse");
    return payload;
}

void ExpectControllerAndUnavailableIdentity(const std::string& rendered) {
    cJSON* payload = ParsePayload(rendered);
    Expect(cJSON_IsTrue(cJSON_GetObjectItem(payload, "armed")),
           "controller status missing under contention");
    cJSON* storage = cJSON_GetObjectItem(payload, "storageIdentity");
    Expect(cJSON_IsObject(storage), "schema v2 identity missing");
    cJSON* status = cJSON_GetObjectItem(storage, "status");
    cJSON* kind = cJSON_GetObjectItem(storage, "kind");
    Expect(cJSON_IsString(status) && std::string(status->valuestring) == "unavailable",
           "contended identity must fail closed unavailable");
    Expect(cJSON_IsString(kind) && std::string(kind->valuestring) == "sdmmc-fat",
           "contended identity kind mismatch");
    cJSON_Delete(payload);
}

void TestV1DoesNotAcquireGuardDuringMutation() {
    ArmController();
    auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
    Expect(static_cast<bool>(mutation), "mutation lease not acquired");
    const std::string rendered = CallWithoutBlocking(1);
    cJSON* payload = ParsePayload(rendered);
    Expect(cJSON_IsTrue(cJSON_GetObjectItem(payload, "armed")),
           "schema v1 lost controller status");
    Expect(cJSON_GetObjectItem(payload, "storageIdentity") == nullptr,
           "schema v1 emitted storage identity");
    cJSON_Delete(payload);
}

void TestV2ReturnsUnavailableDuringMutation() {
    ArmController();
    auto mutation = LessonAssetStorageCoordinator::GetInstance().TryBeginMutation("sync");
    Expect(static_cast<bool>(mutation), "mutation lease not acquired");
    ExpectControllerAndUnavailableIdentity(CallWithoutBlocking(2));
}

void TestV2ReturnsUnavailableDuringLessonSession() {
    ArmController();
    auto& coordinator = LessonAssetStorageCoordinator::GetInstance();
    const auto session = coordinator.TryBeginLessonSession("assignment", "session");
    Expect(session.acquired, "lesson session not acquired");
    ExpectControllerAndUnavailableIdentity(CallWithoutBlocking(2));
    Expect(coordinator.EndLessonSession(
               "assignment", "session", session.generation),
           "lesson session release failed");
}

}  // namespace

bool IsCanonicalLessonCacheKey(const std::string& value) {
    return !value.empty();
}

int main() {
    TestV1DoesNotAcquireGuardDuringMutation();
    TestV2ReturnsUnavailableDuringMutation();
    TestV2ReturnsUnavailableDuringLessonSession();
    std::cout << "Lesson storage HIL status contention tests passed\n";
    return 0;
}
