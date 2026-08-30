import re
import subprocess
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "dependencies.lock"
MAIN_MANIFEST = ROOT / "main/idf_component.yml"
MAIN_CMAKE = ROOT / "main/CMakeLists.txt"
LOCAL_COMPONENTS = {
    "esp-wifi-connect": ROOT / "components/esp-wifi-connect/CMakeLists.txt",
    "esp-ml307": ROOT / "components/esp-ml307/CMakeLists.txt",
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EXACT_VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[~+-][0-9A-Za-z.-]+)?$")


def load_lock():
    assert LOCK.is_file(), "dependencies.lock must be committed at the project root"
    return yaml.safe_load(LOCK.read_text(encoding="utf-8"))


def test_dependency_lock_is_tracked_and_not_ignored():
    tracked = subprocess.run(
        ["git", "ls-files", "--error-unmatch", "dependencies.lock"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert tracked.returncode == 0, "dependencies.lock must be tracked by git"

    ignored = subprocess.run(
        ["git", "check-ignore", "-q", "dependencies.lock"],
        cwd=ROOT,
    )
    assert ignored.returncode == 1, "dependencies.lock must not be ignored"


def test_dependency_lock_is_portable_and_fully_pinned():
    lock_text = LOCK.read_text(encoding="utf-8")
    lock = load_lock()

    assert lock["version"] == "2.0.0"
    assert lock["target"] == "esp32s3"
    assert lock["dependencies"]["idf"]["version"] == "5.5.4"
    assert "type: local" not in lock_text
    assert "override_path" not in lock_text
    assert not re.search(r"(?:^|\s)path:\s*/", lock_text, re.MULTILINE)
    assert "/Users/" not in lock_text
    assert "/home/" not in lock_text
    assert ".worktrees/" not in lock_text

    service_dependencies = {
        name: component
        for name, component in lock["dependencies"].items()
        if component["source"]["type"] == "service"
    }
    assert service_dependencies
    for name, component in service_dependencies.items():
        assert SHA256_RE.fullmatch(component.get("component_hash", "")), name
        version = str(component.get("version", ""))
        assert EXACT_VERSION_RE.fullmatch(version), name


def test_repository_components_replace_machine_specific_overrides():
    manifest = yaml.safe_load(MAIN_MANIFEST.read_text(encoding="utf-8"))
    cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    for component_name, component_cmake in LOCAL_COMPONENTS.items():
        assert component_cmake.is_file(), component_name
        assert not (component_cmake.parent / "idf_component.yml").exists()
        assert re.search(rf"^\s+{re.escape(component_name)}\s*$", cmake, re.MULTILINE)

    for dependency_name in ("78/esp-wifi-connect", "78/esp-ml307"):
        assert dependency_name not in manifest["dependencies"]


def test_classic_local_component_dependencies_are_migrated_without_weakening():
    dependencies = yaml.safe_load(MAIN_MANIFEST.read_text(encoding="utf-8"))["dependencies"]

    assert dependencies["idf"] == {"version": ">=5.5.2"}
    assert dependencies["78/uart-uhci"] == {
        "version": "~0.2.1",
        "rules": [{"if": "target not in [esp32]"}],
    }
    assert dependencies["espressif/cjson"] == {
        "version": "^1.7.19",
        "matches": [{"if": "idf_version >=6.0"}],
    }

    ml307_cmake = LOCAL_COMPONENTS["esp-ml307"].read_text(encoding="utf-8")
    assert 'set(ML307_REQUIRES' in ml307_cmake
    assert re.search(
        r'if\(NOT IDF_TARGET STREQUAL "esp32"\).*?'
        r'list\(APPEND ML307_REQUIRES\s+"uart-uhci"\)',
        ml307_cmake,
        re.DOTALL,
    )
    assert "${ML307_REQUIRES}" in ml307_cmake

    wifi_cmake = LOCAL_COMPONENTS["esp-wifi-connect"].read_text(encoding="utf-8")
    idf6_branch = wifi_cmake.split("else()", 1)[0]
    assert re.search(r'REQUIRES(?:\s+"[^"]+")*\s+"cjson"', idf6_branch)
