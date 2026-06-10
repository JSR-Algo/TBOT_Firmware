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

bool FetchPendingTbotClaimFromDeviceConfig(const std::string& api_base_url,
                                           const std::string& bootstrap_token,
                                           PendingTbotClaim& pending_claim);

// Derive the device-bootstrap URL ({origin}/v1/device/bootstrap) from the
// compiled provisioning-status URL ({origin}/v1/device/provisioning/status).
// Returns "" if the provisioning URL does not carry the expected suffix.
std::string BuildTbotDeviceBootstrapUrl();

// H4: fallback path for backend.api_url. The OTA CheckVersion response is the
// primary source of api_url; if the OTA host omits it the whole claim/heartbeat
// feature goes silently inert. GET /v1/device/bootstrap (which always emits
// api_url) and, on success, persist it into Settings("backend"). Returns the
// fetched api_url on success, "" on failure. main-task-only HTTP (blocking).
std::string FetchBackendApiUrlFromBootstrap(const std::string& bootstrap_token);

class ClaimConfirmationReporter {
public:
    static bool Confirm(const PendingTbotClaim& claim,
                        const std::string& api_base_url,
                        const std::string& bootstrap_token);
};
