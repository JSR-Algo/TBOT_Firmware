from pathlib import Path
import subprocess

import pytest

from test_tbot_claim_confirmation_contract import function_body

ROOT = Path(__file__).resolve().parents[1]


def body(path, signature):
    source = (ROOT / path).read_text()
    if path == "main/main.cc":
        source = source[source.rindex(signature):]
    return function_body(source, signature) + "}"


def test_staging_ota_cannot_fall_back_to_saved_or_hosted_url(tmp_path):
    source = tmp_path / "ota.cc"
    source.write_text('''#include <algorithm>
#include <cassert>
#include <string>
#include <vector>
#include "m1_staging_policy.h"
#define CONFIG_OTA_URL "https://esp.tjbot.vn/tbot/ota/"
#define ESP_LOGW(...) ((void)0)
bool IsEphemeralEndpoint(const std::string&) { return false; }
bool IsStaleConfiguredEndpoint(const std::string&, const std::string&) { return false; }
std::vector<std::string> BuildCheckVersionUrls(const std::string& configured_url)
''' + body("main/ota.cc", "std::vector<std::string> BuildCheckVersionUrls") + '''
int main() {
 for (const auto* saved : {"https://old.example/ota", "https://m0-esp.tjbot.vn/tbot/ota/", ""}) {
  const auto urls = BuildCheckVersionUrls(saved);
  assert(urls.size() == 1 && urls.front() == "https://m0-esp.tjbot.vn/tbot/ota/");
 }
}
''')
    binary = tmp_path / "ota"
    subprocess.run(["clang++", "-std=c++17", "-DCONFIG_TBOT_M1_STAGING=1", "-I", str(ROOT / "main"), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


@pytest.mark.parametrize("path,signature,forbidden", [
    ("main/main.cc", 'extern "C" void app_main', "nvs_flash_erase("),
    ("components/esp-wifi-connect/wifi_manager.cc", "bool WifiManager::Initialize", "nvs_flash_erase("),
    ("main/boards/common/system_reset.cc", "void SystemReset::ResetNvsFlash", "nvs_flash_erase("),
    ("main/boards/common/system_reset.cc", "void SystemReset::ResetToFactory", "esp_partition_erase_range("),
    ("main/application.cc", "void Application::HandleHeartbeatAuthFailure", "ForceClearAndCancelTransaction("),
    ("main/application.cc", "void Application::EnterRepairPairingMode", "ForceClearAndCancelTransaction("),
    ("main/application.cc", "bool Application::UpgradeFirmware", "SetDeviceState("),
    ("main/ota.cc", "bool Ota::Upgrade", "esp_ota_begin("),
    ("main/assets.cc", "bool Assets::Download", "esp_partition_erase_range("),
    ("main/application.cc", "void Application::CheckAssetsVersion", 'GetString("download_url")'),
])
def test_staging_preserves_shared_storage(path, signature, forbidden):
    expanded = subprocess.run(["clang++", "-E", "-P", "-x", "c++", "-DCONFIG_TBOT_M1_STAGING=1", "-"],
                              input=body(path, signature), text=True, capture_output=True, check=True).stdout
    assert forbidden not in expanded


@pytest.mark.parametrize("path,namespace", [
    ("main/lesson_asset_retained_selection.cc", "lesson_select"),
    ("main/lesson_handler.cc", "lesson_course"),
])
def test_raw_backend_state_does_not_use_original_namespace(path, namespace):
    assert f'nvs_open("{namespace}"' not in (ROOT / path).read_text()
