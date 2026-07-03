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
    std::string last_status;
    std::vector<std::pair<std::string, std::string>> chat_messages;  // (role, message)
    std::vector<std::string> lesson_captions;
    int set_emotion_calls = 0;
    int set_status_calls = 0;

    virtual void SetStatus(const char* status) {
        set_status_calls++;
        last_status = status ? status : "";
    }
    virtual void SetEmotion(const char* emotion) {
        set_emotion_calls++;
        last_emotion = emotion ? emotion : "";
    }
    virtual void SetChatMessage(const char* role, const char* message) {
        chat_messages.emplace_back(role ? role : "", message ? message : "");
    }
    virtual void ClearChatMessages() {
        chat_messages.clear();
    }
    virtual void SetLessonCaption(const char* message) {
        lesson_captions.emplace_back(message ? message : "");
    }
};
