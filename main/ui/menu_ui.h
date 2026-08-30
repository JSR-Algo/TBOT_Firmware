#ifndef MENU_UI_H_
#define MENU_UI_H_

#include <cstdint>
#include <string>
#include <vector>

// Chuỗi hiển thị menu chính (tiếng Việt có dấu; menu dùng phông Puhui của hộp thoại
// nên hiển thị được dấu). Ưu tiên đọc đối tượng "vi" từ menu_config.json trên thẻ nhớ;
// nếu thiếu hoặc lỗi thì giữ các giá trị mặc định bên dưới.
struct MenuStrings {
    std::string title = "TBOT";
    std::string subtitle = "Chọn ứng dụng";
    std::string chatbox = "Trò chuyện";
    std::string game = "Trò chơi";
    std::string music = "Âm nhạc";
    std::string music_empty = "Không có nhạc trên thẻ";
    std::string left_right_hint = "Trái/phải: đổi lựa chọn";
    std::string both_hint = "Chạm cả hai: chọn";
    std::string hold_hint = "Giữ cả hai 3 giây: về menu";
    std::string slave_ok = "Slave: OK";
    std::string slave_wait = "Slave: chờ kết nối";
    std::string sd_ok = "Thẻ nhớ: OK";
    std::string sd_fail = "Thẻ nhớ: không có";
    bool from_sd = false;   // true nếu đọc được cấu hình từ thẻ nhớ
};

MenuStrings MenuLoadStrings();

// Âm thanh menu (PCM 16-bit mono 24 kHz). Vector rỗng nếu tệp lỗi hoặc không có.
struct MenuSounds {
    std::vector<int16_t> move;          // LEFT/RIGHT đổi lựa chọn
    std::vector<int16_t> select;        // BOTH_CLICK chọn ứng dụng
    std::vector<int16_t> back;          // BOTH_HOLD về menu
    std::vector<int16_t> open_chatbox;  // vào Trò chuyện
    std::vector<int16_t> open_game;     // vào Trò chơi
    std::vector<int16_t> error;         // thao tác không hợp lệ
    bool loaded = false;
};

void MenuLoadSounds(MenuSounds* s);

#endif  // MENU_UI_H_
