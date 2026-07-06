#include "game/flappy_assets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "FlappyAsset"

namespace {

constexpr char kDir[] = "/sdcard/tbot_games/flappy/";

uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t Rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Decode BMP 24-bit khong nen -> lv_img_dsc_t ARGB8888 (buffer PSRAM).
// magenta_key: pixel RGB(255,0,255) -> alpha 0. Tra ve nullptr neu that bai.
lv_img_dsc_t* LoadBmpArgb(const char* name, bool magenta_key) {
    char path[128];
    snprintf(path, sizeof(path), "%s%s", kDir, name);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "%s: khong mo duoc", name);
        return nullptr;
    }

    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M') {
        fclose(f);
        return nullptr;
    }
    uint32_t data_off = Rd32(h + 10);
    int32_t  w  = (int32_t)Rd32(h + 18);
    int32_t  hh = (int32_t)Rd32(h + 22);
    uint16_t bpp  = Rd16(h + 28);
    uint32_t comp = Rd32(h + 30);

    bool top_down = hh < 0;
    int height = top_down ? -hh : hh;
    if (bpp != 24 || comp != 0 || w <= 0 || w > 1024 || height <= 0 || height > 1024) {
        ESP_LOGW(TAG, "%s: BMP khong ho tro (bpp=%u comp=%lu %ldx%d)",
                 name, bpp, (unsigned long)comp, (long)w, height);
        fclose(f);
        return nullptr;
    }

    size_t px = (size_t)w * height;
    uint8_t* argb = (uint8_t*)heap_caps_malloc(px * 4, MALLOC_CAP_SPIRAM);
    if (argb == nullptr) {
        ESP_LOGE(TAG, "%s: het PSRAM cho %zu bytes", name, px * 4);
        fclose(f);
        return nullptr;
    }

    int row_bytes = ((w * 3 + 3) / 4) * 4;  // hang BMP padding 4-byte
    uint8_t* row = (uint8_t*)malloc(row_bytes);
    if (row == nullptr) {
        heap_caps_free(argb);
        fclose(f);
        return nullptr;
    }

    fseek(f, data_off, SEEK_SET);
    bool ok = true;
    for (int r = 0; r < height && ok; ++r) {
        if (fread(row, 1, row_bytes, f) != (size_t)row_bytes) {
            ok = false;
            break;
        }
        int dst_y = top_down ? r : (height - 1 - r);  // BMP bottom-up -> lat
        uint8_t* dst = argb + (size_t)dst_y * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t rr = row[x * 3 + 2];
            uint8_t a = 0xFF;
            if (magenta_key && rr == 255 && g == 0 && b == 255) {
                a = 0x00;
            }
            // LVGL ARGB8888 luu little-endian: byte order B,G,R,A
            dst[x * 4 + 0] = b;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = rr;
            dst[x * 4 + 3] = a;
        }
    }
    free(row);
    fclose(f);

    if (!ok) {
        heap_caps_free(argb);
        ESP_LOGW(TAG, "%s: doc pixel loi", name);
        return nullptr;
    }

    lv_img_dsc_t* d = (lv_img_dsc_t*)calloc(1, sizeof(lv_img_dsc_t));
    if (d == nullptr) {
        heap_caps_free(argb);
        return nullptr;
    }
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = w;
    d->header.h = height;
    d->header.stride = w * 4;
    d->data = argb;
    d->data_size = px * 4;
    ESP_LOGI(TAG, "%s: sprite %ldx%d ARGB (magenta_key=%d)", name, (long)w, height, magenta_key);
    return d;
}

void FreeDsc(lv_img_dsc_t** d) {
    if (*d == nullptr) {
        return;
    }
    if ((*d)->data != nullptr) {
        heap_caps_free((void*)(*d)->data);
    }
    free(*d);
    *d = nullptr;
}

// Nap WAV tu sfx cua flappy theo ten file.
bool LoadWav(const char* name, std::vector<int16_t>* out) {
    char path[160];
    snprintf(path, sizeof(path), "%ssfx/%s", kDir, name);
    return WavLoadMono16(path, out);
}

}  // namespace

// Nap WAV PCM 16-bit mono. Bo qua chunk khong phai 'data'/'fmt '. Downmix stereo
// don gian (lay kenh trai). Tra ve false neu khong phai 16-bit hoac loi.
bool WavLoadMono16(const char* path, std::vector<int16_t>* out) {
    out->clear();
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }

    uint16_t channels = 1, bits = 16;
    bool got_data = false;
    while (true) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = Rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            uint32_t take = sz < 16 ? sz : 16;
            if (fread(fmt, 1, take, f) != take) {
                break;
            }
            channels = Rd16(fmt + 2);
            bits = Rd16(fmt + 14);
            if (sz > take) {
                fseek(f, sz - take, SEEK_CUR);
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            if (bits != 16 || channels < 1) {
                break;
            }
            size_t total = sz / 2;              // tong sample 16-bit
            std::vector<int16_t> raw(total);
            size_t rd = fread(raw.data(), 2, total, f);
            raw.resize(rd);
            if (channels == 1) {
                *out = std::move(raw);
            } else {
                out->reserve(rd / channels);    // downmix: lay kenh 0
                for (size_t i = 0; i + channels <= rd; i += channels) {
                    out->push_back(raw[i]);
                }
            }
            got_data = true;
            break;
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);  // chunk khac -> bo qua (padding chan)
        }
    }
    fclose(f);
    return got_data && !out->empty();
}

bool FlappySpritesLoad(FlappySprites* s) {
    // bg/ground khong can transparency; bird/pipe dung magenta key.
    s->bg          = LoadBmpArgb("bg.bmp", false);
    s->ground      = LoadBmpArgb("ground.bmp", false);
    s->bird        = LoadBmpArgb("bird.bmp", true);
    s->pipe_top    = LoadBmpArgb("pipe_top.bmp", true);
    s->pipe_bottom = LoadBmpArgb("pipe_bottom.bmp", true);
    bool ok = s->CoreReady();
    ESP_LOGI(TAG, "Sprites load: bg=%d ground=%d bird=%d pipe_top=%d pipe_bottom=%d -> core=%d",
             s->bg != nullptr, s->ground != nullptr, s->bird != nullptr,
             s->pipe_top != nullptr, s->pipe_bottom != nullptr, ok);
    if (!ok) {
        FlappySpritesFree(s);  // thieu core -> huy het, dung primitive
    }
    return ok;
}

void FlappySpritesFree(FlappySprites* s) {
    FreeDsc(&s->bg);
    FreeDsc(&s->ground);
    FreeDsc(&s->bird);
    FreeDsc(&s->pipe_top);
    FreeDsc(&s->pipe_bottom);
}

void FlappySoundsLoad(FlappySounds* s) {
    LoadWav("flap.wav", &s->flap);
    LoadWav("score.wav", &s->score);
    LoadWav("hit.wav", &s->hit);
    LoadWav("game_over.wav", &s->game_over);
    LoadWav("select.wav", &s->select);
    LoadWav("pause.wav", &s->pause);
    LoadWav("resume.wav", &s->resume);
    s->loaded = true;
    ESP_LOGI(TAG, "Sounds load samples: flap=%u score=%u hit=%u over=%u select=%u",
             (unsigned)s->flap.size(), (unsigned)s->score.size(), (unsigned)s->hit.size(),
             (unsigned)s->game_over.size(), (unsigned)s->select.size());
}
