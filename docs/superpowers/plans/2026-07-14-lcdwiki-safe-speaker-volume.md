# LCDWiki Safe Speaker Volume Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep every LCDWiki ES3C35P speaker-volume request at or below the validated codec level 92 so the connected 4 ohm 3 W speaker can run at its loudest safe setting without the volume-100 clipping observed on hardware.

**Architecture:** Add the safety boundary only to the `LcdWikiAudioCodec` subclass by overriding `SetOutputVolume` and clamping requests before delegating to `Es8311AudioCodec`. Preserve the shared ES8311 volume curve and all microphone/playback behavior for other boards. Prove the board contract with the existing source-level pytest suite, then compile, flash, and inspect live serial logs.

**Tech Stack:** C++17, ESP-IDF 5.5, ES8311 codec, pytest source-contract tests, ESP USB Serial/JTAG.

---

## File Structure

- Modify `tests/test_lcdwiki_es3c35p_board.py`: add the failing board-specific volume-cap contract.
- Modify `main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc`: enforce the safe maximum in the LCDWiki codec subclass.
- No shared codec, configuration, or audio-pipeline files change.

### Task 1: Enforce the LCDWiki hardware volume ceiling

**Files:**
- Modify: `tests/test_lcdwiki_es3c35p_board.py:246`
- Modify: `main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc:206`

- [ ] **Step 1: Write the failing board-contract test**

Add this test after `test_lcdwiki_es3c35p_uses_lcdwiki_audio_and_uart_pins`:

```python
def test_lcdwiki_es3c35p_caps_output_volume_at_safe_hardware_maximum():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")

    assert "void SetOutputVolume(int volume) override" in board
    assert "const int safe_volume = std::clamp(volume, 0, kLcdWikiOutputVolume);" in board
    assert '"LCDWiki output volume limited requested=%d applied=%d"' in board
    assert "Es8311AudioCodec::SetOutputVolume(safe_volume);" in board
```

- [ ] **Step 2: Run the new test and verify RED**

Run:

```bash
python3 -m pytest tests/test_lcdwiki_es3c35p_board.py::test_lcdwiki_es3c35p_caps_output_volume_at_safe_hardware_maximum -q
```

Expected: FAIL because the LCDWiki codec does not yet override `SetOutputVolume`.

- [ ] **Step 3: Add the minimal LCDWiki-specific clamp**

Add this method to the public section of `LcdWikiAudioCodec`, immediately before `Start()`:

```cpp
void SetOutputVolume(int volume) override {
    const int safe_volume = std::clamp(volume, 0, kLcdWikiOutputVolume);
    if (safe_volume != volume) {
        ESP_LOGW(TAG, "LCDWiki output volume limited requested=%d applied=%d",
                 volume, safe_volume);
    }
    Es8311AudioCodec::SetOutputVolume(safe_volume);
}
```

Keep the existing startup call to `SetOutputVolume(kLcdWikiOutputVolume)`. Dynamic MCP requests now dispatch through the override, and the base implementation persists only the safe value.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run:

```bash
python3 -m pytest tests/test_lcdwiki_es3c35p_board.py::test_lcdwiki_es3c35p_caps_output_volume_at_safe_hardware_maximum -q
```

Expected: `1 passed`.

- [ ] **Step 5: Run the complete LCDWiki board test file**

Run:

```bash
python3 -m pytest tests/test_lcdwiki_es3c35p_board.py -q
```

Expected: all tests pass with no failures.

- [ ] **Step 6: Commit the tested implementation**

```bash
git add tests/test_lcdwiki_es3c35p_board.py main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc
git commit -m "fix(audio): cap LCDWiki speaker at safe volume"
```

### Task 2: Build the production LCDWiki firmware

**Files:**
- Verify: `sdkconfig`
- Build output: `build/xiaozhi.bin`

- [ ] **Step 1: Confirm the selected board and clean source state**

Run:

```bash
rg '^CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y$' sdkconfig
git status --short
```

Expected: the LCDWiki configuration line is present; only expected plan-tracking changes, if any, are shown.

- [ ] **Step 2: Load ESP-IDF and build**

Run:

```bash
source "$HOME/esp/esp-idf/export.sh" && idf.py build
```

Expected: `Project build complete` and `build/xiaozhi.bin` is generated.

- [ ] **Step 3: Re-run the focused regression after the build**

Run:

```bash
python3 -m pytest tests/test_lcdwiki_es3c35p_board.py -q
```

Expected: all LCDWiki board tests pass.

### Task 3: Flash and validate the connected speaker path

**Files:**
- Flash input: `build/xiaozhi.bin`
- Hardware port: `/dev/cu.usbmodem1101`

- [ ] **Step 1: Confirm the connected ESP USB Serial/JTAG port is free**

Run:

```bash
ls -l /dev/cu.usbmodem1101
lsof /dev/cu.usbmodem1101
```

Expected: the device node exists and `lsof` reports no process holding it.

- [ ] **Step 2: Flash the built firmware**

Run:

```bash
source "$HOME/esp/esp-idf/export.sh" && idf.py -p /dev/cu.usbmodem1101 flash
```

Expected: flash verification succeeds and the ESP32-S3 resets.

- [ ] **Step 3: Capture startup and playback logs**

Run:

```bash
python3 -c 'import serial,time,sys; s=serial.Serial("/dev/cu.usbmodem1101",115200,timeout=0.5); end=time.time()+30
while time.time()<end:
 b=s.read(4096)
 if b: sys.stdout.buffer.write(b); sys.stdout.buffer.flush()
s.close()'
```

Expected during playback:

```text
Es8311AudioCodec: es8311_write ... ret=ESP_OK(0) ... volume=92
```

No `es8311_write` line may report a volume greater than 92. If a runtime caller requests 100, expect:

```text
LCDWikiES3C35P: LCDWiki output volume limited requested=100 applied=92
```

- [ ] **Step 4: Perform the physical acceptance check**

Trigger normal TTS playback and listen at the maximum available setting. Speech must remain clear without the buzzing or breakup heard at codec volume 100. If distortion remains, record the exact content and state, then lower only `kLcdWikiOutputVolume` in a new calibrated change; do not alter the global ES8311 curve during this task.

- [ ] **Step 5: Record final repository state**

Run:

```bash
git status --short
git log -3 --oneline
```

Expected: no uncommitted implementation changes and the audio-cap commit appears after the design/plan documentation commits.
