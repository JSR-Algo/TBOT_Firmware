"""Contract tests for the BOOT long-press "re-pair" flow.

Holding BOOT must let a (possibly different) parent phone re-connect/re-claim the
robot instead of it staying "stuck" to the phone/account it was already paired
with. These source-scrape checks lock the design decided with the user:

  * Re-pair clears all local claimed/runtime identity after cloud release succeeds
    (including an already-released HTTP 401), before rebooting into provisioning.
  * A genuinely offline release keeps only the backend credentials needed for the
    deferred retry, while release_pending prevents them from starting claimed
    runtime/audio during the next provisioning boot.
  * Releasing backend ownership (so the `devices` row stops returning
    DEVICE_ALREADY_OWNED) is DEFERRED via backend.release_pending and fired off
    the priority-10 Application task once the robot is online again.

See also test_tbot_factory_reset_contract.py (the cloud release primitive this
flow reuses) and test_lcdwiki_es3c35p_board.py (the BOOT long-press wiring).
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def app_source() -> str:
    return read("main/application.cc")


def test_enter_repair_pairing_is_thread_safe_and_unclaims_locally():
    body = function_body(app_source(), "void Application::EnterRepairPairingMode")

    # Callable from the BOOT button task -> all claim-FSM/NVS mutation must be
    # marshalled onto the Application task (OQ1 single-threaded claim FSM).
    assert "Schedule([this]()" in body

    # Drop the local "claimed" signal + the live WS/claim tokens so the robot
    # stops acting owned and re-advertises for pairing.
    assert 'claim_state.SetInt("confirmed", 0);' in body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in body
    assert 'websocket_settings.SetString("url", "");' in body

    # Forget Wi-Fi + reboot so the next boot re-opens Wi-Fi provisioning and the parent
    # can pick a NEW network. (Re-advertising on the old, still-connected network makes
    # the app skip the Wi-Fi step, so a different network could never be set.)
    assert "SsidManager::GetInstance().ForceClearAndCancelTransaction()" in body
    assert "esp_restart();" in body


def test_enter_repair_pairing_releases_cloud_before_forgetting_wifi():
    body = function_body(app_source(), "void Application::EnterRepairPairingMode")

    # The cloud-ownership release must happen SYNCHRONOUSLY at press time, BEFORE we
    # forget Wi-Fi + reboot: the release POST needs the (old) network to reach the
    # backend, and a deferred/async release races the parent re-pairing on the new phone
    # and loses (the phone's claim hits the backend first -> DEVICE_ALREADY_OWNED).
    assert "SystemReset::ReleaseCloudOwnership()" in body
    assert body.index("SystemReset::ReleaseCloudOwnership()") < body.index("SsidManager::GetInstance().ForceClearAndCancelTransaction()")

    # On success/already-released: drop the complete backend identity + stop
    # deferring. Leaving device_id behind makes the next boot enter stale-claim
    # recovery only after large audio workers have fragmented internal heap.
    assert 'backend_settings.SetString("device_id", "");' in body
    assert 'backend_settings.SetString("device_secret", "");' in body
    assert 'backend_settings.SetInt("release_pending", 0);' in body
    # On failure (offline): keep credentials + defer a retry for when we're online.
    assert 'backend_settings.SetInt("release_pending", 1);' in body

    # Keep the API URL; the offline branch keeps id + secret for deferred release.
    assert 'SetString("api_url", "");' not in body


def test_enter_repair_pairing_clears_every_claim_runtime_marker_on_completed_release():
    body = function_body(app_source(), "void Application::EnterRepairPairingMode")

    assert 'claim_state.SetInt("confirmed", 0);' in body
    assert 'claim_state.SetInt("factory_test", 0);' in body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in body
    assert 'websocket_settings.SetString("token", "");' in body
    assert 'websocket_settings.SetString("url", "");' in body
    assert 'websocket_settings.SetInt("claim_ambiguous", 0);' in body
    assert 'websocket_settings.EraseKey("claim_device_id");' in body


def test_refresh_pending_claim_fires_deferred_release():
    body = function_body(app_source(), "void Application::RefreshPendingTbotClaim")
    assert "MaybeDispatchDeferredCloudRelease();" in body


def test_deferred_release_is_single_flight_and_off_the_app_task():
    body = function_body(app_source(), "void Application::MaybeDispatchDeferredCloudRelease")

    # Only act when a re-pair actually deferred a release.
    assert 'GetInt("release_pending", 0)' in body
    # Single-flight so a stuck network can't stack workers across refreshes.
    assert "cloud_release_inflight_.compare_exchange_strong(expected, true)" in body
    # Run the blocking POST OFF the priority-10 task, low priority + not core-0
    # pinned (same starvation-avoidance pattern as the claim fetch).
    assert 'xTaskCreateWithCaps(&Application::CloudReleaseTask, "cloud_release"' in body
    # Internal DRAM only — SPIRAM stacks panic when the worker touches NVS/flash.
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in body
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" not in body
    assert "tskIDLE_PRIORITY + 1" in body


def test_deferred_release_allocation_failures_are_retry_safe_and_zeroize_secrets():
    body = function_body(app_source(), "void Application::MaybeDispatchDeferredCloudRelease")

    assert "new (std::nothrow) CloudReleaseContext" in body
    allocation_failure = body[body.index("new (std::nothrow) CloudReleaseContext") :]
    assert "if (ctx == nullptr)" in allocation_failure
    null_branch = allocation_failure[
        allocation_failure.index("if (ctx == nullptr)") :
        allocation_failure.index("xTaskCreateWithCaps")
    ]
    assert "cloud_release_inflight_.store(false);" in null_branch
    assert 'SetInt("release_pending", 0)' not in null_branch
    assert "return;" in null_branch

    task_failure = allocation_failure[allocation_failure.index("xTaskCreateWithCaps") :]
    assert "SecureClearString(ctx->device_secret);" in task_failure
    assert task_failure.index("SecureClearString(ctx->device_secret);") < task_failure.index(
        "delete ctx;"
    )
    assert 'SetInt("release_pending", 0)' not in task_failure

    assert "SecureStringScope device_secret_scope(device_secret);" in body


def test_cloud_release_worker_clears_state_only_on_success():
    body = function_body(app_source(), "void Application::CloudReleaseTask")

    assert "SystemReset::ReleaseCloudOwnership(ctx->api_url, ctx->device_id, ctx->device_secret)" in body
    # Result handling marshalled back onto the Application task (OQ1).
    assert "self->Schedule(" in body
    # On success: stop deferring + drop the now-released secret so the next claim
    # mints a fresh one. A failed release leaves release_pending set to retry.
    assert 'backend_settings.SetInt("release_pending", 0);' in body
    assert 'backend_settings.SetString("device_id", "");' in body
    assert 'backend_settings.SetString("device_secret", "");' in body
    assert "self->RefreshPendingTbotClaim();" in body
    assert "vTaskDelete(nullptr);" in body


def test_deferred_release_uses_captured_credentials_and_cannot_clear_a_later_claim():
    source = app_source()
    dispatch = function_body(source, "void Application::MaybeDispatchDeferredCloudRelease")
    worker = function_body(source, "void Application::CloudReleaseTask")
    reset_header = read("main/boards/common/system_reset.h")

    assert "struct CloudReleaseContext" in source
    assert "api_url, device_id, device_secret" in dispatch
    assert "new (std::nothrow) CloudReleaseContext" in dispatch
    assert "ReleaseCloudOwnership(ctx->api_url, ctx->device_id, ctx->device_secret)" in worker
    assert "credentials_unchanged" in worker
    changed_branch = worker[worker.index("if (!credentials_unchanged)") :]
    assert 'backend_settings.SetInt("release_pending", 0);' in changed_branch
    assert 'backend_settings.SetString("device_secret", "");' not in changed_branch[
        : changed_branch.index("return;")
    ]
    assert "ReleaseCloudOwnership(const std::string& api_url" in reset_header
