#include "blufi_staged_wifi_credentials.h"

#include <cassert>
#include <thread>
#include <type_traits>

int main() {
    static_assert(!std::is_copy_constructible_v<BlufiStagedWifiCredentials::Snapshot>);
    static_assert(std::is_move_constructible_v<BlufiStagedWifiCredentials::Snapshot>);

    BlufiStagedWifiCredentials staged;
    const auto ssid_a = staged.UpdateSsid("ssid-a");
    assert(!staged.Claim(ssid_a).has_value());
    const auto candidate_a = staged.UpdatePassword("password-a");
    assert(staged.FallbackEpoch() == candidate_a);

    const auto password_b = staged.UpdatePassword("password-b");
    assert(!staged.Claim(candidate_a).has_value());
    assert(!staged.Claim(password_b).has_value());
    const auto candidate_b = staged.UpdateSsid("ssid-b");
    const auto claimed_b = staged.Claim(candidate_b);
    assert(claimed_b.has_value());
    assert(claimed_b->ssid == "ssid-b");
    assert(claimed_b->password == "password-b");
    assert(staged.StagedByteCountForTesting() == 0);
    assert(staged.FallbackEpoch() == 0);
    assert(!staged.Claim(candidate_b).has_value());

    const auto replacement_password = staged.UpdatePassword("password-c");
    assert(replacement_password != candidate_b);
    assert(!staged.Claim(replacement_password).has_value());
    const auto replacement_epoch = staged.UpdateSsid("ssid-c");
    const auto replacement = staged.Claim(replacement_epoch);
    assert(replacement.has_value());
    assert(replacement->ssid == "ssid-c");
    assert(replacement->password == "password-c");
    assert(staged.StagedByteCountForTesting() == 0);

    BlufiStagedWifiCredentials password_first;
    const auto password_only = password_first.UpdatePassword("password-first");
    assert(password_first.UpdatePassword("password-first") == password_only);
    assert(!password_first.Claim(password_only).has_value());
    const auto password_first_complete = password_first.UpdateSsid("ssid-second");
    const auto password_first_claim = password_first.Claim(password_first_complete);
    assert(password_first_claim.has_value());
    assert(password_first_claim->ssid == "ssid-second");
    assert(password_first_claim->password == "password-first");
    assert(password_first.StagedByteCountForTesting() == 0);

    staged.Invalidate();
    assert(!staged.ClaimCurrent().has_value());

    for (int iteration = 0; iteration < 1000; ++iteration) {
        BlufiStagedWifiCredentials concurrent;
        concurrent.UpdateSsid("old");
        const auto old_epoch = concurrent.UpdatePassword("old-password");
        std::thread replacement([&]() {
            concurrent.UpdatePassword("new-password");
            concurrent.UpdateSsid("new");
        });
        std::thread stale_fallback([&]() {
            const auto stale = concurrent.Claim(old_epoch);
            if (stale.has_value()) {
                assert(stale->ssid == "old");
                assert(stale->password == "old-password");
            }
        });
        replacement.join();
        stale_fallback.join();
        const auto current = concurrent.ClaimCurrent();
        if (current.has_value()) {
            assert(current->ssid == "new");
            assert(current->password == "new-password");
        }
    }
}
