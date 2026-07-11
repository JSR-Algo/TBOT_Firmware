#include "app_manager.h"

#include <lvgl.h>
#include <esp_log.h>
#include <esp_random.h>

#include <vector>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "game/flappy_config.h"
#include "game/flappy_assets.h"
#include "ui/menu_ui.h"
#include "music/music_app.h"
#include "wifi_board.h"

#include <string>
#include <algorithm>

#define TAG "AppManager"

namespace {

// ---------------------------------------------------------------------------
// Trang thai chung
// ---------------------------------------------------------------------------
AppMode g_mode = AppMode::Chatbox;
lv_obj_t* g_overlay = nullptr;          // goc overlay tren lv_layer_top; null = dang o Chatbox
std::function<void(const char*)> g_slave_sender;
std::function<void(const std::vector<int16_t>&)> g_sound_player;  // phat SFX game
FlappySounds g_sounds;                  // SFX nap 1 lan tu SD, dung lai nhieu luot
bool g_slave_ready = false;

// Menu: 4 app - 0=Chatbox, 1=Game, 2=Music, 3=Speed.
constexpr int kMenuCount = 4;
int g_menu_sel = 0;
lv_obj_t* g_menu_box[kMenuCount] = { nullptr, nullptr, nullptr, nullptr };
lv_obj_t* g_menu_status = nullptr;      // dong trang thai slave
lv_obj_t* g_menu_sd_status = nullptr;   // dong trang thai the nho
MenuStrings g_menu_strings;             // text menu (SD hoac default)
MenuSounds g_menu_sounds;               // SFX menu (nap 1 lan)

// Speed app: chinh toc do tay+dau cua slave (dance nhac + gesture cham nut).
// 100 = baseline moi cua slave (da giam 2 lan). Gui "SPEED:LIMB:<pct>" xuong slave.
constexpr int kSpeedMin = 40;
constexpr int kSpeedMax = 200;
constexpr int kSpeedStep = 20;
int g_limb_speed_pct = 100;
lv_obj_t* g_speed_value_label = nullptr;
lv_obj_t* g_speed_bar = nullptr;

// Music
constexpr int kMusicVisible = 5;        // so dong danh sach hien cung luc
std::vector<std::string> g_music_files;
int g_music_sel = 0;
int g_music_playing_idx = -1;
lv_obj_t* g_music_line[kMusicVisible] = { nullptr };
lv_obj_t* g_music_state_label = nullptr;

// Game (Flappy primitive). Kich thuoc/layout la hang so; vat ly (gravity, jump,
// pipe_speed, gap, fps) lay tu FlappyConfig (SD hoac default).
constexpr int kScreenW = 480;
constexpr int kScreenH = 320;
constexpr int kBirdX = 90;
constexpr int kBirdSize = 22;      // kich thuoc chim che do primitive
constexpr int kPipeW = 46;         // be rong ong che do primitive
constexpr int kNumPipes = 2;
constexpr int kPipeSpacing = 240;
constexpr int kPipeSpriteH = 220;  // chieu cao sprite pipe (de dat vi tri + clip)

enum class GameState { Ready, Playing, Paused, Over };

struct Pipe {
    lv_obj_t* top;
    lv_obj_t* bottom;
    int x;
    int gap_y;      // dinh khe ho
    bool counted;
};

struct Game {
    GameState state = GameState::Ready;
    lv_obj_t* bird = nullptr;
    lv_obj_t* bg = nullptr;
    lv_obj_t* ground = nullptr;
    lv_obj_t* score_label = nullptr;
    lv_obj_t* hint_label = nullptr;
    Pipe pipes[kNumPipes];
    float bird_y = 0;
    float bird_vy = 0;
    int score = 0;
    lv_timer_t* timer = nullptr;
    FlappyConfig cfg;         // vat ly hien hanh (SD hoac default)
    FlappySprites sprites;    // sprite BMP da nap (rong neu primitive)
    bool use_sprites = false; // true = render bang sprite
    int bird_w = kBirdSize;   // hitbox chim (theo sprite neu co)
    int bird_h = kBirdSize;
    int pipe_w = kPipeW;      // be rong ong (theo sprite neu co)
    int floor_y = kScreenH;   // y mat dat -> cham la chet
};
Game g_game;

void PlaySfx(const std::vector<int16_t>& s) {
    if (g_sound_player && !s.empty()) {
        g_sound_player(s);
    }
}

Display* GetDisplay() {
    return Board::GetInstance().GetDisplay();
}

void SendToSlave(const char* line) {
    if (g_slave_sender) {
        g_slave_sender(line);
    }
}

// Chi cho mo Menu/Game khi thiet bi o trang thai on dinh (khong onboarding/lesson).
bool CanEnterMenu() {
    auto& app = Application::GetInstance();
    if (app.IsLessonRuntimeActive()) {
        return false;
    }
    switch (app.GetDeviceState()) {
        case kDeviceStateIdle:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Overlay tien ich
// ---------------------------------------------------------------------------
void EnsureOverlay() {
    if (g_overlay != nullptr) {
        return;
    }
    g_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, kScreenW, kScreenH);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x101828), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Dung font cua chatbox (BUILTIN_TEXT_FONT puhui) de hien tieng Viet co dau;
    // text_font la thuoc tinh ke thua nen moi label con deu nhan font nay.
    const lv_font_t* vi_font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
    if (vi_font != nullptr) {
        lv_obj_set_style_text_font(g_overlay, vi_font, 0);
    }
}

void DestroyOverlay() {
    if (g_overlay != nullptr) {
        lv_obj_del(g_overlay);   // xoa toan bo con ben trong
        g_overlay = nullptr;
    }
    for (int i = 0; i < kMenuCount; ++i) g_menu_box[i] = nullptr;
    g_menu_status = nullptr;
    g_menu_sd_status = nullptr;
    for (int i = 0; i < kMusicVisible; ++i) g_music_line[i] = nullptr;
    g_music_state_label = nullptr;
    g_speed_value_label = nullptr;
    g_speed_bar = nullptr;
    g_game.bird = nullptr;
    g_game.bg = nullptr;
    g_game.ground = nullptr;
    g_game.score_label = nullptr;
    g_game.hint_label = nullptr;
    for (int i = 0; i < kNumPipes; ++i) {
        g_game.pipes[i].top = nullptr;
        g_game.pipes[i].bottom = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void UpdateMenuSelection() {
    for (int i = 0; i < kMenuCount; ++i) {
        if (g_menu_box[i] == nullptr) {
            continue;
        }
        bool sel = (i == g_menu_sel);
        lv_obj_set_style_bg_color(g_menu_box[i],
            lv_color_hex(sel ? 0x2E90FA : 0x1D2939), 0);
        lv_obj_set_style_border_color(g_menu_box[i],
            lv_color_hex(sel ? 0xFFFFFF : 0x475467), 0);
        lv_obj_set_style_border_width(g_menu_box[i], sel ? 3 : 1, 0);
    }
}

void BuildMenu() {
    EnsureOverlay();
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x101828), 0);

    // Text tu SD (object "vi") hoac default tieng Viet khong dau.
    g_menu_strings = MenuLoadStrings();
    if (!g_menu_sounds.loaded) {
        MenuLoadSounds(&g_menu_sounds);   // nap SFX menu 1 lan
    }

    lv_obj_t* title = MakeLabel(g_overlay, g_menu_strings.title.c_str(), 0xFFFFFF);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t* subtitle = MakeLabel(g_overlay, g_menu_strings.subtitle.c_str(), 0x98A2B3);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 40);

