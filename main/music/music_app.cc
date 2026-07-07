#include "music/music_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio/audio_service.h"

#define TAG "MusicApp"

namespace {

constexpr char kMusicDir[] = "/sdcard/tbot_music";
constexpr size_t kChunkFrames = 1440;          // 60ms @ 24kHz mono
constexpr uint32_t kPlaybackThrottle = 8;      // day them khi playback queue < nguong nay

std::mutex g_mutex;                 // bao ve state + fp + remaining + channels
MusicState g_state = MusicState::Stopped;
FILE* g_fp = nullptr;
long g_remaining = 0;               // so byte data con lai
int g_channels = 1;
std::function<void()> g_on_end;
bool g_task_started = false;

uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }

bool HasWavExt(const char* name) {
    size_t n = strlen(name);
    if (n < 4) return false;
    const char* e = name + n - 4;
    return (e[0] == '.' &&
            (e[1] == 'w' || e[1] == 'W') &&
            (e[2] == 'a' || e[2] == 'A') &&
            (e[3] == 'v' || e[3] == 'V'));
}

// Mo WAV, seek toi data, dien channels/remaining. Tra ve fp da mo (caller giu),
// hoac nullptr neu loi/khong phai PCM 16-bit.
FILE* OpenWav(const char* path, int* channels, long* data_bytes) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return nullptr;
    }
    uint16_t ch = 1, bits = 16;
    uint32_t rate = 24000;
    while (true) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) { fclose(f); return nullptr; }
        uint32_t sz = Rd32(hdr + 4);
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            uint32_t take = sz < 16 ? sz : 16;
            if (fread(fmt, 1, take, f) != take) { fclose(f); return nullptr; }
            ch = Rd16(fmt + 2);
            rate = Rd32(fmt + 4);
            bits = Rd16(fmt + 14);
            if (sz > take) fseek(f, sz - take, SEEK_CUR);
        } else if (memcmp(hdr, "data", 4) == 0) {
            if (bits != 16 || ch < 1 || ch > 2) {
                ESP_LOGW(TAG, "WAV khong ho tro: bits=%u ch=%u", bits, ch);
                fclose(f);
                return nullptr;
            }
            if (rate != 24000) {
                ESP_LOGW(TAG, "WAV sample_rate=%lu != 24000: co the sai cao do",
                         (unsigned long)rate);
            }
            *channels = ch;
            *data_bytes = (long)sz;
            return f;   // dang o dau data
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
}

void CloseLocked() {
    if (g_fp != nullptr) {
        fclose(g_fp);
        g_fp = nullptr;
    }
    g_remaining = 0;
}

void MusicTask(void*) {
    std::vector<int16_t> raw;
    std::vector<int16_t> mono;
    while (true) {
        // Doc trang thai + doc chunk duoi lock; phat audio ngoai lock.
        bool eof = false;
        size_t mono_n = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_state != MusicState::Playing || g_fp == nullptr) {
                // khong phat -> nghi
            } else {
                uint32_t dq, sq, pq;
                Application::GetInstance().GetAudioService().GetQueueDepths(dq, sq, pq);
                if (pq < kPlaybackThrottle) {
                    size_t want_samples = kChunkFrames * (size_t)g_channels;
                    size_t want_bytes = want_samples * 2;
                    if ((long)want_bytes > g_remaining) {
                        want_bytes = g_remaining > 0 ? (size_t)g_remaining : 0;
                    }
                    if (want_bytes == 0) {
                        eof = true;
                    } else {
                        raw.resize(want_bytes / 2);
                        size_t got = fread(raw.data(), 1, want_bytes, g_fp);
                        g_remaining -= (long)got;
                        size_t got_samples = got / 2;
                        if (got_samples == 0) {
                            eof = true;
                        } else if (g_channels == 1) {
                            mono.assign(raw.begin(), raw.begin() + got_samples);
                            mono_n = got_samples;
                        } else {  // stereo -> downmix kenh trai
                            size_t frames = got_samples / 2;
                            mono.resize(frames);
                            for (size_t i = 0; i < frames; ++i) mono[i] = raw[i * 2];
                            mono_n = frames;
                        }
                    }
                }
            }
        }

        if (eof) {
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                CloseLocked();
                g_state = MusicState::Stopped;
                cb = g_on_end;
            }
            ESP_LOGI(TAG, "Music ket thuc tu nhien");
            if (cb) cb();
            continue;
        }

        if (mono_n > 0) {
            mono.resize(mono_n);
            Application::GetInstance().GetAudioService().QueuePcmForPlayback(mono);
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));   // paused/stopped hoac queue day
        }
    }
}

}  // namespace

std::vector<std::string> MusicScanFiles() {
    std::vector<std::string> files;
    DIR* dir = opendir(kMusicDir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Khong mo duoc %s (thieu SD?)", kMusicDir);
        return files;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        if (HasWavExt(ent->d_name)) files.push_back(ent->d_name);
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    ESP_LOGI(TAG, "Quet %s: %u file .wav", kMusicDir, (unsigned)files.size());
    return files;
}

void MusicPlayerInit() {
    if (g_task_started) return;
    g_task_started = true;
    xTaskCreate(MusicTask, "music", 4096, nullptr, 4, nullptr);
}

void MusicSetOnEnd(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_on_end = std::move(cb);
}

bool MusicPlay(const std::string& filename) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", kMusicDir, filename.c_str());
    int ch = 1;
    long bytes = 0;
    FILE* f = OpenWav(path, &ch, &bytes);
    if (f == nullptr) {
        ESP_LOGW(TAG, "Play loi: %s", path);
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseLocked();
    g_fp = f;
    g_channels = ch;
    g_remaining = bytes;
    g_state = MusicState::Playing;
    ESP_LOGI(TAG, "Play %s (ch=%d bytes=%ld)", filename.c_str(), ch, bytes);
    return true;
}

void MusicPause() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state == MusicState::Playing) g_state = MusicState::Paused;
}

void MusicResume() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state == MusicState::Paused && g_fp != nullptr) g_state = MusicState::Playing;
}

void MusicStop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseLocked();
    g_state = MusicState::Stopped;
}

MusicState MusicGetState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}
