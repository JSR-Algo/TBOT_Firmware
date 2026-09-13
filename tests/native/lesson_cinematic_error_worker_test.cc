#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include "lesson_handler.h"

#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(value) (value)
constexpr const char* TAG = "Application";
template <typename... Args> void TestLog(const char*, const char*, Args...) {}
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_LOGW(...) TestLog(__VA_ARGS__)

struct ChatContext { std::uint64_t lesson_epoch = 1; };
using ChatRequestContext = std::shared_ptr<ChatContext>;
struct cJSON { const char* valuestring = "frame"; int valueint = 1; };
cJSON* cJSON_Parse(const char*) { return nullptr; }
const cJSON* cJSON_GetObjectItem(const cJSON*, const char*) { return nullptr; }
bool cJSON_IsString(const cJSON*) { return false; }
bool cJSON_IsNumber(const cJSON*) { return false; }
void cJSON_Delete(cJSON*) {}
void cJSON_free(void* memory) { std::free(memory); }
class Protocol {};
class RobotUart {};
struct LessonAssetStorageCoordinator {
    static LessonAssetStorageCoordinator& GetInstance() {
        static LessonAssetStorageCoordinator coordinator; return coordinator;
    }
    void ForceEndLessonSession() {}
};
struct Application {
    std::atomic<bool> lesson_message_stop_{false}, lesson_message_retired_{false}, chat_protocol_owned_{false};
    std::atomic<std::uint32_t> lesson_protocol_readers_{0};
    LessonTransportTerminalControl lesson_terminal_control_;
    LessonTransportEpochGate lesson_transport_epoch_gate_;
    LessonQueueDataAdmission lesson_queue_data_admission_{16};
    void* lesson_message_queue_ = this;
    std::unique_ptr<Protocol> protocol_{new Protocol};
    RobotUart robot_uart_;
    static void LessonMessageTask(void*);
    void Schedule(std::function<void()> fn) { fn(); }
    bool AbandonLessonStorageSession() { return true; }
    bool IsChatLessonRequestCurrent(const ChatRequestContext&) { return true; }
    void HandleLessonMessage(const cJSON*, ChatRequestContext) { assert(false); }
    void FailChatRequest(const ChatRequestContext&) { assert(false); }
};
Application* active = nullptr;
std::uint64_t pending_epoch = 1;
unsigned sends = 0, receives = 0, invalidations = 0, ordinary_completions = 0, dispatches = 0;
std::string scenario;
void LogLessonWorkerStackWatermark(const char*) {}
void LogLessonHeapBoundary(const char*, std::size_t) {}
void CancelLessonRobotEntranceOnDisplay() {}
void vTaskDelay(unsigned) {}
void vTaskSuspend(void*) { assert(active->lesson_protocol_readers_ == 0); }
void SetLessonTransportEpoch(std::uint64_t) {}
void InvalidateLessonVisualCompletionState(std::uint64_t epoch) {
    assert(epoch == 2); ++invalidations; pending_epoch = 0;
}
std::uint64_t PendingLessonCinematicErrorEpoch() { return pending_epoch; }
bool DispatchPendingLessonCinematicError(Protocol* protocol) {
    assert(protocol == active->protocol_.get());
    assert(active->lesson_protocol_readers_ == 1);
    assert(!active->chat_protocol_owned_);
    assert(active->lesson_transport_epoch_gate_.WorkerAcceptFrame(pending_epoch));
    ++dispatches;
    if (dispatches == 1 && scenario == "exception_retry") throw std::bad_alloc();
    if (dispatches == 1 && scenario == "unknown_exception_retry") throw 7;
    ++sends; pending_epoch = 0; return true;
}
bool DispatchLessonVisualCompletion(const LessonQueueItem&, Protocol*, RobotUart*) {
    ++ordinary_completions; pending_epoch = 1; return true;
}
bool DispatchLessonEmbodiedCompletion(const LessonQueueItem&, Protocol*) { assert(false); return false; }
int xQueueReceive(void*, LessonQueueItem* item, std::uint32_t timeout) {
    assert(timeout == 100);
    assert(active->lesson_protocol_readers_ == 0);
    ++receives;
    if ((scenario == "exception_retry" || scenario == "unknown_exception_retry") && receives == 1) return 0;
    if (scenario == "after_timeout" && receives == 1) { pending_epoch = 1; return 0; }
    if (scenario == "after_data" && receives == 1) {
        assert(active->lesson_queue_data_admission_.TryAcquire());
        *item = {}; item->kind = LessonQueueItemKind::kVisualCompleted; item->transport_epoch = 1;
        return pdTRUE;
    }
    active->lesson_message_stop_ = true;
    return 0;
}

// PRODUCTION_WORKER

int main(int argc, char** argv) {
    assert(argc == 2); scenario = argv[1];
    Application app; active = &app;
    if (scenario == "after_timeout" || scenario == "after_data") pending_epoch = 0;
    if (scenario == "owned") app.chat_protocol_owned_ = true;
    if (scenario == "stale_epoch") pending_epoch = 2;
    if (scenario == "terminal") {
        const auto epoch = app.lesson_transport_epoch_gate_.PublishTerminalEpoch();
        app.lesson_terminal_control_.Publish(epoch);
    }
    Application::LessonMessageTask(&app);
    assert(app.lesson_message_retired_ && app.lesson_protocol_readers_ == 0);
    if (scenario == "idle" || scenario == "after_timeout" || scenario == "after_data" ||
        scenario == "exception_retry" || scenario == "unknown_exception_retry") assert(sends == 1);
    else assert(sends == 0);
    if (scenario == "terminal") assert(invalidations == 1);
    if (scenario == "after_data") assert(ordinary_completions == 1);
    if (scenario == "after_timeout" || scenario == "after_data") assert(receives == 2);
    if (scenario == "exception_retry" || scenario == "unknown_exception_retry")
        assert(dispatches == 2 && receives == 2);
}
