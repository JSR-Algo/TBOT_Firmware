"""Exercise the production ROM log call without unsupported integer varargs."""
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def compile_and_run(tmp_path, source):
    unit = tmp_path / "format.cc"
    unit.write_text(source)
    binary = tmp_path / "format"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(unit), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_hex_format_boundaries(tmp_path):
    compile_and_run(tmp_path, r'''
#include "lesson_storage_hil_u64_format.h"
#include <cassert>
#include <string>
int main() {
    assert(std::string(FormatLessonStorageHilUint64Hex(0).c_str()) == "0");
    assert(std::string(FormatLessonStorageHilUint64Hex(UINT64_MAX).c_str()) == "ffffffffffffffff");
    for (unsigned shift = 0; shift < 64; shift += 4) {
        const auto value = UINT64_C(15) << shift;
        assert(std::string(FormatLessonStorageHilUint64Hex(value).c_str()) ==
               "f" + std::string(shift / 4, '0'));
    }
}
''')


def test_actual_boot_log_uses_rom_supported_conversions(tmp_path):
    source = (ROOT / "main/lesson_cinematic_evidence.cc").read_text()
    call = re.search(r'esp_rom_printf\("CINE_EVIDENCE event=boot[\s\S]*?\);', source)
    assert call is not None
    compile_and_run(tmp_path, r'''
#include "lesson_storage_hil_u64_format.h"
#include <cassert>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
std::string output;
void esp_rom_printf(const char* format, ...) {
    // ROM printf cannot consume a 64-bit integer; reject before reading varargs.
    assert(std::strstr(format, "%ll") == nullptr);
    assert(std::strstr(format, "%l") == nullptr);
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int size = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    assert(size > 0 && static_cast<size_t>(size) < sizeof(buffer));
    output = buffer;
}
struct Boot {
    uint64_t boot_nonce;
    const char* reset_reason = "power_on";
    unsigned lifetime_internal_heap_min = 123;
    unsigned psram_heap_min = 456;
} g_boot;
void Log() {
''' + call.group() + r'''
}
int main() {
    for (const auto value : {UINT64_C(0), UINT64_MAX}) {
        g_boot.boot_nonce = value;
        Log();
        const std::string nonce = value == 0 ? "0" : "ffffffffffffffff";
        assert(output == "CINE_EVIDENCE event=boot boot_nonce=0x" + nonce +
            " reset_reason=power_on lifetime_internal_heap_min=123 psram_heap_min=456\n");
    }
}
''')
