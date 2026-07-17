#pragma once

#include <time.h>

#include <string>

struct PendingTbotClaim {
    bool active = false;
    std::string claim_id;
    std::string status;
    std::string message;
    std::string expires_at;
};

bool ParsePendingTbotClaimFromDeviceConfigJson(const std::string& json,
                                               PendingTbotClaim& pending_claim);

// Parse an ISO-8601 / RFC-3339 UTC timestamp ("2026-06-04T12:34:56Z", optional
// fractional seconds) to epoch seconds. Returns false if the string is empty or
// not parseable. UTC only — the backend always emits a trailing 'Z'.
bool ParseIso8601UtcToEpoch(const std::string& iso_utc, time_t& out_epoch_seconds);

// True when the claim has a parseable expires_at that is at or before now_epoch.
// A claim with no/invalid expires_at is treated as NOT expired (false) so a
// missing field never spuriously tears down a live claim window — the bounded
// poll's 5-minute cap is the backstop in that case.
bool IsPendingTbotClaimExpired(const PendingTbotClaim& claim, time_t now_epoch_seconds);

std::string BuildTbotClaimConfirmUrl(const std::string& api_base_url);

std::string BuildTbotClaimConfirmBody(const std::string& claim_id,
                                      const std::string& device_id,
                                      const std::string& confirm_method = "button_press");

std::string BuildTbotDeviceConfigUrl(const std::string& api_base_url,
                                     const std::string& device_id);

std::string BuildTbotConfigFetchUrl(const std::string& api_base_url);

// Post-claim boot refresh for the active runtime config. Uses backend.{api_url,
// device_id,device_secret} from NVS and persists configBlob.websocket.url into
// Settings("websocket"). Returns false on any missing credential, transport,
// auth, or parse failure so callers keep the existing OTA/NVS fallback.
bool RefreshWebsocketUrlFromConfigFetch();

bool FetchPendingTbotClaimFromDeviceConfig(const std::string& api_base_url,
                                           const std::string& bootstrap_token,
                                           PendingTbotClaim& pending_claim);

bool FetchPendingTbotClaimFromDeviceConfig(const std::string& api_base_url,
                                           const std::string& bootstrap_token,
                                           PendingTbotClaim& pending_claim,
                                           int* http_status_code);

// Persist a claim-confirm response already validated by the network worker.
// Kept separate so generation-gated Application code performs NVS writes only
// after confirming the setup session is still current.
bool PersistTbotClaimConfirmationResponse(const std::string& json);

// Derive the device-bootstrap URL ({origin}/v1/device/bootstrap) from the
// compiled provisioning-status URL ({origin}/v1/device/provisioning/status).
// Returns "" if the provisioning URL does not carry the expected suffix.
std::string BuildTbotDeviceBootstrapUrl();

// H4: fallback path for backend.api_url. The OTA CheckVersion response is the
// primary source of api_url; if the OTA host omits it the whole claim/heartbeat
// feature goes silently inert. GET /v1/device/bootstrap (which always emits
// api_url) and, on success, persist it into Settings("backend"). Returns the
// fetched api_url on success, "" on failure. main-task-only HTTP (blocking).
std::string FetchBackendApiUrlFromBootstrap(const std::string& bootstrap_token,
                                            bool persist_result = true);

enum class ClaimConfirmationResult {
    Confirmed,
    RetryableFailure,
    AmbiguousSuccess,
    TerminalFailure,
};

class ClaimConfirmationReporter {
public:
    static ClaimConfirmationResult Confirm(const PendingTbotClaim& claim,
                                           const std::string& api_base_url,
                                           const std::string& bootstrap_token,
                                           std::string* deferred_success_response = nullptr);
};
