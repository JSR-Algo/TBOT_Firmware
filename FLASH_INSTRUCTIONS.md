# TBOT firmware flash — Wake-word barge-in enabled

Đã build firmware mới với:

- `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y` — wake-word luôn active
- `application.cc:kDeviceStateSpeaking` — explicitly enable wake-word in realtime mode
- Wake words active: **"Hi ESP"** (English) và **"Nĩ hảo Xiǎo Zhì"** (Chinese, "你好Tbot")

Binary mới ở `build/xiaozhi.bin` (2.5MB).

## Endpoint đang dùng để nạp code

- Web/admin: `https://eval-renaissance-covering-yukon.trycloudflare.com`
- OTA: `https://athletic-editorials-stereo-anatomy.trycloudflare.com/tbot/ota/`
- WS: `wss://fantastic-hall-owners-programming.trycloudflare.com/tbot/v1/`

Firmware lấy WS từ OTA JSON. Khi build/flash, `CONFIG_OTA_URL` đang trỏ tới OTA URL ở trên.

## Cách flash 1 — USB (nhanh nhất, ~30s)

> CẢNH BÁO: KHÔNG dùng `idf.py flash` trần trên một checkout mới — nó sẽ ra MÀN HÌNH ĐEN.
> `sdkconfig` bị gitignore và ESP-IDF KHÔNG tự nạp `sdkconfig.defaults.local`, nên board
> mặc định Kconfig (`BOARD_TYPE_BREAD_COMPACT_WIFI`, không có driver LCD) sẽ thắng → màn
> hình đen (WiFi vẫn chạy nên dễ tưởng là OK). Luôn flash bằng script fleet-safe dưới đây.

1. Cắm cáp USB-C từ robot vào laptop
2. Trên macOS: liệt kê port serial:
   ```bash
   ls /dev/cu.usbmodem* /dev/cu.wch*
   ```
3. Build + flash bằng script fleet-safe (thay XXXX bằng port từ bước 2):
   ```bash
   cd /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware
   ./build-lcdwiki.sh /dev/cu.usbmodemXXXX
   ```
   Script sẽ **HARD-ABORT** trừ khi xác nhận được `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`
   trong `sdkconfig` — đây chính là chốt chặn ngăn image bread-board màn-hình-đen. Khi
   xong nó in app size + dòng `FLASH OK for <port>`.

4. Robot tự reboot, kết nối lại WiFi + server.

## Cách flash 2 — OTA qua manager-web (không cần cáp)

1. Mở manager-web admin UI (port 8002):
   ```
   https://eval-renaissance-covering-yukon.trycloudflare.com
   ```
2. Vào tab **OTA / Firmware Management** (tên menu tùy ngôn ngữ)
3. Upload file `build/xiaozhi.bin`
4. Chọn target device theo MAC (mỗi robot một MAC riêng) — ví dụ một unit: `3c:0f:02:de:c2:e0`
5. Click "Push firmware" hoặc "OTA update"
6. Robot sẽ tự download + flash + reboot

> CAVEAT (PROJECT_VER 2.2.30): MỌI image — bản LCD đúng lẫn bản bread-board màn-hình-đen —
> đều report cùng version `2.2.30` cho OTA, nên OTA coi là "đã mới nhất" và **KHÔNG** sửa
> được một unit đã bị flash nhầm board. Vì vậy USB qua `build-lcdwiki.sh` (Cách 1) mới là
> source of truth khi flash cả fleet; OTA chỉ dùng để cập nhật unit đã chắc chắn đúng board.

## Sau khi flash

Robot có wake-word barge-in. Khi robot đang nói:
- **Nói "Hi ESP"** → robot dừng nói, vào listening mode, sẵn sàng câu mới
- **Hoặc nói "Nĩ hảo Xiǎo Zhì"** (你好Tbot, đọc gần "ni hao shao chi") → cùng tác dụng
- Sau wake word → user nói câu mới → robot trả lời câu mới

## Verify đã chạy đúng firmware mới

Sau reboot, log server sẽ có line:
```
core.handle.abortHandle - INFO - Abort message received
```
khi user nói wake word (firmware gửi `{"type":"abort","reason":"wake_word_detected"}`).

Và Live API response sẽ bị cancel ngay (qua `voice_provider.interrupt()` đã sync vào container).
