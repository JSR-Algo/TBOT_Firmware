# LCDWiki Safe Speaker Volume Design

## Goal

Prevent audible clipping on the LCDWiki ES3C35P speaker path while preserving
the loudest previously validated operating point for its ES8311 codec,
SC8002B amplifier, and 4 ohm 3 W speaker.

## Context

The LCDWiki board starts at volume 92, which is documented in the board code as
its practical loudness sweet spot. Runtime controls and persisted settings can
still request volume 100. The active ES8311 volume curve maps volume 100 to
+2 dB, and live device logs confirmed playback at that setting while the user
heard distortion.

## Design

The LCDWiki-specific codec subclass will clamp every requested output volume to
the inclusive range 0 through 92 before passing it to `Es8311AudioCodec`.
Volume 92 remains the board's safe hardware maximum and startup default.

The clamp applies uniformly to startup restoration, MCP volume commands, and
future callers of `SetOutputVolume`. The applied value, rather than an unsafe
request, is stored through the existing audio settings path. A warning log will
record both requested and applied values whenever limiting occurs.

The shared ES8311 codec implementation and its volume curve will not change,
because other boards use it and have different amplifier and speaker paths.
Input gain, microphone processing, PCM playback, and PA polarity are outside
this change.

## Verification

A host-side regression test will verify that the LCDWiki codec overrides
`SetOutputVolume`, caps requests at 92, and routes the capped value through the
base codec implementation. Existing LCDWiki board tests and the firmware build
must pass.

After flashing the connected ESP32-S3, serial logs must show playback volume no
higher than 92 even if the stored or requested value was 100. The device must
continue to enter speaking state and codec writes must return `ESP_OK`.

Audible distortion is ultimately a physical observation. The acceptance check
is that speech at the new maximum is clear on the connected 4 ohm 3 W speaker;
if it still distorts, the next calibration step is to lower only the board cap
in small increments without changing the global codec curve.
