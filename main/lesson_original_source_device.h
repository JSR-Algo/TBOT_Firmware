#ifndef LESSON_ORIGINAL_SOURCE_DEVICE_H
#define LESSON_ORIGINAL_SOURCE_DEVICE_H

class LcdDisplay;

namespace tbot {

// Device wiring of the renderer-v6 runtime (ESP_PLATFORM, CONFIG_TBOT_LESSON_RENDERER_V6):
// card text with the board's LVGL text font, frames through the lesson framebuffer
// present. Advertised only with CONFIG_TBOT_LESSON_RENDERER_V6_ADVERTISE.
bool InitializeProductionOriginalSourceDevice(::LcdDisplay* display);

}  // namespace tbot

#endif