    const char* names[kMenuCount] = {
        g_menu_strings.chatbox.c_str(),
        g_menu_strings.game.c_str(),
        g_menu_strings.music.c_str(),
        "Tốc độ",   // literal: khong them std::string member (tiet kiem SRAM noi luc boot)
    };
    const int box_x[kMenuCount] = { -171, -57, 57, 171 };
    for (int i = 0; i < kMenuCount; ++i) {
        lv_obj_t* box = lv_obj_create(g_overlay);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 108, 84);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(box, LV_ALIGN_CENTER, box_x[i], -8);
        lv_obj_t* lbl = MakeLabel(box, names[i], 0xFFFFFF);
        lv_obj_set_width(lbl, 98);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        g_menu_box[i] = box;
    }
    UpdateMenuSelection();

    lv_obj_t* h1 = MakeLabel(g_overlay, g_menu_strings.left_right_hint.c_str(), 0x98A2B3);
    lv_obj_align(h1, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_t* h2 = MakeLabel(g_overlay, g_menu_strings.both_hint.c_str(), 0x98A2B3);
    lv_obj_align(h2, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_t* h3 = MakeLabel(g_overlay, g_menu_strings.hold_hint.c_str(), 0x98A2B3);
    lv_obj_align(h3, LV_ALIGN_BOTTOM_MID, 0, -34);

    g_menu_status = MakeLabel(g_overlay,
        (g_slave_ready ? g_menu_strings.slave_ok : g_menu_strings.slave_wait).c_str(), 0x667085);
    lv_obj_align(g_menu_status, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    g_menu_sd_status = MakeLabel(g_overlay,
        (g_menu_strings.from_sd ? g_menu_strings.sd_ok : g_menu_strings.sd_fail).c_str(), 0x667085);
    lv_obj_align(g_menu_sd_status, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
}

// ---------------------------------------------------------------------------
// Game
// ---------------------------------------------------------------------------
int RandGapY() {
    int margin = 30;
    int range = kScreenH - g_game.cfg.gap_size - 2 * margin;
    if (range < 1) {
        range = 1;
    }
    return margin + (int)(esp_random() % (uint32_t)range);
}

void PositionPipe(Pipe* p) {
    int gap = g_game.cfg.gap_size;
    if (g_game.use_sprites) {
        // Sprite cao co dinh: dat sao cho mieng ong trung mep khe ho, phan thua
        // tran ra ngoai man se bi overlay clip.
        lv_obj_set_pos(p->top, p->x, p->gap_y - kPipeSpriteH);   // mieng o day sprite -> mep duoi = gap_y
        lv_obj_set_pos(p->bottom, p->x, p->gap_y + gap);         // mieng o dinh sprite -> mep tren = gap_y+gap
    } else {
        lv_obj_set_pos(p->top, p->x, 0);
        lv_obj_set_size(p->top, kPipeW, p->gap_y);
        lv_obj_set_pos(p->bottom, p->x, p->gap_y + gap);
        lv_obj_set_size(p->bottom, kPipeW, kScreenH - (p->gap_y + gap));
    }
}

void GameReset() {
    g_game.bird_y = kScreenH / 2 - g_game.bird_h / 2;
    g_game.bird_vy = 0;
    g_game.score = 0;
    for (int i = 0; i < kNumPipes; ++i) {
        g_game.pipes[i].x = kScreenW + i * kPipeSpacing;
        g_game.pipes[i].gap_y = RandGapY();
        g_game.pipes[i].counted = false;
        PositionPipe(&g_game.pipes[i]);
    }
    lv_obj_set_pos(g_game.bird, kBirdX, (int)g_game.bird_y);
    lv_label_set_text(g_game.score_label, "0");
}

void SetGameHint(const char* text) {
    if (g_game.hint_label == nullptr) {
        return;
    }
    if (text == nullptr) {
        lv_obj_add_flag(g_game.hint_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(g_game.hint_label, text);
    lv_obj_clear_flag(g_game.hint_label, LV_OBJ_FLAG_HIDDEN);
}

void GameOver() {
    g_game.state = GameState::Over;
    SetGameHint("GAME OVER  -  BOTH de choi lai");
    PlaySfx(g_sounds.hit);
    PlaySfx(g_sounds.game_over);
}

void GameTick(lv_timer_t* /*t*/) {
    if (g_game.state != GameState::Playing) {
        return;
    }

    g_game.bird_vy += g_game.cfg.gravity;
    g_game.bird_y += g_game.bird_vy;

    if (g_game.bird_y < 0) {
        g_game.bird_y = 0;
        g_game.bird_vy = 0;
    }
    if (g_game.bird_y + g_game.bird_h >= g_game.floor_y) {
        g_game.bird_y = g_game.floor_y - g_game.bird_h;
        lv_obj_set_y(g_game.bird, (int)g_game.bird_y);
        GameOver();
        return;
    }
    lv_obj_set_y(g_game.bird, (int)g_game.bird_y);

    for (int i = 0; i < kNumPipes; ++i) {
        Pipe* p = &g_game.pipes[i];
        p->x -= g_game.cfg.pipe_speed;
        if (p->x + g_game.pipe_w < 0) {
            p->x += kNumPipes * kPipeSpacing;
            p->gap_y = RandGapY();
            p->counted = false;
        }
        PositionPipe(p);

        // Diem: qua ong.
        if (!p->counted && p->x + g_game.pipe_w < kBirdX) {
            p->counted = true;
            g_game.score++;
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", g_game.score);
            lv_label_set_text(g_game.score_label, buf);
            PlaySfx(g_sounds.score);
        }

        // Va cham.
        bool x_overlap = (kBirdX + g_game.bird_w > p->x) && (kBirdX < p->x + g_game.pipe_w);
        if (x_overlap) {
            bool y_hit = (g_game.bird_y < p->gap_y) ||
                         (g_game.bird_y + g_game.bird_h > p->gap_y + g_game.cfg.gap_size);
            if (y_hit) {
                GameOver();
                return;
            }
        }
    }
}

// Tao 1 ong (rect primitive). Sprite dung ham rieng ben duoi.
void BuildPrimitivePipe(Pipe* p) {
    for (lv_obj_t** obj : { &p->top, &p->bottom }) {
        *obj = lv_obj_create(g_overlay);
        lv_obj_remove_style_all(*obj);
        lv_obj_set_style_bg_color(*obj, lv_color_hex(0x12B76A), 0);
        lv_obj_set_style_bg_opa(*obj, LV_OPA_COVER, 0);
        lv_obj_clear_flag(*obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void BuildGame() {
    EnsureOverlay();
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x4EC0E9), 0);  // troi xanh

    // Nap config gameplay tu SD (fallback default) + bao cao asset.
    g_game.cfg = FlappyConfigLoad();
    FlappyAssetsProbeAndLog();

    // Nap sprite neu config yeu cau; that bai -> primitive.
    g_game.use_sprites = false;
    g_game.bird_w = kBirdSize;
    g_game.bird_h = kBirdSize;
    g_game.pipe_w = kPipeW;
    g_game.floor_y = kScreenH;
    if (g_game.cfg.use_assets) {
        if (FlappySpritesLoad(&g_game.sprites)) {
            g_game.use_sprites = true;
        }
        if (!g_sounds.loaded) {
            FlappySoundsLoad(&g_sounds);   // nap SFX 1 lan, dung lai cac luot sau
        }
    }

    // Layer 0: background (chi khi co sprite bg).
    if (g_game.use_sprites && g_game.sprites.bg != nullptr) {
        g_game.bg = lv_image_create(g_overlay);
        lv_image_set_src(g_game.bg, g_game.sprites.bg);
        lv_obj_set_pos(g_game.bg, 0, 0);
    }

    // Layer 1: pipes.
    for (int i = 0; i < kNumPipes; ++i) {
        Pipe* p = &g_game.pipes[i];
        if (g_game.use_sprites) {
            p->top = lv_image_create(g_overlay);
            lv_image_set_src(p->top, g_game.sprites.pipe_top);
            p->bottom = lv_image_create(g_overlay);
            lv_image_set_src(p->bottom, g_game.sprites.pipe_bottom);
        } else {
            BuildPrimitivePipe(p);
        }
    }

    // Layer 2: ground (sprite) -> che chan ong; cap nhat floor va bird hitbox.
    if (g_game.use_sprites && g_game.sprites.ground != nullptr) {
        int gh = g_game.sprites.ground->header.h;
        g_game.floor_y = kScreenH - gh;
        g_game.ground = lv_image_create(g_overlay);
        lv_image_set_src(g_game.ground, g_game.sprites.ground);
        lv_obj_set_pos(g_game.ground, 0, g_game.floor_y);
    }
    if (g_game.use_sprites) {
        g_game.pipe_w = g_game.sprites.pipe_top->header.w;
    }

    // Layer 3: bird (sprite hoac rect).
    if (g_game.use_sprites && g_game.sprites.bird != nullptr) {
        g_game.bird_w = g_game.sprites.bird->header.w;
        g_game.bird_h = g_game.sprites.bird->header.h;
        g_game.bird = lv_image_create(g_overlay);
        lv_image_set_src(g_game.bird, g_game.sprites.bird);
    } else {
        g_game.bird = lv_obj_create(g_overlay);
        lv_obj_remove_style_all(g_game.bird);
        lv_obj_set_size(g_game.bird, kBirdSize, kBirdSize);
        lv_obj_set_style_radius(g_game.bird, kBirdSize / 2, 0);
        lv_obj_set_style_bg_color(g_game.bird, lv_color_hex(0xFDB022), 0);
        lv_obj_set_style_bg_opa(g_game.bird, LV_OPA_COVER, 0);
    }

    // Layer 4: labels.
    g_game.score_label = MakeLabel(g_overlay, "0", 0xFFFFFF);
    lv_obj_align(g_game.score_label, LV_ALIGN_TOP_MID, 0, 10);
    g_game.hint_label = MakeLabel(g_overlay, "", 0xFFFFFF);
    lv_obj_align(g_game.hint_label, LV_ALIGN_CENTER, 0, 0);

    g_game.state = GameState::Ready;
    GameReset();
    SetGameHint("BOTH de bat dau  -  RIGHT bay");

    ESP_LOGI(TAG, "Game built: sprites=%d bird=%dx%d pipe_w=%d floor_y=%d fps=%d",
             g_game.use_sprites, g_game.bird_w, g_game.bird_h, g_game.pipe_w,
             g_game.floor_y, g_game.cfg.fps);

    if (g_game.timer == nullptr) {
        int period_ms = 1000 / g_game.cfg.fps;   // fps da clamp 10..60
        g_game.timer = lv_timer_create(GameTick, period_ms, nullptr);
    }
}

void StopGameTimer() {
    if (g_game.timer != nullptr) {
        lv_timer_del(g_game.timer);
        g_game.timer = nullptr;
    }
}

void GameStartOrRestart() {
    GameReset();
    g_game.state = GameState::Playing;
    SetGameHint(nullptr);
    PlaySfx(g_sounds.select);
}

void GameFlap() {
    if (g_game.state == GameState::Playing) {
        g_game.bird_vy = g_game.cfg.jump_velocity;
        PlaySfx(g_sounds.flap);
    }
}

void GameLeft() {
    // Pause/resume khi dang choi; choi lai khi game over.
    if (g_game.state == GameState::Playing) {
        g_game.state = GameState::Paused;
        SetGameHint("PAUSED  -  LEFT de tiep tuc");
        PlaySfx(g_sounds.pause);
    } else if (g_game.state == GameState::Paused) {
        g_game.state = GameState::Playing;
        SetGameHint(nullptr);
        PlaySfx(g_sounds.resume);
    } else if (g_game.state == GameState::Over) {
        GameStartOrRestart();
    }
}

// ---------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------
const char* MusicStateText() {
    switch (MusicGetState()) {
        case MusicState::Playing: return "Dang phat  (cham 2 nut de tam dung)";
        case MusicState::Paused:  return "Tam dung  (cham 2 nut de phat)";
        default:                  return "Da dung  (cham 2 nut de phat)";
    }
}

void UpdateMusicList() {
    int n = (int)g_music_files.size();
    // Cua so hien thi bao quanh muc dang chon.
    int start = g_music_sel - kMusicVisible / 2;
    if (start > n - kMusicVisible) start = n - kMusicVisible;
    if (start < 0) start = 0;

    for (int row = 0; row < kMusicVisible; ++row) {
        if (g_music_line[row] == nullptr) continue;
        int idx = start + row;
        if (n == 0 && row == 0) {
            lv_label_set_text(g_music_line[row], g_menu_strings.music_empty.c_str());
            lv_obj_set_style_text_color(g_music_line[row], lv_color_hex(0x98A2B3), 0);
            continue;
        }
        if (idx < 0 || idx >= n) {
            lv_label_set_text(g_music_line[row], "");
            continue;
        }
        bool sel = (idx == g_music_sel);
        bool playing = (idx == g_music_playing_idx && MusicGetState() == MusicState::Playing);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s%s", playing ? "> " : (sel ? "  " : "  "),
                 g_music_files[idx].c_str());
        lv_label_set_text(g_music_line[row], buf);
        lv_obj_set_style_text_color(g_music_line[row],
            lv_color_hex(sel ? 0xFDB022 : 0xE4E7EC), 0);
    }
    if (g_music_state_label != nullptr) {
        lv_label_set_text(g_music_state_label, MusicStateText());
    }
}

void BuildMusic() {
    EnsureOverlay();
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x101828), 0);

    lv_obj_t* title = MakeLabel(g_overlay, g_menu_strings.music.c_str(), 0xFFFFFF);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    for (int row = 0; row < kMusicVisible; ++row) {
        lv_obj_t* l = MakeLabel(g_overlay, "", 0xE4E7EC);
        lv_obj_set_width(l, 440);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 24, 46 + row * 26);
        g_music_line[row] = l;
    }

    g_music_state_label = MakeLabel(g_overlay, "", 0x98A2B3);
    lv_obj_align(g_music_state_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_t* hint = MakeLabel(g_overlay,
        "Trai/Phai: doi bai   Cham 2 nut: phat/dung   Giu 3s: menu", 0x667085);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    UpdateMusicList();
}

void MusicSelectMove(int delta) {
    int n = (int)g_music_files.size();
    if (n <= 0) {
        PlaySfx(g_menu_sounds.error);
        return;
    }
    g_music_sel = (g_music_sel + delta % n + n) % n;
    UpdateMusicList();
    PlaySfx(g_menu_sounds.move);
}

void MusicToggle() {
    int n = (int)g_music_files.size();
    if (n <= 0) {
        PlaySfx(g_menu_sounds.error);
        return;
    }
    MusicState st = MusicGetState();
    if (st == MusicState::Playing && g_music_playing_idx == g_music_sel) {
        MusicPause();
        SendToSlave("DANCE:MUSIC:STOP");
    } else if (st == MusicState::Paused && g_music_playing_idx == g_music_sel) {
        MusicResume();
        SendToSlave("DANCE:MUSIC:START");
    } else {
        if (MusicPlay(g_music_files[g_music_sel])) {
            g_music_playing_idx = g_music_sel;
            SendToSlave("DANCE:MUSIC:START");
            PlaySfx(g_menu_sounds.select);
        } else {
            PlaySfx(g_menu_sounds.error);
        }
    }
    UpdateMusicList();
}

// ---------------------------------------------------------------------------
// Speed (chinh toc do tay+dau cua slave: dance nhac + gesture cham nut)
// ---------------------------------------------------------------------------
void SendLimbSpeed() {
    char buf[24];
    snprintf(buf, sizeof(buf), "SPEED:LIMB:%d", g_limb_speed_pct);
    SendToSlave(buf);
}

void UpdateSpeedUI() {
    if (g_speed_value_label != nullptr) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", g_limb_speed_pct);
        lv_label_set_text(g_speed_value_label, buf);
    }
    if (g_speed_bar != nullptr) {
        // Chieu rong thanh theo % trong khoang [kSpeedMin..kSpeedMax] -> [0..360]px.
        int span = kSpeedMax - kSpeedMin;
        int w = 360 * (g_limb_speed_pct - kSpeedMin) / (span > 0 ? span : 1);
        if (w < 6) w = 6;
        lv_obj_set_width(g_speed_bar, w);
    }
}

void BuildSpeed() {
    EnsureOverlay();
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x101828), 0);

    lv_obj_t* title = MakeLabel(g_overlay, "Tốc độ tay & đầu", 0xFFFFFF);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    g_speed_value_label = MakeLabel(g_overlay, "100%", 0xFDB022);
    lv_obj_align(g_speed_value_label, LV_ALIGN_CENTER, 0, -24);

    // Track + thanh muc do toc do.
    lv_obj_t* track = lv_obj_create(g_overlay);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 360, 16);
    lv_obj_set_style_radius(track, 8, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x1D2939), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 24);

    g_speed_bar = lv_obj_create(track);
    lv_obj_remove_style_all(g_speed_bar);
    lv_obj_set_size(g_speed_bar, 180, 16);
    lv_obj_set_style_radius(g_speed_bar, 8, 0);
    lv_obj_set_style_bg_color(g_speed_bar, lv_color_hex(0x2E90FA), 0);
    lv_obj_set_style_bg_opa(g_speed_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_speed_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g_speed_bar, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* hint = MakeLabel(g_overlay,
        "Trái: chậm   Phải: nhanh   Chạm 2 nút: đặt lại 100%   Giữ 3s: menu", 0x667085);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    UpdateSpeedUI();
}

