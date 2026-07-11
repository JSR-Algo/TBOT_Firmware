# TBOT Main 3.5 UI + Slave TTP223 UART Plan

This document is the implementation prompt for Claude or another coding agent. Read it before patching. The goal is to make the TBOT robot use the ESP32-S3 3.5-inch main board as a display/UI/chat/game device, while a separate slave ESP32 reads two TTP223 capacitive buttons and sends input events to the main board over UART.

## Repositories And Workspace

Workspace root:

```text
/Users/gwang_su/esp32s3ver1test
```

Main firmware repository:

```text
/Users/gwang_su/esp32s3ver1test
```

Slave firmware repository:

```text
/Users/gwang_su/esp32s3ver1test/TBOT-Servant-Firmware
```

The slave repo is intentionally inside the same workspace as the main repo. Treat it as a separate firmware project with its own `.git`.

## Product Goal

Build this flow:

```text
[ Slave ESP32 ]
  - Controls existing servos
  - Reads 2 TTP223 buttons: LEFT and RIGHT
  - Sends button events to Main through UART

[ Main ESP32-S3 LCDWiki ES3C35P 3.5 inch ]
  - Displays UI only; do not use screen touch for UI input
  - Receives abstract input events from Slave UART
  - Has an app menu with 2 apps:
      1. Chatbox
      2. Game
  - BOTH buttons held for 3 seconds always returns to menu
  - Game is a simple built-in game, preferably Flappy Bird style
  - SD card may provide game config/assets, but game code is compiled in firmware
```

Important: do not load executable/game code from SD card. SD is only for config/assets.

## Current Main Firmware Discovery

Main firmware is ESP-IDF C++.

Selected board:

```text
CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y
```

File:

```text
/Users/gwang_su/esp32s3ver1test/sdkconfig.defaults.local
```

Main board config:

```text
/Users/gwang_su/esp32s3ver1test/main/boards/lcdwiki-es3c35p/config.h
```

Main board implementation:

```text
/Users/gwang_su/esp32s3ver1test/main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc
```

Main display:

```text
Board: LCDWiki ES3C35P
Chip: ESP32-S3 N16R8
LCD: 3.5 inch 320x480 physical, UI logical 480x320 landscape
LCD driver: ST77922
LCD bus: QSPI
UI library: LVGL
Display class: St77922QspiDisplay -> LcdDisplay
```

Main display init happens in:

```text
LCDWikiES3C35PBoard::InitializeLcdDisplay()
```

Touch is already disabled in:

```text
LCDWikiES3C35PBoard::InitializeTouch()
```

Current code logs that touch is disabled and returns early. Keep this behavior unless the user explicitly asks otherwise. The desired UI input is UART events from slave, not LCD touch.

Main audio/chat/cloud application lives in:

```text
/Users/gwang_su/esp32s3ver1test/main/application.h
/Users/gwang_su/esp32s3ver1test/main/application.cc
```

Important main application points:

```text
Application::Initialize()
  - gets board display
  - calls display->SetupUI()
  - initializes audio service
  - starts audio service
  - initializes robot_uart_
  - starts network

Application::Run()
  - main event loop
  - handles chat/listening/audio/network/UI updates
```

Do not rewrite chatbox/audio/cloud. Wrap the existing behavior as `APP_CHATBOX`.

## Current Main UART Discovery

Main UART class:

```text
/Users/gwang_su/esp32s3ver1test/main/robot_uart.h
/Users/gwang_su/esp32s3ver1test/main/robot_uart.cc
```

Current main UART purpose:

```text
- Send JSON servo commands to the slave
- Optional ACK reader only logs lines
```

Current main UART config from `lcdwiki-es3c35p/config.h`:

```c
#define ROBOT_UART_NUM        UART_NUM_1
#define ROBOT_UART_TX_PIN     GPIO_NUM_43
#define ROBOT_UART_RX_PIN     GPIO_NUM_44
#define ROBOT_UART_BAUD_RATE  115200
#define ROBOT_UART_ALT_NUM    UART_NUM_1
#define ROBOT_UART_ALT_TX_PIN GPIO_NUM_44
#define ROBOT_UART_ALT_RX_PIN GPIO_NUM_43
#define ROBOT_UART_ACK_READER_ENABLED 1
```

Notes:

- Main has a primary UART profile TX=43 RX=44.
- Main also has an alternate reversed profile TX=44 RX=43.
- ACK/event reader is currently enabled.
- TTP223 events are parsed from explicit `EVT:*` / `SLAVE:*` lines.
- If one UART port is used for both TX/RX, avoid repeatedly switching pins while a reader task is active. Prefer a single confirmed TX/RX wiring profile for final deployment.

