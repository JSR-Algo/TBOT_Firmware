#pragma once
// Host stub for main/assets.h. The lesson renderer probes the flashed (build-time)
// asset image via Assets::GetInstance().GetAssetData(name, ptr, size). On the host we
// drive it from a settable table so the AssetAvailable() fallback rung is reachable.
#include <cstddef>
#include <map>
#include <string>
#include <vector>

class Assets {
public:
    static Assets& GetInstance() {
        static Assets inst;
        return inst;
    }

    // Settable test table: name -> backing bytes. Present + non-empty => available.
    std::map<std::string, std::vector<unsigned char>> table;

    bool GetAssetData(const std::string& name, void*& ptr, size_t& size) {
        auto it = table.find(name);
        if (it == table.end() || it->second.empty()) {
            ptr = nullptr;
            size = 0;
            return false;
        }
        ptr = it->second.data();
        size = it->second.size();
        return true;
    }

    void Clear() { table.clear(); }
};
