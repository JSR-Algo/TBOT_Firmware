#include "tbot_connect_mapper.h"

// All copy/timeout/recovery values come from kTbotConnectStateSpecs (the
// authoritative 21-state contract). This file only decides WHICH contract row
// the live runtime is in; it never invents text or timeouts of its own.

const TbotConnectStateSpec* TbotConnectMapper::SpecFor(TbotConnectState state) {
    for (std::size_t i = 0; i < kTbotConnectStateSpecCount; ++i) {
        if (kTbotConnectStateSpecs[i].state == state) {
            return &kTbotConnectStateSpecs[i];
        }
    }
    // The table is exhaustive over the enum, but stay defensive.
    return &kTbotConnectStateSpecs[0];  // BOOT
}

const char* TbotConnectMapper::ScreenTextFor(TbotConnectState state) {
    return SpecFor(state)->screen_text;
}

TbotConnectState TbotConnectMapper::ResolveState(DeviceState device_state,
                                                 TbotClaimSubstate claim_substate,
                                                 TbotBleSubstate ble_substate,
                                                 bool backend_offline) {
    // 1) Claim sub-state takes priority over the coarse DeviceState because the
    //    claim flow is a side-channel that overlays Idle/Activating: a pending
    //    claim must surface "Allow connection?" even though DeviceState==Idle.
    switch (claim_substate) {
        case TbotClaimSubstate::AvailableStandby:
            return TbotConnectState::CLAIM_AVAILABLE;
        case TbotClaimSubstate::WaitingConfirm:
            return TbotConnectState::CLAIM_WAITING_CONFIRM;
        case TbotClaimSubstate::ConfirmTimeout:
            return TbotConnectState::CLAIM_CONFIRM_TIMEOUT;
        case TbotClaimSubstate::Confirmed:
            return TbotConnectState::CLAIM_CONFIRMED;
        case TbotClaimSubstate::Claimed:
            return TbotConnectState::CLAIMED;
        case TbotClaimSubstate::None:
            break;  // fall through to BLE / DeviceState mapping
    }

    // 2) BLE setup sub-state (only meaningful while configuring Wi-Fi).
    if (device_state == kDeviceStateWifiConfiguring) {
        switch (ble_substate) {
            case TbotBleSubstate::Advertising:
                return TbotConnectState::BLE_SETUP_ADVERTISING;
            case TbotBleSubstate::Timeout:
                return TbotConnectState::BLE_SETUP_TIMEOUT;
            case TbotBleSubstate::Off:
                // No BLE radio up -> Wi-Fi not configured / waiting for the app.
                return TbotConnectState::WIFI_NOT_CONFIGURED;
        }
    }

    // 3) Backend offline overlay: the 11-state engine drops to Idle on a ws/
    //    backend error and reconnects, so an offline Idle/Connecting is really
    //    the OFFLINE_RETRY state ("Server unavailable. Retrying...").
    if (backend_offline &&
        (device_state == kDeviceStateIdle ||
         device_state == kDeviceStateConnecting ||
         device_state == kDeviceStateListening ||
         device_state == kDeviceStateSpeaking)) {
        return TbotConnectState::OFFLINE_RETRY;
    }

    // 4) Runtime DeviceState -> connect state.
    switch (device_state) {
        case kDeviceStateStarting:
            return TbotConnectState::BOOT;
        case kDeviceStateActivating:
            // Activation runs OTA CheckVersion + bootstrap fetch.
            return TbotConnectState::BOOTSTRAP_FETCHING;
        case kDeviceStateConnecting:
            return TbotConnectState::BACKEND_CONNECTING;
        case kDeviceStateIdle:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
            // Backend session is up and serving -> ONLINE. Radio details are
            // reported live by WifiBoard status/heartbeat.
            return TbotConnectState::ONLINE;
        case kDeviceStateUpgrading:
            return TbotConnectState::OTA_UPDATING;
        case kDeviceStateFatalError:
            return TbotConnectState::ERROR_RECOVERABLE;
        case kDeviceStateAudioTesting:
            // Audio test is a sub-mode of Wi-Fi config; no setup radio yet.
            return TbotConnectState::WIFI_NOT_CONFIGURED;
        case kDeviceStateUnknown:
        default:
            return TbotConnectState::BOOT;
    }
}

// Secondary / fallback connect states that have NO independent runtime trigger
// in build-blufi and are therefore not produced by ResolveState() above:
//   * AP_SETUP_ACTIVE / AP_SETUP_TIMEOUT — SoftAP is compiled OUT
//     (CONFIG_USE_HOTSPOT_WIFI_PROVISIONING absent); BLE/BluFi is the path.
//   * QR_CODE_VISIBLE_FALLBACK / PAIRING_CODE_VISIBLE_FALLBACK — the QR/code
//     fallback is mobile-driven; the robot has no QR render path.
//   * WIFI_CONNECTING / WIFI_CONNECTED — driven by NetworkEvent callbacks in
//     application.cc, not by a steady-state DeviceState.
// They remain first-class rows in kTbotConnectStateSpecs (and are reachable via
// SpecFor / ScreenTextFor) so copy/timeout/recovery stay defined; this function
// simply never selects them in this build. Listed explicitly so the dormant set
// is documented rather than silently dropped. [[maybe_unused]] keeps the build
// warning-free under -Werror without forcing a synthetic consumer.
[[maybe_unused]] static constexpr TbotConnectState kTbotDormantConnectStates[] = {
    TbotConnectState::AP_SETUP_ACTIVE,
    TbotConnectState::AP_SETUP_TIMEOUT,
    TbotConnectState::QR_CODE_VISIBLE_FALLBACK,
    TbotConnectState::PAIRING_CODE_VISIBLE_FALLBACK,
    TbotConnectState::WIFI_CONNECTING,
    TbotConnectState::WIFI_CONNECTED,
};

const TbotConnectStateSpec* TbotConnectMapper::Resolve(DeviceState device_state,
                                                       TbotClaimSubstate claim_substate,
                                                       TbotBleSubstate ble_substate,
                                                       bool backend_offline) {
    return SpecFor(ResolveState(device_state, claim_substate, ble_substate, backend_offline));
}