## Current Main SD Card Discovery

Main SD card is already implemented for this board.

Mount point:

```text
/sdcard
```

SD init:

```text
LCDWikiES3C35PBoard::InitializeSdCard()
```

Pins:

```c
#define SDCARD_SDMMC_CLK_PIN  GPIO_NUM_5
#define SDCARD_SDMMC_CMD_PIN  GPIO_NUM_4
#define SDCARD_SDMMC_D0_PIN   GPIO_NUM_6
#define SDCARD_SDMMC_D1_PIN   GPIO_NUM_7
#define SDCARD_SDMMC_D2_PIN   GPIO_NUM_2
#define SDCARD_SDMMC_D3_PIN   GPIO_NUM_3
```

Do not format SD automatically. The code already uses:

```c
.format_if_mount_failed = false
```

For the game, use SD only as optional input:

```text
/sdcard/tbot_games/manifest.json
/sdcard/tbot_games/flappy/game_config.json
/sdcard/tbot_games/flappy/bird.bmp
/sdcard/tbot_games/flappy/pipe.bmp
/sdcard/tbot_games/flappy/bg.bmp
```

If SD mount or file read fails, game must still run with primitive graphics.

## Current Slave Firmware Discovery

Slave firmware is ESP-IDF C.

Main file:

```text
/Users/gwang_su/esp32s3ver1test/TBOT-Servant-Firmware/main/main.c
```

Slave currently:

```text
- Initializes 3 servos
- Receives UART line input
- Parses JSON servo commands with cJSON
- Sweeps servo to target
- Sends JSON ACK back
```

Current slave UART:

```c
#define UART_PORT UART_NUM_0
#define UART_TX_PIN GPIO_NUM_43
#define UART_RX_PIN GPIO_NUM_44
#define UART_BAUD_RATE 115200
```

Current servo pins:

```c
#define LEFT_ARM_SERVO_GPIO  GPIO_NUM_12
#define RIGHT_ARM_SERVO_GPIO GPIO_NUM_13
#define HEAD_SERVO_GPIO      GPIO_NUM_11
```

Do not break existing servo behavior.

Important concern: slave currently uses `UART_NUM_0` on GPIO43/GPIO44. Depending on the exact slave chip/board, this may overlap with the console UART. If this causes flashing/logging problems, move slave communication to `UART_NUM_1` or another safe UART, but do not change without considering board pinout.

## TTP223 Hardware Assumptions

The slave will have two TTP223 capacitive touch modules:

```text
LEFT TTP223
RIGHT TTP223
```

Start with active HIGH:

```c
#define TTP_ACTIVE_HIGH 1
```

Most TTP223 modules output HIGH when touched in momentary/direct mode. If test logs show inverted behavior, change to:

```c
#define TTP_ACTIVE_HIGH 0
```

Use momentary/direct mode, not toggle/self-lock mode.

Candidate pins for the slave TTP223:

```c
#define BTN_LEFT_PIN  GPIO_NUM_4
#define BTN_RIGHT_PIN GPIO_NUM_5
```

These are candidates only. Before finalizing, confirm the exact slave board/chip and avoid:

- Servo pins: GPIO11, GPIO12, GPIO13
- UART pins: GPIO43, GPIO44
- Boot strap pins if they cause boot issues
- Pins used by flash/PSRAM/USB or board-specific peripherals

## Desired Input Semantics

Abstract input events:

```text
INPUT_LEFT
INPUT_RIGHT
INPUT_SELECT
INPUT_BACK_MENU
```

These must come from UART events sent by slave. Do not use screen touch input.

Button behavior:

```text
LEFT short press:
  - Menu: previous/toggle selection
  - Game: pause/restart depending on state
  - Chatbox: ToggleChatState (talk / stop); ignored during lesson

RIGHT short press:
  - Menu: next/toggle selection
  - Game: flap/jump
  - Chatbox: volume +10

BOTH short press under 700 ms:
  - Menu: select highlighted app
  - Game: start/restart
  - Chatbox: volume -10

BOTH hold 3000 ms:
  - Always switch Main back to APP_MENU from any mode

RIGHT hold 3000 ms:
  - EVT:RIGHT_HOLD_3S → EnterWifiConfigMode (change Wi-Fi, keep claim)
  - Ignored during lesson
  - Product path when BOOT is internal / case sealed
```

Conflict rule:

```text
If both buttons are down, do not send LEFT_CLICK or RIGHT_CLICK.
Only send BOTH_CLICK or MENU_HOLD_3S depending on hold duration.
MENU_HOLD_3S must be sent once per hold, not repeatedly.
```

## UART Protocol

Use text line protocol. Every message ends with `\n`.

