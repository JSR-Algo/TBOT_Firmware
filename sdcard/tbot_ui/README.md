# TBOT UI Menu Assets

Copy `tbot_ui` to the SD card root so paths resolve as `/sdcard/tbot_ui/...`.

Sound files are original generated WAV files: mono, signed 16-bit PCM, 24000 Hz.

`menu_config.json` also includes Vietnamese menu strings. They are ASCII-free Vietnamese in meaning, but currently without accents for safer embedded font compatibility. If the selected LVGL font supports Vietnamese glyphs, Claude can switch to accented strings in firmware.
