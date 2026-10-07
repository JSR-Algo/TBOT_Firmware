# TBOT Firmware

## Hardware platform

## Board/target
- **SOC**: ESP32-S3 (3320kB SRAM, 16MB SPI FLASH, 8MB OSPI PSRAM)
- **Network**: WiFi, Bluetooth
- **Display**: IPS TFT 320x480(RGB), RGB565 color format, Driver IC ST77922, interface QSPI
- **Module**: LCD wiki es3c35p
- **Application size** 4MB on SPI FLASH
- **ESP-IDF version** v5.5.5

## Common build & run commands
Ensure the ESP-IDF environment is activate before run the command
- **Buid project** `idf.py build`
- **Full clean** `idf.py fullclean`

## Critiacal don'ts
- **NEVER** call input PSRAM variable for NVS API.
- **NEVER** Create the task stack on PSRAM that task has call to NVS API.

## Memory managent
- **Stack Allocation** Keep the stack on static PSRAM if task handle not call to NVS API
- **Avoid fragment** avoid the `malloc` and `free` if possible by using the static PSRAM

## Important Note for AI Assistant
- When generating the code, must consider to use the static PSRAM first.

