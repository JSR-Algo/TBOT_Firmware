from pathlib import Path
import re

from test_lesson_cinematic_log_format import compile_and_run


SOURCE = Path(__file__).resolve().parents[1] / "main" / "lesson_layered_cinematic_renderer.cc"
V3_SOURCE = Path(__file__).resolve().parents[1] / "main" / "lesson_cinematic_renderer.cc"


def test_prepare_logs_each_layer_decode_boundary_with_elapsed_and_deadline():
    source = SOURCE.read_text()

    assert '"prepare decode layer=background frame=static elapsed_ms=%"' in source
    assert '"prepare decode layer=teachingObject frame=static elapsed_ms=%"' in source
    assert '"prepare decode layer=robotOverlay frame=%u elapsed_ms=%"' in source
    assert '" deadline_ms=%"' in source
    assert 'config.phase_id' in source
    assert source.index("const bool decoded = ops_.decode_video") < source.index(
        '"prepare decode layer=robotOverlay frame=%u elapsed_ms=%"'
    ) < source.index("if (!decoded)")


def test_v3_logs_every_mp4_decode_attempt_before_returning_the_error():
    source = V3_SOURCE.read_text()
    decode_call = "const bool decoded = ops_.decode("
    diagnostic = '"decode layer=%u frame=%u elapsed_ms=%s"'

    assert decode_call in source
    assert diagnostic in source
    assert '" deadline_ms=%s decoded=%d operation_error=%u"' in source
    assert source.index(decode_call) < source.index(diagnostic) < source.index("if (!decoded)")


def test_actual_v3_decode_log_preserves_uint64_values_and_failure(tmp_path):
    source = V3_SOURCE.read_text()
    call = re.search(r'ESP_LOGI\("LessonCinematic",\s*"decode layer=[\s\S]*?\);', source)
    assert call is not None
    compile_and_run(tmp_path, r'''
#include "lesson_storage_hil_u64_format.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
std::string output;
void ESP_LOGI(const char* tag, const char* format, ...) {
    assert(std::string(tag) == "LessonCinematic");
    assert(std::strstr(format, "%l") == nullptr);
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int size = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    assert(size > 0 && static_cast<size_t>(size) < sizeof(buffer));
    output = buffer;
}
void Log(uint64_t started, uint64_t finished, bool decoded) {
    const unsigned layer_index = 1, frame_index = 7;
    const uint64_t decode_deadline_ms = UINT64_MAX;
    const unsigned operation_error = decoded ? 0 : 3;
''' + call.group() + r'''
}
int main() {
    Log(0, UINT64_MAX, false);
    assert(output == "decode layer=1 frame=7 elapsed_ms=18446744073709551615"
        " deadline_ms=18446744073709551615 decoded=0 operation_error=3");
    Log(1, 0, true);
    assert(output == "decode layer=1 frame=7 elapsed_ms=0"
        " deadline_ms=18446744073709551615 decoded=1 operation_error=0");
}
''')
