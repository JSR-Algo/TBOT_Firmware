"""Regression tests for persisted Wi-Fi credential loading.

The robot can end up with `wifi/ssid` and `wifi/password` present but empty in
NVS after a re-pair/reset path. Empty SSIDs must not count as configured Wi-Fi,
otherwise WifiBoard::TryWifiConnect() starts station mode forever and never
opens BluFi provisioning.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    candidate = ROOT / path
    if not candidate.exists() and path.startswith("managed_components/78__esp-wifi-connect/"):
        candidate = ROOT / path.replace(
            "managed_components/78__esp-wifi-connect",
            "components/esp-wifi-connect",
            1,
        )
    return candidate.read_text(encoding="utf-8")


def _function_body(src: str, signature: str, next_signature: str) -> str:
    start = src.index(signature)
    end = src.index(next_signature, start + len(signature))
    return src[start:end]


def test_ssid_manager_ignores_empty_persisted_ssids():
    src = read("managed_components/78__esp-wifi-connect/ssid_manager.cc")
    body = _function_body(
        src,
        "void SsidManager::LoadFromNvs()",
        "void SsidManager::SaveToNvs()",
    )

    empty_guard = body.index("ssid[0] == '\\0'")
    push_idx = body.index("ssid_list_.push_back")

    assert empty_guard < push_idx, (
        "LoadFromNvs() must skip empty persisted SSIDs before adding them to "
        "ssid_list_, so blank NVS credentials enter provisioning instead of "
        "station retry mode"
    )
    assert "continue;" in body[empty_guard:push_idx]


def test_ssid_manager_rejects_empty_ssids_before_persisting():
    src = read("managed_components/78__esp-wifi-connect/ssid_manager.cc")
    body = _function_body(
        src,
        "void SsidManager::AddSsid(const std::string& ssid, const std::string& password)",
        "void SsidManager::RemoveSsid(int index)",
    )

    empty_guard = body.index("ssid.empty()")
    save_idx = body.index("SaveToNvs();")

    assert empty_guard < save_idx
    assert "return;" in body[empty_guard:save_idx]
