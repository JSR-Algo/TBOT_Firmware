#include "game/flappy_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <esp_log.h>
#include <cJSON.h>

#define TAG "FlappyCfg"

namespace {

constexpr char kConfigPath[]   = "/sdcard/tbot_games/flappy/game_config.json";
constexpr char kManifestPath[] = "/sdcard/tbot_games/manifest.json";

// Doc toan bo file text nho (<=64KB) vao buffer NUL-terminated. Caller free().
// Tra ve nullptr neu khong mo/doc duoc (SD chua mount, file khong co, qua lon).
char* ReadWholeFile(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
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
    if (out_len != nullptr) {
        *out_len = rd;
    }
    return buf;
}

// Lay so tu object JSON neu la number; nguoc lai giu default.
void GetNumber(const cJSON* root, const char* key, double* dst) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        *dst = item->valuedouble;
    }
}

int Clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Doc header BMP (BITMAPFILEHEADER + BITMAPINFOHEADER) de bao cao WxH/bpp.
// Tra ve true neu la BMP hop le; dien width/height/bpp.
bool ProbeBmp(const char* path, int32_t* w, int32_t* h, int16_t* bpp) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    uint8_t hdr[30];
    size_t rd = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (rd < sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M') {
        return false;
    }
    // Little-endian: biWidth@18, biHeight@22 (int32), biBitCount@28 (int16).
    *w   = static_cast<int32_t>(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24));
    *h   = static_cast<int32_t>(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
    *bpp = static_cast<int16_t>(hdr[28] | (hdr[29] << 8));
    return true;
}

}  // namespace

FlappyConfig FlappyConfigLoad() {
    FlappyConfig cfg;  // default

    size_t len = 0;
    char* text = ReadWholeFile(kConfigPath, &len);
    if (text == nullptr) {
        ESP_LOGW(TAG, "No SD config (%s) -> dung default primitive "
                      "(gravity=%.2f jump=%.2f pipe_speed=%d gap=%d fps=%d)",
                 kConfigPath, cfg.gravity, cfg.jump_velocity, cfg.pipe_speed,
                 cfg.gap_size, cfg.fps);
        return cfg;
    }

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Config JSON parse failed -> dung default");
        return cfg;
    }

    double gravity = cfg.gravity, jump = cfg.jump_velocity;
    double pipe_speed = cfg.pipe_speed, gap = cfg.gap_size, fps = cfg.fps;
    GetNumber(root, "gravity", &gravity);
    GetNumber(root, "jump_velocity", &jump);
    GetNumber(root, "pipe_speed", &pipe_speed);
    GetNumber(root, "gap_size", &gap);
    GetNumber(root, "fps", &fps);

    const cJSON* ua = cJSON_GetObjectItemCaseSensitive(root, "use_assets");
    if (cJSON_IsBool(ua)) {
        cfg.use_assets = cJSON_IsTrue(ua);
    }

    cfg.gravity = static_cast<float>(gravity);
    cfg.jump_velocity = static_cast<float>(jump);
    cfg.pipe_speed = Clampi(static_cast<int>(pipe_speed), 1, 12);
    cfg.gap_size = Clampi(static_cast<int>(gap), 60, 220);
    cfg.fps = Clampi(static_cast<int>(fps), 10, 60);
    cfg.from_sd = true;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "SD config loaded: gravity=%.2f jump=%.2f pipe_speed=%d gap=%d "
                  "fps=%d use_assets=%d",
             cfg.gravity, cfg.jump_velocity, cfg.pipe_speed, cfg.gap_size,
             cfg.fps, cfg.use_assets);
    return cfg;
}

void FlappyAssetsProbeAndLog() {
    // Manifest (chi bao cao co/khong).
    size_t mlen = 0;
    char* man = ReadWholeFile(kManifestPath, &mlen);
    if (man != nullptr) {
        ESP_LOGI(TAG, "ASSET REPORT: manifest.json OK (%u bytes)", (unsigned)mlen);
        free(man);
    } else {
        ESP_LOGW(TAG, "ASSET REPORT: manifest.json MISSING (%s)", kManifestPath);
    }

    struct { const char* name; const char* path; } assets[] = {
        { "bg",          "/sdcard/tbot_games/flappy/bg.bmp" },
        { "ground",      "/sdcard/tbot_games/flappy/ground.bmp" },
        { "bird",        "/sdcard/tbot_games/flappy/bird.bmp" },
        { "pipe_top",    "/sdcard/tbot_games/flappy/pipe_top.bmp" },
        { "pipe_bottom", "/sdcard/tbot_games/flappy/pipe_bottom.bmp" },
    };

    int ok = 0;
    for (auto& a : assets) {
        int32_t w = 0, h = 0;
        int16_t bpp = 0;
        if (ProbeBmp(a.path, &w, &h, &bpp)) {
            ESP_LOGI(TAG, "ASSET REPORT: %-11s OK  %ldx%ld %dbpp  (render se lam sau)",
                     a.name, (long)w, (long)h, bpp);
            ok++;
        } else {
            ESP_LOGW(TAG, "ASSET REPORT: %-11s MISSING -> fallback primitive", a.name);
        }
    }
    ESP_LOGI(TAG, "ASSET REPORT: %d/%d sprite san sang; game dang chay primitive "
                  "(decode BMP se bat o buoc sau)", ok, (int)(sizeof(assets)/sizeof(assets[0])));
}
