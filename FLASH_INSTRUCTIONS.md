# TBOT firmware flash — Wake-word barge-in enabled

Đã build firmware mới với:

- `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y` — wake-word luôn active
- `application.cc:kDeviceStateSpeaking` — explicitly enable wake-word in realtime mode
- Wake words active: **"Hi ESP"** (English) và **"Nĩ hảo Xiǎo Zhì"** (Chinese, "你好小智")

Binary mới ở `build/xiaozhi.bin` (2.5MB).

## Endpoint đang dùng để nạp code

- Web/admin: `https://admin.skylabs.vn`
- OTA: `https://ota.skylabs.vn/tbot/ota/`
- WS: `wss://ws.skylabs.vn/tbot/v1/`

Firmware lấy WS từ OTA JSON. Khi build/flash, `CONFIG_OTA_URL` đang trỏ tới OTA URL ở trên.

## Cách flash 1 — USB (nhanh nhất, ~30s)

1. Cắm cáp USB-C từ robot vào laptop
2. Trên macOS: kiểm tra device serial:
   ```bash
   ls /dev/cu.usb* /dev/cu.wch*
   ```
3. Flash:
   ```bash
   cd /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware
   source ~/esp/esp-idf/export.sh
   idf.py -p /dev/cu.usbserial-XXXX flash
   ```
   (thay XXXX bằng device path từ bước 2; thường là `/dev/cu.usbserial-110` hoặc tương tự)

4. Robot tự reboot, kết nối lại WiFi + server.

## Cách flash 2 — OTA qua manager-web (không cần cáp)

1. Mở manager-web admin UI (port 8002):
   ```
   https://admin.skylabs.vn
   ```
2. Vào tab **OTA / Firmware Management** (tên menu tùy ngôn ngữ)
3. Upload file `build/xiaozhi.bin`
4. Chọn target device: MAC `3c:0f:02:de:c2:e0` (robot)
5. Click "Push firmware" hoặc "OTA update"
6. Robot sẽ tự download + flash + reboot

## Sau khi flash

Robot có wake-word barge-in. Khi robot đang nói:
- **Nói "Hi ESP"** → robot dừng nói, vào listening mode, sẵn sàng câu mới
- **Hoặc nói "Nĩ hảo Xiǎo Zhì"** (你好小智, đọc gần "ni hao shao chi") → cùng tác dụng
- Sau wake word → user nói câu mới → robot trả lời câu mới

## Verify đã chạy đúng firmware mới

Sau reboot, log server sẽ có line:
```
core.handle.abortHandle - INFO - Abort message received
```
khi user nói wake word (firmware gửi `{"type":"abort","reason":"wake_word_detected"}`).

Và Live API response sẽ bị cancel ngay (qua `voice_provider.interrupt()` đã sync vào container).
