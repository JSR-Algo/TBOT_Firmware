# TBOT Flappy SD Asset Pack

Copy `tbot_games` to the SD card root so paths resolve as `/sdcard/tbot_games/...`.

Assets are original generated BMP files. Magenta RGB(255,0,255) is used as the transparency key for sprites if the firmware implements color-key masking. If not, primitive fallback should still work.

## Sound Effects

The `sfx/` folder contains original generated WAV files: mono, signed 16-bit PCM, 24000 Hz. They are intentionally short and small for embedded playback. If firmware cannot play WAV from SD yet, keep game visuals working and ignore missing/unsupported sound files.