void SpeedAdjust(int delta) {
    int v = g_limb_speed_pct + delta;
    if (v < kSpeedMin) v = kSpeedMin;
    if (v > kSpeedMax) v = kSpeedMax;
    if (v == g_limb_speed_pct) {
        PlaySfx(g_menu_sounds.error);   // da cham/kich tran
        return;
    }
    g_limb_speed_pct = v;
    SendLimbSpeed();
    UpdateSpeedUI();
    PlaySfx(g_menu_sounds.move);
}

void SpeedReset() {
    g_limb_speed_pct = 100;
    SendLimbSpeed();
    UpdateSpeedUI();
    PlaySfx(g_menu_sounds.select);
}

}  // namespace

// ---------------------------------------------------------------------------
// API cong khai
// ---------------------------------------------------------------------------
void AppManagerInit() {
    g_mode = AppMode::Chatbox;
    g_overlay = nullptr;

    MusicPlayerInit();
    // Bai nhac ket thuc tu nhien (music task) -> marshal sang main task: bao slave
    // ngung nhay + cap nhat UI neu dang o Music.
    MusicSetOnEnd([] {
        Application::GetInstance().Schedule([] {
            SendToSlave("DANCE:MUSIC:STOP");
            g_music_playing_idx = -1;
            if (g_mode == AppMode::Music) {
                DisplayLockGuard lock(GetDisplay());
                UpdateMusicList();
            }
        });
    });
}

