import os
from pathlib import Path
import subprocess

from test_m1_staging_boundaries import ROOT, body


def test_advertised_routes_rejected_before_claim_or_ota_mutation(tmp_path):
    cjson = Path(os.environ.get("CJSON_DIR", Path.home() / "esp/esp-idf/components/json/cJSON"))
    assert (cjson / "cJSON.c").is_file(), "real cJSON source is required"
    source = tmp_path / "response.cc"
    source.write_text('''#include <cassert>
#include <string>
#include "cJSON.h"
#include "m1_staging_policy.h"
#include "firmware_version_policy.h"
static int writes = 0;
struct Settings {
 Settings(const char*, bool) {}
 void SetString(const char*, const char*) { ++writes; }
 void SetInt(const char*, int) { ++writes; }
};
static bool ProcessTbotClaimConfirmationResponse(const std::string& json, bool persist, bool require_api_url = false)
''' + body("main/provisioning/claim_confirmation_reporter.cc", "static bool ProcessTbotClaimConfirmationResponse") + '''
bool IsValidCheckVersionResponse(const cJSON* root, const std::string& current_version, bool* should_download)
''' + body("main/ota.cc", "bool IsValidCheckVersionResponse") + r'''
int main() {
 const char* good = R"({"device_id":"device","device_secret":"secret","ws_url":"wss://m0-esp.tjbot.vn/tbot/v1/","api_url":"https://m0-api.tjbot.vn/v1"})";
 for (bool persist : {false,true}) {
  assert(ProcessTbotClaimConfirmationResponse(good, persist, true));
  for (const char* field : {"ws_url", "api_url"}) {
   for (const char* bad : {"https://evil.example/v1", "http://m0-api.tjbot.vn/v1", "https://m0-api.tjbot.vn.evil/v1", "https://m0-api.tjbot.vn/v1?token=secret"}) {
    auto* root = cJSON_Parse(good);
    cJSON_ReplaceItemInObject(root, field, cJSON_CreateString(bad));
    char* json = cJSON_PrintUnformatted(root);
    int before = writes;
    assert(!ProcessTbotClaimConfirmationResponse(json, persist, true));
    assert(writes == before);
    cJSON_free(json); cJSON_Delete(root);
   }
  }
 }
 const char* ota = R"({"firmware":{"version":"2.2.93","url":""},"api_url":"https://m0-api.tjbot.vn/v1","websocket":{"url":"wss://m0-esp.tjbot.vn/tbot/v1/","token":"signed-token"}})";
 for (int mutation = 0; mutation < 8; ++mutation) {
  auto* root = cJSON_Parse(ota);
  auto* ws = cJSON_GetObjectItem(root,"websocket");
  if (mutation == 1) cJSON_AddObjectToObject(root,"mqtt");
  if (mutation == 2) cJSON_AddNumberToObject(ws,"factory_test_claimed",1);
  if (mutation == 3) cJSON_ReplaceItemInObject(ws,"url",cJSON_CreateString("wss://esp.tjbot.vn/tbot/v1/"));
  if (mutation == 4) cJSON_ReplaceItemInObject(root,"api_url",cJSON_CreateString("https://old.example/v1"));
  if (mutation == 5) cJSON_ReplaceItemInObject(ws,"token",cJSON_CreateString(""));
  if (mutation == 6) {
   auto* firmware=cJSON_GetObjectItem(root,"firmware");
   cJSON_ReplaceItemInObject(firmware,"version",cJSON_CreateString("2.2.94"));
   cJSON_ReplaceItemInObject(firmware,"url",cJSON_CreateString("https://m0-esp.tjbot.vn/new.bin"));
  }
  if (mutation == 7) cJSON_AddBoolToObject(root,"factory_test_claimed",true);
  bool download = false;
  assert(IsValidCheckVersionResponse(root,"2.2.93",&download) == (mutation == 0));
  cJSON_Delete(root);
 }
}
''')
    obj = tmp_path / "cjson.o"
    subprocess.run(["clang", "-fsanitize=address,undefined", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    binary = tmp_path / "response"
    subprocess.run(["clang++", "-std=c++17", "-fsanitize=address,undefined", "-DCONFIG_TBOT_M1_STAGING=1", "-I", str(ROOT / "main"), "-I", str(cjson), str(source), str(obj), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
