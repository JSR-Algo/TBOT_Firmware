#include <cassert>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

#include "blufi_advertising_ledger.h"

namespace {

using Kind = TbotBlufiAdvertisingLedger::CallbackKind;

void TestAtomicFallbackClaimDoesNotAdoptNewEpoch() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto first = ledger.BeginCompact(0x03);
    assert(first.has_value());
    const auto fallback = ledger.ClaimDefaultFallback();
    assert(fallback.has_value());
    assert(fallback->epoch == first->epoch);

    const auto second = ledger.BeginCompact(0x03);
    assert(!second.has_value());
    assert(fallback->epoch == ledger.ActiveEpoch());
}

void TestFallbackStartAndInvalidateAreLinearizable() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto first = ledger.BeginCompact(0x01);
    assert(first.has_value());

    std::atomic<bool> go{false};
    std::optional<TbotBlufiAdvertisingLedger::Owner> fallback;
    std::optional<TbotBlufiAdvertisingLedger::CompactSubmission> restart;
    std::thread fallback_thread([&]() {
        while (!go.load()) std::this_thread::yield();
        fallback = ledger.ClaimDefaultFallback();
    });
    std::thread restart_thread([&]() {
        while (!go.load()) std::this_thread::yield();
        restart = ledger.BeginCompact(0x01);
    });
    std::thread invalidate_thread([&]() {
        while (!go.load()) std::this_thread::yield();
        ledger.Invalidate();
    });
    go.store(true);
    fallback_thread.join();
    restart_thread.join();
    invalidate_thread.join();

    assert(!restart.has_value());
    if (fallback) {
        assert(fallback->epoch == first->epoch);
    }
    assert(!ledger.BeginCompact(0x01).has_value());
}

void TestTypedStartOwnershipDoesNotCrossConsume() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto first = ledger.BeginCompact(0x01);
    assert(first.has_value());
    assert(ledger.Cancel(first->adv_data));
    const auto fallback = ledger.ClaimDefaultFallback();
    assert(fallback.has_value());
    const auto default_transfer = ledger.CompleteDefaultConfig(true);
    assert(default_transfer.forward_to_idf);
    assert(default_transfer.owner.kind == Kind::kDefaultStart);

    const auto default_start = ledger.CompleteStart(true);
    assert(default_start.owner.kind == Kind::kDefaultStart);
    assert(!default_start.compact_completed);

    const auto second = ledger.BeginCompact(0x01);
    assert(second.has_value());
    const auto raw = ledger.CompleteCompactConfig(Kind::kCompactAdvData, true);
    assert(raw.start_compact);
    const auto compact_start = ledger.CompleteStart(true);
    assert(compact_start.owner.kind == Kind::kCompactStart);
    assert(compact_start.compact_completed);
}

void TestNullCallbacksDoNotPop() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto compact = ledger.BeginCompact(0x01);
    assert(compact.has_value());
    assert(ledger.Pending(Kind::kCompactAdvData) == 1);
    assert(!ledger.CompleteCompactConfig(Kind::kCompactAdvData, false).owned);
    assert(ledger.Pending(Kind::kCompactAdvData) == 1);
}

void TestDroppedCallbackPoisonsChannelUntilHostReset() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto first = ledger.BeginCompact(0x01);
    assert(first.has_value());
    ledger.Invalidate();
    assert(!ledger.BeginCompact(0x01).has_value());
    const uint32_t incarnation = ledger.HostIncarnation();
    ledger.ResetAfterSuccessfulHostDeinit();
    assert(ledger.HostIncarnation() == incarnation + 1);
    assert(ledger.ActivateAfterSuccessfulHostInit());
    assert(ledger.BeginCompact(0x01).has_value());
}

void TestExactCancellationPreservesOtherOwnership() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto compact = ledger.BeginCompact(0x03);
    assert(compact.has_value());
    assert(ledger.Cancel(compact->adv_data));
    assert(ledger.Pending(Kind::kCompactAdvData) == 0);
    assert(ledger.Pending(Kind::kCompactScanResponse) == 1);
    assert(ledger.Cancel(compact->scan_response));
}

void TestSynchronousSubmissionFailureCancelsExactOwner() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    bool default_submitted = false;
    const auto compact = ledger.BeginCompactAndSubmit(
        0x03,
        []() { return false; },
        []() { assert(false); return true; },
        [&]() { default_submitted = true; });
    assert(compact.has_value());
    assert(default_submitted);
    assert(ledger.Pending(Kind::kCompactAdvData) == 0);
    assert(ledger.Pending(Kind::kCompactScanResponse) == 0);
    assert(ledger.Pending(Kind::kDefaultAdvData) == 1);
}

void TestNormalEitherOrderSuccess() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    const auto compact = ledger.BeginCompact(0x03);
    assert(compact.has_value());
    const auto scan = ledger.CompleteCompactConfig(Kind::kCompactScanResponse, true);
    assert(scan.owned && !scan.start_compact);
    const auto adv = ledger.CompleteCompactConfig(Kind::kCompactAdvData, true);
    assert(adv.owned && adv.start_compact);
    const auto start = ledger.CompleteStart(true);
    assert(start.compact_completed);
}

void TestActivationPrecedesImmediateInitFinishSubmission() {
    TbotBlufiAdvertisingLedger ledger;
    ledger.Invalidate();
    bool init_finish_accepted = false;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    auto synchronous_profile_init = [&]() {
        init_finish_accepted = ledger.BeginCompact(0x01).has_value();
    };
    synchronous_profile_init();
    assert(init_finish_accepted);
}

void TestOnlyOwnedDefaultConfigForwardsWithStateUnlocked() {
    TbotBlufiAdvertisingLedger ledger;
    assert(ledger.ActivateAfterSuccessfulHostInit());
    int forwards = 0;

    ledger.CompleteDefaultConfigAndForward(false, [&]() { ++forwards; });
    assert(forwards == 0);

    const auto compact = ledger.BeginCompact(0x01);
    assert(compact.has_value());
    assert(ledger.Cancel(compact->adv_data));
    assert(ledger.ClaimDefaultFallback().has_value());
    ledger.CompleteDefaultConfigAndForward(true, [&]() {
        assert(ledger.ActiveEpoch() == compact->epoch);
        ++forwards;
    });
    assert(forwards == 1);

    ledger.Invalidate();
    ledger.CompleteDefaultConfigAndForward(true, [&]() { ++forwards; });
    assert(forwards == 1);
}

}  // namespace

int main() {
    TestAtomicFallbackClaimDoesNotAdoptNewEpoch();
    TestFallbackStartAndInvalidateAreLinearizable();
    TestTypedStartOwnershipDoesNotCrossConsume();
    TestNullCallbacksDoNotPop();
    TestDroppedCallbackPoisonsChannelUntilHostReset();
    TestExactCancellationPreservesOtherOwnership();
    TestSynchronousSubmissionFailureCancelsExactOwner();
    TestNormalEitherOrderSuccess();
    TestActivationPrecedesImmediateInitFinishSubmission();
    TestOnlyOwnedDefaultConfigForwardsWithStateUnlocked();
    std::cout << "PASS: BluFi advertising ledger host model\n";
    return 0;
}
