from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_hil_storage_config_is_default_off_and_board_restricted():
    kconfig = read("main/Kconfig.projbuild")
    start = kconfig.index("config TBOT_HIL_STORAGE_FAULTS")
    block = kconfig[start:]
    next_config = block.find("\nconfig ", 1)
    if next_config != -1:
        block = block[:next_config]

    assert "default n" in block
    assert "depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LCDWIKI_ES3C35P" in block


def test_hil_profile_is_separate_from_production_defaults():
    assert read("sdkconfig.defaults.hil-storage") == "CONFIG_TBOT_HIL_STORAGE_FAULTS=y\n"
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS=y" not in read("sdkconfig.defaults")
    assert "sdkconfig.defaults.hil-storage" not in read("CMakeLists.txt")


def test_hil_sources_are_conditionally_compiled():
    cmake = read("main/CMakeLists.txt")
    conditional = cmake[cmake.index("if(CONFIG_TBOT_HIL_STORAGE_FAULTS)") :]
    conditional = conditional[: conditional.index("endif()")]

    assert '"lesson_storage_hil_controller.cc"' in conditional
    assert '"lesson_storage_hil_hooks.cc"' in conditional
    assert '"lesson_storage_hil_fixture.cc"' in conditional
    assert '"lesson_storage_hil_mcp_tools.cc"' in conditional
