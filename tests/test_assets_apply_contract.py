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


def test_load_srmodels_treats_absent_key_as_optional_but_declared_bad_models_as_fatal():
    source = read("main/assets.cc")
    loader = function_body(source, "bool Assets::LoadSrmodelsFromIndex")

    absent_idx = loader.index("if (srmodels == nullptr)")
    type_idx = loader.index("if (!cJSON_IsString(srmodels))", absent_idx)
    load_idx = loader.index("std::string srmodels_file = srmodels->valuestring", type_idx)

    assert "return true;" in loader[absent_idx:type_idx]
    assert "return false;" in loader[type_idx:load_idx]
    assert "return false;" in loader[load_idx:]

def test_lvgl_apply_fails_when_declared_local_srmodels_do_not_load():
    source = read("main/assets.cc")
    apply_body = function_body(source, "bool Assets::LvglStrategy::Apply")

    load_idx = apply_body.index("if (!Assets::LoadSrmodelsFromIndex(assets, root.get()))")
    theme_idx = apply_body.index("auto& theme_manager", load_idx)
    failure_branch = apply_body[load_idx:theme_idx]

    assert "std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root" in apply_body
    assert "Assets::LoadSrmodelsFromIndex(assets, root.get())" in failure_branch
    assert "return false;" in failure_branch

def test_lvgl_apply_uses_raii_for_all_post_parse_early_returns():
    source = read("main/assets.cc")
    apply_body = function_body(source, "bool Assets::LvglStrategy::Apply")
    parse_idx = apply_body.index("cJSON_ParseWithLength(static_cast<char*>(ptr), size)")
    first_post_parse_return = apply_body.index("return", parse_idx)

    assert "std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root" in apply_body
    assert "root.get()" in apply_body
    assert "cJSON_Delete(root);" not in apply_body[parse_idx:]
    assert apply_body.index("std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root") < first_post_parse_return

    for expected_failure in (
        "version->valuedouble > 1",
        "Assets::LoadSrmodelsFromIndex(assets, root.get())",
        "text_font->font() == nullptr",
        "The background image file %s is not found",
    ):
        assert expected_failure in apply_body


def test_emote_apply_fails_when_declared_local_srmodels_do_not_load():
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
    finish = function_body(application, "bool Application::FinishClaimActivationAfterLocalAssetsReady")

    assert "return assets.Apply(false);" in helper
    assert "if (!Assets::LoadSrmodelsFromIndex(assets, root.get()))" in function_body(
        assets, "bool Assets::LvglStrategy::Apply"
    )
    assert "if (!Assets::LoadSrmodelsFromIndex(assets))" in function_body(
        assets, "bool Assets::EmoteStrategy::Apply"
    )

    ready_idx = finish.index("if (!EnsureLocalAssetsAppliedForClaim())")
    gate_idx = finish.index("if (!audio_service_.Start())", ready_idx)
    idle_idx = finish.index("SetDeviceState(kDeviceStateIdle);", gate_idx)
    heartbeat_idx = finish.index("StartHeartbeat();", gate_idx)
    gated_branch = finish[gate_idx:heartbeat_idx]

    assert gate_idx < idle_idx < heartbeat_idx
    assert "if (!audio_service_.Start())" in gated_branch
    assert "return false;" in function_body(finish, "if (!audio_service_.Start())")
    assert "audio_service_.EnableWakeWordDetection(true);" in gated_branch
    assert "audio_service_.Start()" not in finish[ready_idx:gate_idx]
    assert "audio_service_.EnableWakeWordDetection(true);" not in finish[ready_idx:gate_idx]
