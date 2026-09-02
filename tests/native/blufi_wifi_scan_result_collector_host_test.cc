#include "../../main/boards/common/blufi_wifi_scan_result_collector.h"

#include <cassert>
#include <new>

namespace {

struct Record {
    int value = 0;
};

using Collector = BlufiWifiScanResultCollector<Record, 32>;

void AllocationFailureAfterBoundedReadCompletesCleanup() {
    int read_calls = 0;
    int clear_calls = 0;
    const auto result = Collector::Collect(
        65535,
        [&](uint16_t* count, Record* records) {
            ++read_calls;
            assert(*count == 32);
            records[0].value = 7;
            *count = 1;
            return true;
        },
        [&]() {
            ++clear_calls;
            return true;
        },
        [](const Record*, uint16_t) { throw std::bad_alloc(); });
    assert(read_calls == 1);
    assert(clear_calls == 0);
    assert(result.cleanup_proven);
    assert(result.materialization_failed);
    assert(result.record_count == 0);
}

void ReadAndCleanupFailureRequiresExactRecoveryDebt() {
    const auto result = Collector::Collect(
        8,
        [](uint16_t*, Record*) { return false; },
        []() { return false; },
        [](const Record*, uint16_t) {});
    assert(!result.cleanup_proven);
    assert(!result.materialization_failed);
    assert(result.record_count == 0);
}

void SuccessfulReadMaterializesBoundedCount() {
    uint16_t materialized = 0;
    const auto result = Collector::Collect(
        40,
        [](uint16_t* count, Record*) {
            assert(*count == 32);
            *count = 12;
            return true;
        },
        []() { return true; },
        [&](const Record*, uint16_t count) { materialized = count; });
    assert(result.cleanup_proven);
    assert(!result.materialization_failed);
    assert(result.record_count == 12);
    assert(materialized == 12);
}

}  // namespace

int main() {
    AllocationFailureAfterBoundedReadCompletesCleanup();
    ReadAndCleanupFailureRequiresExactRecoveryDebt();
    SuccessfulReadMaterializesBoundedCount();
}
