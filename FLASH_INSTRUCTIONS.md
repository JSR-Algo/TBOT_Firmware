# TBOT firmware flash — LCDWiki production build

Build production cho robot LCDWiki ES3C35P dùng cấu hình mặc định đã được commit:

- `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`
- `# CONFIG_MBEDTLS_HARDWARE_AES is not set`
- `CONFIG_OTA_URL="https://tbot-backend-8wmh.onrender.com/tbot/ota/"`
- `CONFIG_WEBSOCKET_URL=""` — WS production lấy từ OTA/bootstrap hoặc build-time injection.

## Endpoint đang dùng để nạp code

- API/bootstrap seed: `https://tbot-backend-8wmh.onrender.com/v1`
- OTA seed: `https://tbot-backend-8wmh.onrender.com/tbot/ota/`
- WS: do OTA/bootstrap trả về từ managed robot-server endpoint. Không commit quick-tunnel host.

## Cách flash 1 — USB (nhanh nhất, ~30s)

> Root `CMakeLists.txt` đã set `SDKCONFIG_DEFAULTS` để plain `idf.py build` nạp
> `sdkconfig.defaults.local`. Với flash fleet, vẫn dùng script dưới đây vì nó xóa
> `sdkconfig`, build lại sạch, rồi hard-gate board type trước khi nạp.

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

## Plain build verification

```bash
cd /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware
rm -f sdkconfig
idf.py set-target esp32s3
idf.py build
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

## Cách flash 2 — OTA qua manager-web (không cần cáp)

OTA chỉ dùng sau khi managed OTA/admin endpoint đã được user provision và unit đã
chạy đúng board. Upload `build/xiaozhi.bin`, chọn đúng MAC, rồi push firmware.

## Sau khi flash

Robot phải hiện mặt trên LCDWiki và chạy một cuộc hội thoại đầy đủ không rớt
`Máy chủ không khả dụng`.

## Verify đã chạy đúng firmware mới

Sau reboot, kiểm tra:

- LCD có face, không màn hình đen.
- `python3 scripts/assert_lcdwiki_prod_config.py sdkconfig` in `LCDWiki production build config OK`.
- Hội thoại kéo dài qua WSS không có AES internal-SRAM OOM / `Máy chủ không khả dụng`.
