#ifndef FLAPPY_ASSETS_H_
#define FLAPPY_ASSETS_H_

#include <lvgl.h>
#include <cstdint>
#include <vector>

// Bo sprite Flappy decode tu BMP 24-bit tren SD -> lv_img_dsc_t ARGB8888 (buffer
// trong PSRAM). Pixel magenta RGB(255,0,255) -> trong suot (alpha 0).
// Con tro null = asset do khong nap duoc (game se fallback primitive cho phan do).
struct FlappySprites {
    lv_img_dsc_t* bg = nullptr;           // 480x320
    lv_img_dsc_t* ground = nullptr;       // 480xN
    lv_img_dsc_t* bird = nullptr;         // WxH nho
    lv_img_dsc_t* pipe_top = nullptr;     // 64x220, mieng o DAY sprite
    lv_img_dsc_t* pipe_bottom = nullptr;  // 64x220, mieng o DINH sprite

    // Du dieu kien render sprite (toi thieu can bird + 2 pipe).
    bool CoreReady() const { return bird && pipe_top && pipe_bottom; }
};

// Nap toan bo sprite (best-effort tung file). Tra ve true neu CoreReady().
// An toan khi thieu SD/file: tra ve false, cac con tro giu null.
bool FlappySpritesLoad(FlappySprites* s);

// Giai phong buffer PSRAM + dsc. An toan goi nhieu lan.
void FlappySpritesFree(FlappySprites* s);

// SFX game (PCM 16-bit mono 24kHz). Vector rong = khong nap duoc file do.
struct FlappySounds {
    std::vector<int16_t> flap;
    std::vector<int16_t> score;
    std::vector<int16_t> hit;
    std::vector<int16_t> game_over;
    std::vector<int16_t> select;
    std::vector<int16_t> pause;
    std::vector<int16_t> resume;
    bool loaded = false;   // da thu nap (du thanh cong hay khong)
};

// Nap cac WAV tu SD (best-effort). File loi -> vector tuong ung rong.
void FlappySoundsLoad(FlappySounds* s);

// Loader WAV dung chung (PCM 16-bit mono; stereo -> lay kenh trai). Tra ve true
// neu doc duoc data 16-bit. Dung cho ca SFX game lan SFX menu.
bool WavLoadMono16(const char* path, std::vector<int16_t>* out);

#endif  // FLAPPY_ASSETS_H_
