#include "chat_protocol_signals.h"
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
enum class PowerSaveLevel { PERFORMANCE };
struct Board { unsigned saves=0; void SetPowerSaveLevel(PowerSaveLevel) { ++saves; } };
struct Codec { int output_sample_rate() { return 24000; } };
struct Protocol {
    std::function<void()> connected,opened;
    void OnConnected(std::function<void()> fn) { connected=std::move(fn); }
    void OnAudioChannelOpened(std::function<void()> fn) { opened=std::move(fn); }
    int server_sample_rate() { return 16000; }
};
struct Application {
    std::unique_ptr<Protocol> protocol_=std::make_unique<Protocol>();
    std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_=std::make_shared<ChatProtocolSignals>();
    std::atomic<uint64_t> protocol_generation_{1};
    std::atomic<uint32_t> protocol_callback_connect_generation_{1},connect_generation_{1};
    std::atomic<bool> chat_protocol_owned_{false},online_intent_{false},backend_offline_{true};
    std::atomic<bool> passive_ws_intent_{false},lesson_runtime_active_{false};
    std::atomic<bool> lesson_interactive_listen_pending_{false},lesson_interactive_listening_active_{false};
    struct { unsigned resets=0; void Reset() { ++resets; } } backend_recovery_window_;
    unsigned starts=0,stops=0,dispatches=0,claim_stops=0;
    bool suppressed=false;
    Board board_;
    Codec codec_;
    std::deque<std::function<void()>> tasks;
    void Schedule(std::function<void()> fn) { tasks.push_back(std::move(fn)); }
    void Once() { auto fn=std::move(tasks.front());tasks.pop_front();fn(); }
    bool IsConnectSuccessPublicationSuppressed() { return suppressed; }
    bool IsDeviceClaimed() { return true; }
    void DismissAlert() {}
    void StopHeartbeat() { ++stops; }
    void StartHeartbeat() { ++starts; }
    void DispatchDeviceHeartbeat() { ++dispatches; }
    void StopClaimPoll() { ++claim_stops; }
    void Install() {
        const auto callback_protocol_generation=protocol_generation_.load();
        const auto callback_signals=chat_protocol_signals_;
        auto* callback_protocol=protocol_.get();
        auto* codec=&codec_;
        auto& board=board_;
        // PRODUCTION_CALLBACKS
    }
};
int main() {
    Application healthy;
    healthy.Install(); healthy.protocol_->connected(); healthy.Once();
    assert(healthy.starts==1 && !healthy.backend_offline_);
    healthy.protocol_->opened(); healthy.Once();
    assert(healthy.starts==2 && healthy.board_.saves==1);
    healthy.Once(); assert(healthy.claim_stops==1);

    Application old;
    old.Install(); old.protocol_->connected(); old.protocol_->opened();
    old.chat_protocol_signals_->Disable(); old.chat_protocol_owned_=true;
    old.protocol_.reset(); old.Once(); old.Once();
    assert(old.starts==0 && old.board_.saves==0);

    Application nested;
    nested.Install(); nested.protocol_->opened(); nested.Once();
    nested.chat_protocol_signals_->Disable(); ++nested.connect_generation_;
    nested.Once(); assert(nested.claim_stops==0);

    Application stale_intent;
    stale_intent.Install(); stale_intent.protocol_->opened();
    ++stale_intent.connect_generation_; stale_intent.Once();
    assert(stale_intent.starts==0 && stale_intent.board_.saves==0);
}
