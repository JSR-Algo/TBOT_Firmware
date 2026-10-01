#pragma once

// Warning log switches, off by default. Set to 1 here (or pass -D...=1) to enable again.

// ESP_LOGW in application_protocol_json.cc, application_state_change.cc and
// application_chat_receiver.cc (tag "Application").
#ifndef TBOT_APPLICATION_WARN_LOG
#define TBOT_APPLICATION_WARN_LOG 0
#endif

// ESP_LOGW in display/lcd_emotion.cc (tag "LcdDisplay").
#ifndef TBOT_LCD_DISPLAY_WARN_LOG
#define TBOT_LCD_DISPLAY_WARN_LOG 0
#endif

#ifndef TBOT_AUDIO_INFO_LOG
#define TBOT_AUDIO_INFO_LOG 0
#endif

// esp-sr "AFE" tag, e.g. "Ringbuffer of AFE is empty". When 0, the AFE tag is set to ERROR.
#ifndef TBOT_AFE_WARN_LOG
#define TBOT_AFE_WARN_LOG 0
#endif
