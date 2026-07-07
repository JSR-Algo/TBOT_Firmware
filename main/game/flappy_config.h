#ifndef FLAPPY_CONFIG_H_
#define FLAPPY_CONFIG_H_

// Cau hinh gameplay cho Flappy, doc tu /sdcard/tbot_games/flappy/game_config.json.
// Moi truong thieu/sai -> giu gia tri default ben duoi. Loader KHONG bao gio crash;
// neu SD chua mount / file khong co / JSON hong -> tra ve toan bo default.
struct FlappyConfig {
    float gravity = 0.6f;         // px/frame^2 keo chim xuong
    float jump_velocity = -7.2f;  // px/frame khi flap (am = len)
    int   pipe_speed = 3;         // px/frame ong chay sang trai
    int   gap_size = 110;         // do rong khe ho giua 2 ong (px)
    int   fps = 30;               // khung hinh/giay (10..60)
    bool  use_assets = false;     // true = co y dinh dung sprite BMP tren SD
    bool  from_sd = false;        // true neu doc duoc config tu SD (khac default)
};

// Doc game_config.json (neu co) va tra ve config da dien default cho field thieu.
FlappyConfig FlappyConfigLoad();

// Probe manifest.json + tung asset BMP: log co ton tai khong, kich thuoc, bpp.
// CHI bao cao ra log (yeu cau #8), KHONG decode/render sprite o buoc nay.
// An toan khi khong co SD (chi log "missing").
void FlappyAssetsProbeAndLog();

#endif  // FLAPPY_CONFIG_H_
