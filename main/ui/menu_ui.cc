#include "ui/menu_ui.h"

#include <cstdio>
#include <cstdlib>

#include <esp_log.h>
#include <cJSON.h>

#include "game/flappy_assets.h"   // WavLoadMono16

#define TAG "MenuUI"

namespace {

constexpr char kConfigPath[] = "/sdcard/tbot_ui/menu_config.json";

char* ReadWholeFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024) {
        fclose(f);
        return nullptr;
    }
    char* buf = static_cast<char*>(malloc(static_cast<size_t>(sz) + 1));
    if (buf == nullptr) {
        fclose(f);
        return nullptr;
    }
    size_t rd = fread(buf, 1, static_cast<size_t>(sz), f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

// Gan chuoi tu vi[key] neu la string khong rong.
void GetStr(const cJSON* vi, const char* key, std::string* dst) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(vi, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring[0] != '\0') {
        *dst = item->valuestring;
    }
}

}  // namespace

MenuStrings MenuLoadStrings() {
    MenuStrings s;  // default hard-code tieng Viet khong dau

    char* text = ReadWholeFile(kConfigPath);
    if (text == nullptr) {
        ESP_LOGW(TAG, "Khong co %s -> dung text mac dinh", kConfigPath);
        return s;
    }
    cJSON* root = cJSON_Parse(text);
    free(text);
    if (root == nullptr) {
        ESP_LOGW(TAG, "menu_config.json parse loi -> dung text mac dinh");
        return s;
    }

    const cJSON* vi = cJSON_GetObjectItemCaseSensitive(root, "vi");
    if (cJSON_IsObject(vi)) {
        GetStr(vi, "title", &s.title);
        GetStr(vi, "subtitle", &s.subtitle);
        GetStr(vi, "chatbox", &s.chatbox);
        GetStr(vi, "game", &s.game);
        GetStr(vi, "music", &s.music);
        GetStr(vi, "music_empty", &s.music_empty);
        GetStr(vi, "left_right_hint", &s.left_right_hint);
        GetStr(vi, "both_hint", &s.both_hint);
        GetStr(vi, "hold_hint", &s.hold_hint);
        GetStr(vi, "slave_ok", &s.slave_ok);
        GetStr(vi, "slave_wait", &s.slave_wait);
        GetStr(vi, "sd_ok", &s.sd_ok);
        GetStr(vi, "sd_fail", &s.sd_fail);
        s.from_sd = true;
        ESP_LOGI(TAG, "Menu strings loaded tu SD (vi)");
    } else {
        ESP_LOGW(TAG, "menu_config.json thieu object 'vi' -> dung text mac dinh");
    }
    cJSON_Delete(root);
    return s;
}

void MenuLoadSounds(MenuSounds* s) {
    WavLoadMono16("/sdcard/tbot_ui/sfx/menu_move.wav", &s->move);
    WavLoadMono16("/sdcard/tbot_ui/sfx/menu_select.wav", &s->select);
    WavLoadMono16("/sdcard/tbot_ui/sfx/menu_back.wav", &s->back);
    WavLoadMono16("/sdcard/tbot_ui/sfx/open_chatbox.wav", &s->open_chatbox);
    WavLoadMono16("/sdcard/tbot_ui/sfx/open_game.wav", &s->open_game);
    WavLoadMono16("/sdcard/tbot_ui/sfx/menu_error.wav", &s->error);
    s->loaded = true;
    ESP_LOGI(TAG, "Menu sounds: move=%u select=%u back=%u open_chat=%u open_game=%u error=%u",
             (unsigned)s->move.size(), (unsigned)s->select.size(), (unsigned)s->back.size(),
             (unsigned)s->open_chatbox.size(), (unsigned)s->open_game.size(),
             (unsigned)s->error.size());
}
