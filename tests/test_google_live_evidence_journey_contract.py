"""Source contracts for the optional Google Live evidence journey handoff."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JOURNEY_KEY = "evidence_journey_id"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def test_ota_declares_transient_journey_without_persistent_settings_surface():
    header = read("main/ota.h")
    source = read("main/ota.cc")

    assert "GetTransientEvidenceJourneyId" in header
    assert "transient_evidence_journey_id_" in header
    assert f'SetString("{JOURNEY_KEY}"' not in source


def test_validator_is_exact_safe_ascii_length_contract_in_both_consumers():
    ota = read("main/ota.cc")
    websocket = read("main/protocols/websocket_protocol.cc")

    for source in (ota, websocket):
        validator = function_body(source, "bool IsValidEvidenceJourneyId")
        assert "value.empty()" in validator
        assert "value.size() > 64" in validator
        for allowed in ("'A'", "'Z'", "'a'", "'z'", "'0'", "'9'", "'.'", "'_'", "':'", "'-'"):
            assert allowed in validator
        for unsafe in ("'/'", "'\\\\'", "' '"):
            assert unsafe not in validator
        assert "return false;" in validator


def test_normal_ota_intercepts_journey_before_generic_string_persistence():
    source = read("main/ota.cc")
    check = function_body(source, "esp_err_t Ota::CheckVersion")
    assert check.index("transient_evidence_journey_id_.clear();") < check.index("#if")
    websocket_parse = check[check.index('cJSON *websocket = cJSON_GetObjectItem(root, "websocket")') :]
    loop = websocket_parse[
        websocket_parse.index("cJSON_ArrayForEach(item, websocket)") :
        websocket_parse.index("has_websocket_config_ = true;")
    ]

    intercept = loop.index(f'std::strcmp(item->string, "{JOURNEY_KEY}") == 0')
    persist = loop.index("settings.SetString(item->string, item->valuestring);")
    assert intercept < persist
    assert "cJSON_IsString(item)" in loop[intercept:persist]
    assert "IsValidEvidenceJourneyId" in loop[intercept:persist]
    assert "transient_evidence_journey_id_.clear();" in websocket_parse[
        : websocket_parse.index("cJSON_ArrayForEach(item, websocket)")
    ]
    assert "continue;" in loop[intercept:persist]


def test_course_mode_schema_allows_only_optional_valid_journey():
    source = read("main/ota.cc")
    parser = function_body(source, "bool Ota::ParseCourseModeResponse")

    assert "transient_evidence_journey_id_.clear();" in parser
    assert f'cJSON_GetObjectItem(websocket, "{JOURNEY_KEY}")' in parser
    assert "IsValidEvidenceJourneyId" in parser
    assert "websocket_field_count != 2 && websocket_field_count != 3" in parser
    assert "Settings" not in parser
    assert "SetString" not in parser


def test_application_hands_journey_to_websocket_in_production_and_local_branches():
    source = read("main/application.cc")
    initialize = function_body(source, "void Application::InitializeProtocol")

    assert initialize.count("SetTransientConfig(") == 2
    assert initialize.count("ota_->GetTransientEvidenceJourneyId()") == 2
    production, local = initialize.split("#else", 1)
    assert "SetTransientConfig(" in production
    assert "SetTransientConfig(" in local


def test_websocket_transient_config_keeps_url_token_branch_semantics_and_validates_journey():
    header = read("main/protocols/websocket_protocol.h")
    source = read("main/protocols/websocket_protocol.cc")
    setter = function_body(source, "void WebsocketProtocol::SetTransientConfig")

    assert "std::string evidence_journey_id" in header
    assert "transient_evidence_journey_id_" in header
    assert "IsValidEvidenceJourneyId(evidence_journey_id)" in setter
    assert "transient_evidence_journey_id_.clear();" in setter
    local, production = setter.split("#else", 1)
    assert "IsValidCourseModeWebsocketUrl(url)" in local
    assert "url_ = std::move(url);" in local
    assert "token_ = std::move(token);" in local
    invalid_url = local[local.index("if (!IsValidCourseModeWebsocketUrl(url)") : local.index("url_ = std::move(url);")]
    assert "transient_evidence_journey_id_.clear();" in invalid_url
    assert "(void)url;" in production
    assert "(void)token;" in production


def test_hello_adds_valid_journey_only_and_consumes_before_allocation_or_add_failure():
    source = read("main/protocols/websocket_protocol.cc")
    hello = function_body(source, "std::string WebsocketProtocol::GetHelloMessage")

    move = hello.index("std::move(transient_evidence_journey_id_)")
    clear = hello.index("transient_evidence_journey_id_.clear();", move)
    create = hello.index("cJSON_CreateObject()")
    add = hello.index(f'cJSON_AddStringToObject(root, "{JOURNEY_KEY}"')
    assert move < clear < create < add
    assert "IsValidEvidenceJourneyId(evidence_journey_id)" in hello
    assert "if (root == nullptr)" in hello
    assert "if (json_str == nullptr)" in hello


def test_journey_value_never_reaches_logging_or_print_sinks():
    sources = [read("main/ota.cc"), read("main/protocols/websocket_protocol.cc")]
    sink = re.compile(r"(?:ESP_LOG\w*|printf|fprintf|snprintf|puts|fputs)\s*\([^;]*evidence_journey", re.S)
    for source in sources:
        assert sink.search(source) is None