void AppManagerSetSlaveSender(std::function<void(const char*)> sender) {
    g_slave_sender = std::move(sender);
}

void AppManagerSetSoundPlayer(std::function<void(const std::vector<int16_t>&)> player) {
    g_sound_player = std::move(player);
}

AppMode AppGetMode() {
    return g_mode;
}

void AppOnSlaveReady() {
    g_slave_ready = true;
    if (g_mode == AppMode::Menu && g_menu_status != nullptr) {
        DisplayLockGuard lock(GetDisplay());
        lv_label_set_text(g_menu_status, g_menu_strings.slave_ok.c_str());
    }
}

static void SwitchToInternal(AppMode mode) {
    if (mode == g_mode) {
        return;
    }
    DisplayLockGuard lock(GetDisplay());

    // Roi che do hien tai.
    if (g_mode == AppMode::Game) {
        StopGameTimer();
        FlappySpritesFree(&g_game.sprites);  // tra PSRAM cua sprite
        g_game.use_sprites = false;
    }
    if (g_mode == AppMode::Music) {
        MusicStop();
        SendToSlave("DANCE:MUSIC:STOP");   // dam bao slave ngung nhay + ve nghi
        g_music_playing_idx = -1;
    }
    DestroyOverlay();

    g_mode = mode;
    switch (mode) {
        case AppMode::Menu:
            g_menu_sel = 0;
            BuildMenu();
            SendToSlave("MODE:MENU");
            PlaySfx(g_menu_sounds.back);   // ve menu
            break;
        case AppMode::Game:
            BuildGame();
            SendToSlave("MODE:GAME");
            PlaySfx(g_menu_sounds.open_game);
            break;
        case AppMode::Music:
            g_music_files = MusicScanFiles();
            g_music_sel = 0;
            g_music_playing_idx = -1;
            BuildMusic();
            SendToSlave("MODE:MUSIC");
            PlaySfx(g_menu_sounds.select);
            break;
        case AppMode::Speed:
            BuildSpeed();
            SendToSlave("MODE:MENU");   // slave khong co mode rieng cho Speed; giu tu the nghi
            SendLimbSpeed();            // dong bo toc do hien tai xuong slave khi vao app
            PlaySfx(g_menu_sounds.select);
            break;
        case AppMode::Chatbox:
        default:
            // Overlay da bi xoa -> UI chatbox ben duoi hien lai.
            SendToSlave("MODE:CHATBOX");
            PlaySfx(g_menu_sounds.open_chatbox);
            break;
    }
}

