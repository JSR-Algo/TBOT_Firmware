#pragma once

#include <string>
#include <vector>

class SsidManager {
public:
    std::vector<std::pair<std::string, std::string>> saved;

    static SsidManager &GetInstance() {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string &ssid, const std::string &password) {
        saved.emplace_back(ssid, password);
    }

    void Reset() { saved.clear(); }
};
