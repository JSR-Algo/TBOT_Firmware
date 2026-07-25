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


def test_lvgl_apply_fails_when_local_srmodels_do_not_load():
    source = read("main/assets.cc")
    apply_body = function_body(source, "bool Assets::LvglStrategy::Apply")

    load_idx = apply_body.index("if (!Assets::LoadSrmodelsFromIndex(assets, root))")
    theme_idx = apply_body.index("auto& theme_manager", load_idx)
    failure_branch = apply_body[load_idx:theme_idx]

    assert "cJSON_Delete(root);" in failure_branch
    assert "return false;" in failure_branch


def test_emote_apply_fails_when_local_srmodels_do_not_load():
    source = read("main/assets.cc")
    apply_body = function_body(source, "bool Assets::EmoteStrategy::Apply")

    load_idx = apply_body.index("if (!Assets::LoadSrmodelsFromIndex(assets))")
    display_idx = apply_body.index("auto display = Board::GetInstance().GetDisplay();", load_idx)
    failure_branch = apply_body[load_idx:display_idx]

    assert "return false;" in failure_branch
    assert "emote_load_assets" not in failure_branch


def test_claim_audio_gate_depends_on_truthful_local_srmodels_apply_result():
    assets = read("main/assets.cc")
    application = read("main/application.cc")
    helper = function_body(application, "bool Application::EnsureLocalAssetsAppliedForClaim")
    claim = function_body(
        application, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    success = claim[claim.index("CancelClaimExpiryTimer();") :]

    assert "return assets.Apply(false);" in helper
    assert "if (!Assets::LoadSrmodelsFromIndex(assets, root))" in function_body(
        assets, "bool Assets::LvglStrategy::Apply"
    )
    assert "if (!Assets::LoadSrmodelsFromIndex(assets))" in function_body(
        assets, "bool Assets::EmoteStrategy::Apply"
    )

    ready_idx = success.index("const bool local_assets_ready = EnsureLocalAssetsAppliedForClaim();")
    gate_idx = success.index("if (local_assets_ready)", ready_idx)
    heartbeat_idx = success.index("StartHeartbeat();", gate_idx)
    gated_branch = success[gate_idx:heartbeat_idx]

    assert "audio_service_.Start();" in gated_branch
    assert "audio_service_.EnableWakeWordDetection(true);" in gated_branch
    assert "audio_service_.Start();" not in success[ready_idx:gate_idx]
    assert "audio_service_.EnableWakeWordDetection(true);" not in success[ready_idx:gate_idx]
