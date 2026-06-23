#pragma once
// Host stub for main/display/display.h. Recording, polymorphic base so the renderer's
// dynamic_cast<LvglDisplay*>(Display*) resolves correctly (LvglDisplay derives from it)
// and so a plain Display (non-LVGL board) takes the caption-only path.
#include <string>
#include <vector>

class Display {
public:
    virtual ~Display() = default;

    std::string last_emotion;
    std::vector<std::pair<std::string, std::string>> chat_messages;  // (role, message)
    int set_emotion_calls = 0;

    virtual void SetEmotion(const char* emotion) {
        set_emotion_calls++;
        last_emotion = emotion ? emotion : "";
    }
    virtual void SetChatMessage(const char* role, const char* message) {
        chat_messages.emplace_back(role ? role : "", message ? message : "");
    }
};
