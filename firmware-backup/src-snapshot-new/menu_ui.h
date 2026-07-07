#ifndef MENU_UI_H_
#define MENU_UI_H_

#include <cstdint>
#include <string>
#include <vector>

// Chuoi hien thi Main Menu (tieng Viet CO DAU; menu dung font puhui cua chatbox
// nen hien duoc dau). Uu tien doc tu /sdcard/tbot_ui/menu_config.json object "vi";
// thieu/loi -> giu default hard-code ben duoi.
struct MenuStrings {
    std::string title = "TBOT";
    std::string subtitle = "Chọn ứng dụng";
    std::string chatbox = "Trò chuyện";
    std::string game = "Trò chơi";
    std::string music = "Âm nhạc";
    std::string music_empty = "Không có nhạc trên thẻ";
    std::string left_right_hint = "Trái/Phải: đổi lựa chọn";
    std::string both_hint = "Chạm cả hai: chọn";
    std::string hold_hint = "Giữ cả hai 3 giây: về menu";
    std::string slave_ok = "Slave: OK";
    std::string slave_wait = "Slave: đợi kết nối";
    std::string sd_ok = "Thẻ nhớ: OK";
    std::string sd_fail = "Thẻ nhớ: không có";
    bool from_sd = false;   // true neu doc duoc config tu SD
};

MenuStrings MenuLoadStrings();

// SFX menu (PCM 16-bit mono 24kHz). Vector rong = file loi/khong co.
struct MenuSounds {
    std::vector<int16_t> move;          // LEFT/RIGHT doi lua chon
    std::vector<int16_t> select;        // BOTH_CLICK chon app
    std::vector<int16_t> back;          // BOTH_HOLD ve menu
    std::vector<int16_t> open_chatbox;  // vao Chatbox
    std::vector<int16_t> open_game;     // vao Game
    std::vector<int16_t> error;         // input khong hop le
    bool loaded = false;
};

void MenuLoadSounds(MenuSounds* s);

#endif  // MENU_UI_H_