void AppHandleMenuHold() {
    if (g_mode != AppMode::Menu && !CanEnterMenu()) {
        ESP_LOGW(TAG, "MENU_HOLD ignored: device busy (state=%d)",
                 (int)Application::GetInstance().GetDeviceState());
        PlaySfx(g_menu_sounds.error);
        return;
    }
    SwitchToInternal(AppMode::Menu);
}

// RIGHT hold 3s (slave EVT:RIGHT_HOLD_3S): doi Wi-Fi, giu claim.
// Tuong duong BOOT double-click tren main board (lab). Co the goi tu moi AppMode.
void AppHandleRightHold() {
    auto& app = Application::GetInstance();
    if (app.IsLessonRuntimeActive()) {
        ESP_LOGI(TAG, "RIGHT_HOLD ignored during lesson");
        PlaySfx(g_menu_sounds.error);
        return;
    }
    ESP_LOGI(TAG, "RIGHT_HOLD -> EnterWifiConfigMode (change Wi-Fi, keep claim)");
    // TBOT main (lcdwiki) la WifiBoard. static_cast giong cac board khac (RTTI off).
    static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
}

// Chatbox volume: buoc 10, clamp 0..100. Man hinh co the an — van ShowNotification best-effort.
static void AdjustChatboxVolume(int delta) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int volume = codec->output_volume() + delta;
    volume = std::max(0, std::min(100, volume));
    codec->SetOutputVolume(volume);
    auto* display = GetDisplay();
    if (display != nullptr) {
        if (volume == 0) {
            display->ShowNotification(Lang::Strings::MUTED);
        } else {
            display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume / 10));
        }
    }
    ESP_LOGI(TAG, "Chatbox volume -> %d", volume);
}

