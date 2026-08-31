#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Reads scan records into bounded stack storage, then materializes them outside
// the ESP driver API. Allocation failure is contained and reported instead of
// unwinding through the WiFi event callback.
template <typename Record, size_t MaxRecords>
class BlufiWifiScanResultCollector {
public:
    struct Result {
        bool cleanup_proven = false;
        bool materialization_failed = false;
        uint16_t record_count = 0;
    };

    template <typename Read, typename Clear, typename Materialize>
    static Result Collect(uint16_t reported_count, Read&& read, Clear&& clear,
                          Materialize&& materialize) noexcept {
        Result result;
        std::array<Record, MaxRecords> records{};
        uint16_t count = std::min<uint16_t>(
            reported_count, static_cast<uint16_t>(MaxRecords));
        try {
            if (!read(&count, records.data())) {
                result.cleanup_proven = clear();
                return result;
            }
            result.cleanup_proven = true;
            try {
                materialize(records.data(), count);
                result.record_count = count;
            } catch (...) {
                result.materialization_failed = true;
            }
        } catch (...) {
            try {
                result.cleanup_proven = clear();
            } catch (...) {
                result.cleanup_proven = false;
            }
        }
        return result;
    }
};
