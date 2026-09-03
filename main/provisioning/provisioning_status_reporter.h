#pragma once

#include <string>

// Returns a bounded, log-safe top-level backend error code or "unknown".
std::string ExtractProvisioningErrorCode(const std::string& response_body);

/**
 * Reports provisioning status to the backend after Wi-Fi join.
 *
 * Endpoint: POST CONFIG_PROVISIONING_STATUS_URL
 * Auth:     Authorization: Bearer <bootstrap_token>
 * Body:     {"device_id":"<uuid>","status":"device_authenticated","code":"123456","credential_response":true}
 *       or  {"device_id":"<uuid>","status":"failed","reason":"wifi_connect_failed"}
 *
 * HTTP client id 2 (OTA=0, WebSocket=1).
 * TLS: esp_crt_bundle_attach (default, via EspNetwork).
 * Retry: 3 attempts with exponential back-off 2 s / 4 s / 8 s.
 */
class ProvisioningStatusReporter {
public:
    enum class Status {
        DeviceAuthenticated,
        Failed,
    };

    /**
     * @brief POST provisioning status to backend.
     *
     * @param status   DeviceAuthenticated or Failed
     * @param token    Bootstrap token (Bearer credential). Must not be empty.
     * @param code     6-digit BLE pairing code (required for DeviceAuthenticated).
     * @param reason   Failure reason string (used only when status == Failed).
     * @return true    Backend returned 2xx and authenticated credentials were persisted.
     * @return false   All retries exhausted or token empty.
     */
    static bool Report(Status status,
                       const std::string& token,
                       const std::string& code,
                       const std::string& reason = "");
};