Slave to Main:

```text
SLAVE:READY\n
BTN:LEFT:DOWN\n          optional debug
BTN:LEFT:UP\n            optional debug
BTN:RIGHT:DOWN\n         optional debug
BTN:RIGHT:UP\n           optional debug
EVT:LEFT_CLICK\n
EVT:RIGHT_CLICK\n
EVT:BOTH_CLICK\n
EVT:MENU_HOLD_3S\n
EVT:RIGHT_HOLD_3S\n
PONG\n
ERR:<reason>\n
```

Main to Slave:

```text
PING\n
MODE:MENU\n
MODE:CHATBOX\n
MODE:GAME\n
SERVO:ALL_STOP\n
```

Existing servo commands from Main to Slave are JSON lines such as:

```json
{"cmd":"servo","part":"left_arm","action":"raise","from":0,"to":60,"step":2,"delay_ms":20}
```

The slave must continue supporting these JSON servo lines. Add new protocol handling without removing JSON support:

```text
if line starts with "{":
  parse as existing servo JSON
else:
  parse as text protocol command such as PING or MODE:*
```

## Main Architecture To Add

Add an app manager in C++ style matching the repo. Suggested files:

```text
main/app_manager.h
main/app_manager.cc
```

Possible API:

```cpp
enum class AppMode {
    Menu = 0,
    Chatbox,
    Game,
};

void AppManagerInit();
void AppSwitchTo(AppMode mode);
AppMode AppGetMode();

void AppHandleInputLeft();
void AppHandleInputRight();
void AppHandleInputBothClick();
void AppHandleMenuHold3s();
```

Behavior:

```text
APP_MENU:
  - show TBOT menu
  - highlight Chatbox or Game
  - send MODE:MENU to slave if helper exists

APP_CHATBOX:
  - restore/show normal chatbox UI
  - keep existing audio/cloud/chat behavior
  - send MODE:CHATBOX to slave

APP_GAME:
  - show game screen
  - pause/avoid conflicting chat UI updates where needed
  - send MODE:GAME to slave
```

The simplest phase-1 approach is to let Chatbox remain mostly as it is and only overlay/switch to menu/game screens when needed. Be careful with LVGL object ownership and locking.

All UI mutations must run in the correct task/thread context. UART listener should not directly mutate LVGL objects. It should marshal work via:

```cpp
Application::GetInstance().Schedule(...)
```

or an equivalent safe main-task mechanism.

## Main UART Input Handling

Modify or extend `RobotUart` so Main can receive button events.

Current `AckReaderTask` only logs lines and is disabled. Replace/extend with a listener that parses:

```text
EVT:LEFT_CLICK
EVT:RIGHT_CLICK
EVT:BOTH_CLICK
EVT:MENU_HOLD_3S
SLAVE:READY
PONG
ERR:...
```

When line is received:

```cpp
if (line == "EVT:LEFT_CLICK") {
    Application::GetInstance().Schedule([] {
        AppHandleInputLeft();
    });
}

if (line == "EVT:MENU_HOLD_3S") {
    Application::GetInstance().Schedule([] {
        AppHandleMenuHold3s();
    });
}
```

Do not block UI/audio/game in UART reader task.

## UI Menu Requirements

The boot screen should become a menu eventually:

```text
TBOT

[ CHATBOX ]   [ GAME ]

LEFT/RIGHT: choose
BOTH: select
BOTH HOLD 3S: menu

Slave: OK/WAIT
SD: OK/FAIL
```

Since there are only two apps, both LEFT and RIGHT can toggle selection in phase 1. If more apps are added later, LEFT should be previous and RIGHT should be next.

No touch instructions such as tap, swipe, or touch coordinate.

## Game Requirements

First game should be simple, preferably Flappy Bird style:

```text
- bird affected by gravity
- RIGHT_CLICK = flap/jump
- LEFT_CLICK = pause or restart if game over
- BOTH_CLICK = start/restart
- BOTH_HOLD_3S = exit to menu
- run at around 20-30 FPS
- do not block UART listener or application event loop
```

Implement primitive graphics first. Asset loading from SD is optional and should be added after primitive game works.

Game config path:

```text
/sdcard/tbot_games/flappy/game_config.json
```

Example config:

```json
{
  "gravity": 1,
  "jump_velocity": -8,
  "pipe_speed": 3,
  "gap_size": 90,
  "fps": 30,
  "use_assets": true
}
```

If config is missing, use defaults.

## Slave Implementation Plan

Add TTP223 reading to slave while preserving servo code.

Suggested constants:

```c
#define BTN_LEFT_PIN        GPIO_NUM_4
#define BTN_RIGHT_PIN       GPIO_NUM_5
#define TTP_ACTIVE_HIGH     1
#define DEBOUNCE_MS         40
#define BOTH_CLICK_MAX_MS   700
#define MENU_HOLD_MS        3000
#define BUTTON_POLL_MS      20
```

Implement:

```text
button_gpio_init()
read_ttp_left()
read_ttp_right()
button_task()
uart_send_line()
```

Button task rules:

- Poll every 20 ms.
- Debounce stable state for about 40 ms.
- Use edge detection.
- When both are pressed, suppress individual clicks.
- Send `EVT:MENU_HOLD_3S\n` once after 3000 ms.
- On release before 700 ms, send `EVT:BOTH_CLICK\n`.
- For single press/release, send `EVT:LEFT_CLICK\n` or `EVT:RIGHT_CLICK\n`.
- On boot after UART init, send `SLAVE:READY\n`.

Avoid long blocking delays in button task. Servo sweeps currently block inside servo command handling; this is okay for existing behavior, but button task must be separate so touch still works while servo is moving.

## Safety And Non-Goals

Do not:

- Re-enable LCD touch for UI.
- Rewrite the existing chatbox/audio/cloud system.
- Break existing JSON servo protocol.
- Change servo pins unless explicitly required.
- Require SD card for game.
- Format SD card.
- Load executable code from SD.
- Send repeated `MENU_HOLD_3S` spam while holding buttons.
- Mutate LVGL from UART task directly.
- Use long blocking delays in UART reader/game loop/button task.

## Recommended Phase Order

Phase 1: Slave button events

```text
1. Add TTP223 GPIO config.
2. Add button task.
3. Send SLAVE:READY on boot.
4. Add PING -> PONG handling.
5. Keep JSON servo parser intact.
6. Log button state for test.
```

Acceptance:

```text
LEFT touch -> EVT:LEFT_CLICK
RIGHT touch -> EVT:RIGHT_CLICK
BOTH quick -> EVT:BOTH_CLICK
BOTH hold 3s -> EVT:MENU_HOLD_3S once
Servo JSON commands still work
```

Phase 2: Main UART event receiver

```text
1. Turn RobotUart ACK reader into event reader or add a new line listener.
2. Parse EVT:* lines.
3. Marshal into Application main task.
4. Keep sending servo commands working.
5. Add PING command helper if useful.
```

Acceptance:

```text
Main logs SLAVE:READY
Main logs/responds to EVT:LEFT_CLICK/RIGHT/BOTH/HOLD
Existing servo commands still work
```

Phase 3: Main app manager and menu

```text
1. Add AppMode enum.
2. Add menu UI with LVGL.
3. Boot to menu after display/audio init.
4. LEFT/RIGHT changes selected app.
5. BOTH_CLICK enters Chatbox or Game.
6. MENU_HOLD_3S returns to menu from any app.
```

Acceptance:

```text
Boot shows menu
2 buttons control menu
Chatbox can be selected
Menu can be restored with 3s hold
```

Phase 4: Game primitive

```text
1. Add simple Flappy Bird style screen.
2. Use LVGL timer/task safely.
3. RIGHT flap.
4. LEFT pause/restart.
5. BOTH start/restart.
6. HOLD 3s back to menu.
```

Acceptance:

```text
Game runs without SD
No UART input loss while game is running
Can switch menu <-> chatbox <-> game repeatedly
```

Phase 5: Optional SD config/assets

```text
1. Read /sdcard/tbot_games/flappy/game_config.json if present.
2. Use defaults if missing.
3. Add bitmap assets only after config works.
4. Keep fallback graphics.
```

## Questions To Confirm Before Final Hardware Patch

Ask or verify these before final pin changes:

```text
1. Exact slave board/chip: ESP32, ESP32-S3, ESP32-C3, etc.
2. Are GPIO4 and GPIO5 free and physically convenient on slave?
3. Is slave UART_NUM_0 acceptable, or should UART communication move to UART_NUM_1?
4. Actual wiring: Main TX44 -> Slave RX44? Or Main TX44 -> Slave RX43?
5. TTP223 modules are active HIGH or active LOW?
6. TTP223 modules are momentary/direct mode, not toggle mode?
```

## Final Desired Result

TBOT should boot to a menu on the ESP32-S3 3.5-inch screen. The screen touch is not used. The slave reads two TTP223 buttons and sends UART events. LEFT/RIGHT selects Chatbox or Game, BOTH_CLICK opens the selected app, and BOTH_HOLD_3S always returns to the menu. Chatbox keeps the existing voice/cloud behavior. Game is built into main firmware and can use SD config/assets when available, with a fallback if SD is missing.
