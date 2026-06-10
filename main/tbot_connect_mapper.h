#ifndef TBOT_CONNECT_MAPPER_H_
#define TBOT_CONNECT_MAPPER_H_

#include "device_state.h"
#include "tbot_connect_state.h"

// ---------------------------------------------------------------------------
// TBOT connect runtime FSM mapper
//
// The 21 TbotConnectState entries in tbot_connect_state.h are the authoritative
// contract for screen text / timeout / retry / BLE-AP / next / recovery. The
// real engine remains DeviceStateMachine (11 DeviceState values). This mapper
// is the single translation point: it takes the live runtime DeviceState plus
// the claim and BLE sub-states and returns the matching TbotConnectStateSpec so
// that display copy, timeout and recovery never drift from the contract table.
//
// LOCKED DECISION (2026-06-04): every one of the 21 states is runtime-driven.
// AP_SETUP_ACTIVE / AP_SETUP_TIMEOUT are DEFINED-BUT-DORMANT in build-blufi:
// SoftAP (CONFIG_USE_HOTSPOT_WIFI_PROVISIONING) is compiled OUT, so this mapper
// never resolves to them in this build. They remain mapped so the same code is
// correct if the AP path is ever compiled in. The QR / PAIRING_CODE fallback
// states are mobile-driven (no firmware render path) and likewise dormant here.
// ---------------------------------------------------------------------------

// Claim-side sub-state the Application tracks alongside DeviceState.
enum class TbotClaimSubstate {
    None,            // No claim activity.
    AvailableStandby, // Unowned -> "Ready to connect"; bounded poll running.
    WaitingConfirm,  // pending_tbot_claim_.active -> "Allow connection?".
    ConfirmTimeout,  // expires_at elapsed locally -> "Setup expired".
    Confirmed,       // confirm POST succeeded -> "Connection allowed".
    Claimed,         // credentials persisted in NVS.
};

// BLE-side sub-state mirrored from Blufi::BleState (kept decoupled so this
// header has no BLE dependency and so AP-only builds still compile).
enum class TbotBleSubstate {
    Off,
    Advertising,
    Timeout,
};

class TbotConnectMapper {
public:
    // Resolve the runtime DeviceState + claim/BLE sub-state to the contract
    // spec. Never returns nullptr: falls back to the BOOT spec for an
    // unmapped combination so callers always have a defined screen/timeout.
    //
    // backend_offline distinguishes the two otherwise-identical Idle cases the
    // 11-state engine collapses: a healthy online session (-> ONLINE) versus a
    // post-drop reconnect loop (-> OFFLINE_RETRY, "Server unavailable.
    // Retrying..."). The Application sets it on OnNetworkError / clears it on
    // OnConnected so OFFLINE_RETRY is genuinely mapper-driven, not faked.
    static const TbotConnectStateSpec* Resolve(DeviceState device_state,
                                               TbotClaimSubstate claim_substate,
                                               TbotBleSubstate ble_substate,
                                               bool backend_offline = false);

    // Convenience: resolve and return only the connect state enum.
    static TbotConnectState ResolveState(DeviceState device_state,
                                         TbotClaimSubstate claim_substate,
                                         TbotBleSubstate ble_substate,
                                         bool backend_offline = false);

    // Look up a spec by its TbotConnectState (table lookup, never nullptr).
    static const TbotConnectStateSpec* SpecFor(TbotConnectState state);

    // Screen copy for a connect state, straight from the contract table.
    static const char* ScreenTextFor(TbotConnectState state);
};

#endif  // TBOT_CONNECT_MAPPER_H_
