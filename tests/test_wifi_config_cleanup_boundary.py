"""Execute the board's pre-BLE boundary with delayed application cleanup."""
from pathlib import Path
import subprocess

from test_protocol_work_lifetime import method


ROOT = Path(__file__).resolve().parents[1]


def test_reconnected_station_drains_and_rolls_back_before_ble(tmp_path):
    source = (ROOT / "main/boards/common/wifi_board.cc").read_text()
    body = method(source, "WifiBoard::WifiConfigEntryResult WifiBoard::StartWifiConfigMode")
    # All paths under test return before the platform BLE reservation boundary.
    boundary = body[:body.index("#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING")]
    fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct WifiManager {
    bool connected=false;
    static WifiManager& GetInstance(){static WifiManager value;return value;}
    bool IsConnected(){return connected;}
    bool IsConfigMode(){return false;}
};
struct Application {
    struct WifiConfigEntryPreparation { bool valid=false; };
    bool pending=false,ready=false;
    unsigned prepares=0,rollbacks=0;
    static Application& GetInstance(){static Application value;return value;}
    bool IsWifiConfigEntryPending(){return pending;}
    bool IsLessonRuntimeActive(){return false;}
    bool PrepareWifiConfigEntry(WifiConfigEntryPreparation& result){
        ++prepares;pending=true;
        if(!ready)return false;
        pending=false;result.valid=true;return true;
    }
    bool RollbackWifiConfigEntry(const WifiConfigEntryPreparation& value){
        assert(value.valid);++rollbacks;return true;
    }
};
struct WifiBoard {
    enum class WifiConfigEntryResult{kStarted,kCancelled,kRetry};
    bool in_config_mode_=false;
    std::atomic<uint32_t> wifi_recovery_generation_{1};
    unsigned ble_starts=0;
    void ArmWifiConfigIntentRetry(){}
    WifiConfigEntryResult StartWifiConfigMode(bool,bool,uint32_t);
};
''' + boundary + r'''
    ++ble_starts;return WifiConfigEntryResult::kStarted;
}
int main(){
    WifiBoard board;auto& app=Application::GetInstance();auto& wifi=WifiManager::GetInstance();
    using Result=WifiBoard::WifiConfigEntryResult;
    assert(board.StartWifiConfigMode(false,true,1)==Result::kRetry);
    wifi.connected=true;
    assert(board.StartWifiConfigMode(false,true,1)==Result::kRetry);
    assert(app.pending && app.rollbacks==0 && board.ble_starts==0);
    app.ready=true;
    assert(board.StartWifiConfigMode(false,true,1)==Result::kCancelled);
    assert(!app.pending && app.rollbacks==1 && board.ble_starts==0);
    assert(board.StartWifiConfigMode(true,false,1)==Result::kStarted);
    assert(board.ble_starts==1 && !app.pending);
}
'''
    generated = tmp_path / "cleanup.cc"
    generated.write_text(fixture)
    binary = tmp_path / "cleanup"
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_application_tick_resumes_pending_board_intent():
    source = (ROOT / "main/application.cc").read_text()
    tick = source[source.index("        if (bits & MAIN_EVENT_CLOCK_TICK) {"):]
    poll = tick.index("PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK)")
    pending = tick.index("if (wifi_config_preparation_.valid)", poll)
    resume = tick.index("ResumePendingWifiConfigMode()", pending)
    assert poll < pending < resume < tick.index("display->UpdateStatusBar()", poll)
