from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_combined_cmake_registers_both_identity_candidates_in_correct_profiles():
    root_cmake = read("CMakeLists.txt")
    main_cmake = read("main/CMakeLists.txt")

    assert "set(TBOT_HIL_PROFILE" not in root_cmake
    assert '"esp_build_identity.cc"' in main_cmake
    assert '"sd_fat_session_guard.cc"' in main_cmake
    hil_sources = main_cmake.split("if(CONFIG_TBOT_HIL_STORAGE_FAULTS)", 1)[1].split(
        "endif()", 1
    )[0]
    assert '"physical_sd_identity.cc"' in hil_sources
    assert '"esp_build_identity.cc"' not in hil_sources
    assert '"sd_fat_session_guard.cc"' not in hil_sources
    identity = read("main/esp_build_identity.cc")
    assert "#ifdef TBOT_HIL_PROFILE" in identity
    assert "TBOT_EMBEDDED_PROFILE" in identity


def test_exact_rebased_source_and_test_destinations_exist():
    destinations = (
        "main/physical_sd_identity.cc",
        "main/physical_sd_identity.h",
        "main/sd_fat_session_guard.cc",
        "main/sd_fat_session_guard.h",
        "main/esp_build_identity.cc",
        "main/esp_build_identity.h",
        "scripts/hil_storage_identity_contract.py",
        "tests/test_hil_storage_identity_contract.py",
        "tests/native/esp_build_identity_host_test.cc",
        "tests/native/physical_sd_identity_host_test.cc",
        "tests/native/sd_fat_session_guard_host_test.cc",
        "tests/native/sd_guard_registry_lifecycle_host_test.cc",
        "scripts/run_host_native_esp_build_identity_test.sh",
        "scripts/run_host_native_physical_sd_identity_test.sh",
        "scripts/run_host_native_sd_fat_session_guard_test.sh",
        "scripts/run_host_native_sd_guard_registry_lifecycle_test.sh",
        "scripts/run_goal2_firmware_rebind_full_python.sh",
        "tests/native/lesson_storage_hil_status_contention_host_test.cc",
        "scripts/run_host_native_lesson_storage_hil_status_contention_test.sh",
    )
    assert all((ROOT / destination).is_file() for destination in destinations)
    runner = read("scripts/run_goal2_firmware_rebind_full_python.sh")
    assert "CONFIG_JD_USE_ROM=y" not in runner
    assert 'SDKCONFIG="${ROOT}/sdkconfig"' not in runner
    assert "trap cleanup EXIT INT TERM" in runner
    assert "python3 -m pytest -q" in runner
    assert "worktree changed during full Python gate" in runner
    assert "stat -f" not in runner
    assert "stat.S_IMODE" in runner
    assert "git ls-files -z --cached --others --exclude-standard" in runner
    assert "os.lstat" in runner
    assert "hashlib.sha256" in runner
    assert "WORKTREE_SNAPSHOT_SHA256" in runner
    source = read("main/lesson_storage_hil_mcp_tools.cc")
    body = source.split("std::string CallStatusForSchema", 1)[1].split(
        "std::string CallStatus(", 1
    )[0]
    assert body.index("LessonStorageHilController::GetInstance().Status()") < body.index(
        "TryAcquire()"
    )
    assert "if (schema_version == 2)" in body
    assert "AddUnavailablePhysicalSdIdentity" in body


def test_websocket_emits_build_identity_on_canonical_transport_before_connect():
    source = read("main/protocols/websocket_protocol.cc")
    assert '#include "esp_build_identity.h"' in source
    identity = source.index("ReadRunningEspBuildIdentity")
    set_header = source.index("replacement_websocket->SetHeader", identity)
    connect = source.index("replacement_websocket->Connect", set_header)
    assert identity < set_header < connect


def test_sd_identity_is_bound_to_board_hil_tools_and_storage_coordinator():
    board = read("main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc")
    tools = read("main/lesson_storage_hil_mcp_tools.cc")
    coordinator = read("main/lesson_asset_storage_coordinator.cc")

    assert '#include "physical_sd_identity.h"' in board
    assert "identity_registry.ObserveMountedCard" in board
    assert "schemaVersion" in tools and "storageIdentity" in tools
    assert "SdFatSessionGuard" in coordinator
