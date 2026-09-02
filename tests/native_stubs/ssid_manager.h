#pragma once

#include <string>
#include <vector>

enum class SsidMutationResult {
    kApplied,
    kBusy,
    kInvalid,
    kPersistenceFailed,
};

class SsidManager {
public:
    std::vector<std::pair<std::string, std::string>> saved;
    SsidMutationResult next_add_result = SsidMutationResult::kApplied;

    static SsidManager &GetInstance() {
        static SsidManager instance;
        return instance;
    }

    SsidMutationResult AddSsid(const std::string &ssid,
                               const std::string &password) {
        const auto result = next_add_result;
        next_add_result = SsidMutationResult::kApplied;
        if (result != SsidMutationResult::kApplied) {
            return result;
        }
        saved.emplace_back(ssid, password);
        return SsidMutationResult::kApplied;
    }

    void Reset() {
        saved.clear();
        next_add_result = SsidMutationResult::kApplied;
    }
};