void AppHandleInputLeft() {
    switch (g_mode) {
        case AppMode::Menu: {
            DisplayLockGuard lock(GetDisplay());
            g_menu_sel = (g_menu_sel + kMenuCount - 1) % kMenuCount;   // truoc
            UpdateMenuSelection();
            PlaySfx(g_menu_sounds.move);
            break;
        }
        case AppMode::Game: {
            DisplayLockGuard lock(GetDisplay());
            GameLeft();
            break;
        }
        case AppMode::Music: {
            DisplayLockGuard lock(GetDisplay());
            MusicSelectMove(-1);
            break;
        }
        case AppMode::Speed: {
            DisplayLockGuard lock(GetDisplay());
            SpeedAdjust(-kSpeedStep);   // Trai: cham lai
            break;
        }
        case AppMode::Chatbox:
        default: {
            // LEFT tap = talk / dung (tuong duong BOOT click).
            auto& app = Application::GetInstance();
            if (app.IsLessonRuntimeActive()) {
                ESP_LOGI(TAG, "LEFT_CLICK ignored during lesson");
                break;
            }
            app.ToggleChatState();
            break;
        }
    }
}

void AppHandleInputRight() {
    switch (g_mode) {
        case AppMode::Menu: {
            DisplayLockGuard lock(GetDisplay());
            g_menu_sel = (g_menu_sel + 1) % kMenuCount;   // sau
            UpdateMenuSelection();
            PlaySfx(g_menu_sounds.move);
            break;
        }
        case AppMode::Game: {
            DisplayLockGuard lock(GetDisplay());
            GameFlap();
            break;
        }
        case AppMode::Music: {
            DisplayLockGuard lock(GetDisplay());
            MusicSelectMove(1);
            break;
        }
        case AppMode::Speed: {
            DisplayLockGuard lock(GetDisplay());
            SpeedAdjust(kSpeedStep);   // Phai: nhanh hon
            break;
        }
        case AppMode::Chatbox:
        default:
            AdjustChatboxVolume(+10);
            break;
    }
}

AppMode MenuSelToMode(int sel) {
    switch (sel) {
        case 1:  return AppMode::Game;
        case 2:  return AppMode::Music;
        case 3:  return AppMode::Speed;
        default: return AppMode::Chatbox;
    }
}

void AppHandleInputBothClick() {
    switch (g_mode) {
        case AppMode::Menu:
            PlaySfx(g_menu_sounds.select);
            SwitchToInternal(MenuSelToMode(g_menu_sel));
            break;
        case AppMode::Game: {
            DisplayLockGuard lock(GetDisplay());
            GameStartOrRestart();
            break;
        }
        case AppMode::Music: {
            DisplayLockGuard lock(GetDisplay());
            MusicToggle();
            break;
        }
        case AppMode::Speed: {
            DisplayLockGuard lock(GetDisplay());
            SpeedReset();   // Cham 2 nut: dat lai 100%
            break;
        }
        case AppMode::Chatbox:
        default:
            AdjustChatboxVolume(-10);
            break;
    }
}
