"""Verify the actual station config enables SDK same-SSID AP failover."""
from pathlib import Path
import subprocess

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def test_station_config_preserves_credentials_and_allows_ap_failover(tmp_path):
    source = (ROOT / "components/esp-wifi-connect/wifi_station.cc").read_text()
    body = method(source, "std::string WifiStation::StartConnectForSession")
    fixture = r'''
#include <cassert>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include "wifi_credential_limits.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define WIFI_IF_STA 0
enum { WIFI_FAST_SCAN, WIFI_ALL_CHANNEL_SCAN };
enum { WIFI_CONNECT_AP_BY_SIGNAL, WIFI_CONNECT_AP_BY_SECURITY };
struct wifi_config_t {
 struct {
  uint8_t ssid[32], password[64], bssid[6];
  bool bssid_set; uint8_t channel; int listen_interval;
  int scan_method, sort_method; uint8_t failure_retry_cnt;
 } sta;
};
wifi_config_t captured{};
int connects=0;
int esp_wifi_set_config(int, const wifi_config_t* config){captured=*config;return 0;}
int esp_wifi_connect(){++connects;return 0;}
struct WifiApRecord {
 std::string ssid,password; uint8_t channel; int authmode; uint8_t bssid[6];
};
struct WifiStation {
 std::mutex session_data_mutex_;
 std::string password_,ssid_;
 int reconnect_count_=0;bool remember_bssid_=false;
 wifi_config_t active_station_config_{};bool active_station_config_valid_=false;
 std::string StartConnectForSession(WifiApRecord,bool=true);
};
''' + body + r'''
int main(){
 WifiStation station;
 WifiApRecord record{"fixture-network","fixture-passphrase",6,0,{1,2,3,4,5,6}};
 for(bool remember:{false,true})for(bool use_remembered:{false,true}){
  station.remember_bssid_=remember;
  assert(station.StartConnectForSession(record,use_remembered)==record.ssid);
  assert(std::string(reinterpret_cast<char*>(captured.sta.ssid))==record.ssid);
  assert(std::string(reinterpret_cast<char*>(captured.sta.password))==record.password);
  assert(captured.sta.bssid_set==(remember&&use_remembered));
  if(remember&&use_remembered){
   assert(captured.sta.channel==6);
   assert(std::memcmp(captured.sta.bssid,record.bssid,6)==0);
  }else{
   assert(captured.sta.scan_method==WIFI_ALL_CHANNEL_SCAN);
   assert(captured.sta.sort_method==WIFI_CONNECT_AP_BY_SIGNAL);
   assert(captured.sta.failure_retry_cnt>=1&&captured.sta.failure_retry_cnt<=2);
   assert(captured.sta.channel==0);
  }
  assert(station.active_station_config_valid_);
  assert(std::memcmp(&captured,&station.active_station_config_,sizeof(captured))==0);
 }
 assert(connects==4);
 record.password=std::string(64,'x');
 assert(station.StartConnectForSession(record).empty());
 assert(connects==4);
}
'''
    generated = tmp_path / "ap_selection.cc"
    generated.write_text(fixture)
    binary = tmp_path / "ap_selection"
    subprocess.run([
        "c++", "-std=c++20", "-fsanitize=address,undefined",
        "-I", str(ROOT / "components/esp-wifi-connect/include"),
        str(generated), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
