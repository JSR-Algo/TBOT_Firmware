"""Native firmware coverage harness contract.

The Python contract suite can reach 100% coverage without hardware. Native
C/C++ source coverage needs an ESP-IDF gcov build plus a runtime dump from the
target. This file locks the build hooks that make that native path measurable.
"""

from pathlib import Path
import stat

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_native_coverage_defaults_enable_esp_idf_gcov_transport():
    defaults = read("sdkconfig.defaults.coverage")

    required = {
        "CONFIG_APPTRACE_DEST_JTAG=y",
        "# CONFIG_APPTRACE_DEST_NONE is not set",
        "CONFIG_APPTRACE_ENABLE=y",
        "CONFIG_APPTRACE_GCOV_ENABLE=y",
        "CONFIG_ESP_DEBUG_STUBS_ENABLE=y",
    }

    for line in required:
        assert line in defaults


def test_top_level_cmake_exposes_native_gcov_report_target():
    cmake = read("CMakeLists.txt")

    assert "option(TBOT_NATIVE_COVERAGE" in cmake
    assert "idf_build_set_property(MINIMAL_BUILD OFF)" in cmake
    assert "components/app_trace/project_include.cmake" in cmake
    assert "idf_create_coverage_report" in cmake
    assert "idf_clean_coverage_report" in cmake


def test_main_component_is_the_only_project_component_instrumented():
    cmake = read("main/CMakeLists.txt")

    assert "if(TBOT_NATIVE_COVERAGE)" in cmake
    assert "tests/native/afsk_demod_host_test.cc" in cmake
    assert "tests/native_stubs" in cmake
    assert "TBOT_NATIVE_COVERAGE_SOURCES" in cmake
    assert "set_source_files_properties" in cmake
    assert "list(APPEND MAIN_PRIV_REQUIRES_EXTRA app_trace)" in cmake
    assert "target_compile_options(${COMPONENT_LIB} PRIVATE --coverage)" in cmake
    assert "target_link_libraries(${COMPONENT_LIB} PRIVATE --coverage)" in cmake

def test_target_coverage_main_runs_afsk_harness_and_hardcoded_gcov_dump():
    main = read("main/main.cc")

    assert "#if TBOT_NATIVE_COVERAGE" in main
    assert "TbotAfskDemodHostCoverageMain" in main
    assert "esp_gcov_dump()" in main
    assert "TBOT native coverage: ready to dump GCOV data" in main

def test_afsk_native_harness_exposes_esp_target_entrypoint_without_breaking_host_main():
    harness = read("tests/native/afsk_demod_host_test.cc")

    assert "#ifdef TBOT_ESP_NATIVE_COVERAGE" in harness
    assert "int TbotAfskDemodHostCoverageMain()" in harness
    assert "int main()" in harness

def test_native_coverage_runner_uses_openocd_dump_and_fails_without_gcda():
    script_path = ROOT / "scripts" / "run_native_coverage.sh"
    script = script_path.read_text(encoding="utf-8")

    assert script_path.stat().st_mode & stat.S_IXUSR
    assert "set -euo pipefail" in script
    assert "sdkconfig.defaults.coverage" in script
    assert "TBOT_NATIVE_COVERAGE=ON" in script
    assert "TBOT_NATIVE_COVERAGE_SOURCES" in script
    assert "openocd" in script
    assert "board/esp32s3-builtin.cfg" in script
    assert "esp gcov dump" in script
    assert "*.gcda" in script
    assert "gcovr-report" in script
    assert "--filter main/boards/common/afsk_demod.cc" in script
    assert "--fail-under-line" in script
    assert "--fail-under-branch" not in script
    assert "--fail-under-function" in script

def test_host_native_afsk_runner_compiles_real_source_with_stubs_and_coverage_gate():
    script_path = ROOT / "scripts" / "run_host_native_afsk_coverage.sh"
    script = script_path.read_text(encoding="utf-8")

    assert script_path.stat().st_mode & stat.S_IXUSR
    assert "set -euo pipefail" in script
    assert "main/boards/common/afsk_demod.cc" in script
    assert "tests/native_stubs" in script
    assert "tests/native/afsk_demod_host_test.cc" in script
    assert "--coverage" in script
    assert "LLVM_COV=\"${LLVM_COV:-${LLVM_COV_BIN} gcov}\"" in script
    assert "--filter main/boards/common/afsk_demod.cc" in script
    assert "--fail-under-line 100" in script
    assert "--fail-under-function 100" in script

def test_host_native_afsk_runner_keeps_llvm_coverage_artifacts_outside_repo():
    script = read("scripts/run_host_native_afsk_coverage.sh")

    assert "mktemp -d \"${TMPDIR:-/tmp}/tbot-host-native-afsk-coverage.XXXXXX\"" in script
    assert "trap 'rm -rf \"${BUILD_DIR}\"' EXIT" in script
    assert "build-host-native-afsk-coverage" not in script

def test_host_native_lesson_runner_compiles_real_renderer_and_enforces_line_coverage():
    script_path = ROOT / "scripts" / "run_host_native_lesson_coverage.sh"
    script = script_path.read_text(encoding="utf-8")

    assert "set -euo pipefail" in script
    assert "main/lesson_handler.cc" in script
    assert "tests/native_stubs_lesson" in script
    assert "tests/native/lesson_handler_host_test.cc" in script
    assert "-Dfread=HostLessonFread" in script
    assert "--coverage" in script
    assert "--filter '.*lesson_handler\\.cc'" in script
    assert "--fail-under-line 100" in script
