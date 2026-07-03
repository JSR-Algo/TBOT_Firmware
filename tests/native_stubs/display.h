#pragma once

#include <string>

class Display {
public:
    std::string last_role;
    std::string last_message;

    void SetChatMessage(const char *role, const char *message) {
        last_role = role ? role : "";
        last_message = message ? message : "";
    }
    void SetLessonCaption(const char *message) {
        SetChatMessage("assistant", message);
    }
};
