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


def test_eviction_hil_hook_surface_is_noexcept_and_fixed_outcome():
    header = read("main/lesson_storage_hil_hooks.h")

    assert "enum class LessonStorageHilHookOutcome" in header
    assert "kContinue" in header
    assert "kFail" in header
    assert "kNoSpace" in header
    assert "LessonStorageHilHookOutcome RunLessonStorageHilCheckpoint(" in header
    assert ") noexcept;" in header
    assert "SetLessonStorageHilPauseCallbackForTest" in header


def test_eviction_checkpoints_are_guarded_and_ordered_in_destructive_phase():
    source = read("main/lesson_asset_cache_evict.cc")
    body = source[source.index("int deleted_count = 0;") :]
    first_unlink_checkpoint = body.index("kBeforeFirstUnlink")
    first_unlink = body.index("unlink(child_path.c_str())")
    increment = body.index("++deleted_count")
    after_unlinks = body.index("kAfterUnlinks")
    before_rmdir = body.index("kBeforeRmdir")
    rmdir = body.index("rmdir(leaf_path.c_str())")

    assert first_unlink_checkpoint < first_unlink < increment < after_unlinks
    assert after_unlinks < before_rmdir < rmdir
    assert "TBOT_LESSON_STORAGE_HIL_HOOKS_TESTING" in body
    assert "CONFIG_TBOT_HIL_STORAGE_FAULTS" in body

    destructive = body[first_unlink_checkpoint:rmdir].replace(
        "throw std::bad_alloc();", ""
    )
    for banned in (
        "std::string(",
        "std::vector",
        "push_back(",
        "reserve(",
        "MakeResult(",
        "cJSON",
        "throw ",
    ):
        assert banned not in destructive


def test_hil_pause_uses_yielding_delay_and_stable_markers():
    source = read("main/lesson_storage_hil_hooks.cc")

    assert "HIL_STORAGE_CHECKPOINT_REACHED" in source
    assert "HIL_STORAGE_CHECKPOINT_CONTINUED" in source
    assert "vTaskDelay(pdMS_TO_TICKS(seconds * 1000U))" in source
    assert "while (" not in source
    assert "sleep(" not in source
    corrupt_start = source.index("case LessonStorageHilAction::kCorruptStaging:")
    corrupt = source[corrupt_start : source.index("\n    }", corrupt_start)]
    assert "return LessonStorageHilHookOutcome::kFail;" in corrupt
    for forbidden_field in ("sequence=", "action=", "pause_seconds="):
        assert forbidden_field not in source
