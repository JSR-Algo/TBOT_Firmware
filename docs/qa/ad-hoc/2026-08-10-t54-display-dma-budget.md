# T5.4 prerequisite — LCD SPI DMA transfer budget

**Date:** 2026-08-10
**Branch:** `lesson-prod/t54-display-dma`
**Finding:** F-T54-19

## Repro

The mandatory full course/robot gate failed on firmware `main` (`436782e`):

```text
test_lcdwiki_es3c35p_matches_lcdwiki_panel_power_sequence
assert "DISPLAY_WIDTH * 80 * static_cast<int>(sizeof(uint16_t))" in board
```

`main` still configured `max_transfer_sz` as a whole 320x480 RGB565 frame.

## Root cause and fix

The LCD SPI driver already chunks a full cinematic frame while holding CS and
signals completion only after the final chunk. The full-frame DMA allocation was
therefore unnecessary; the PSRAM DMA flag is what permits cinematic transfers.
The oversized internal DMA allocation also reduced the heap available to BluFi.

The measured fix from `38b22b1` is isolated here: restore the 80-line transfer
budget and document the driver chunking behavior. No lesson wire contract changed.

## Verification

```bash
python3 -m pytest tests/test_lcdwiki_es3c35p_board.py -q
```

The physical robot already runs firmware built from the branch containing this
fix (`2.2.89`, compile time `2026-08-10T10:07:13Z`), so no additional flash is
needed for the T5.4 capture.

