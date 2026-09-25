"""Build real vendored LVGL and run extracted production display methods."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from test_protocol_work_lifetime import method

build = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
selected = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else ROOT
build.mkdir(parents=True, exist_ok=True)
output.mkdir(parents=True, exist_ok=True)
lcd = (selected / "main/display/lcd_display.cc").read_text()
base = (selected / "main/display/lvgl_display/lvgl_display.cc").read_text()
methods = [method(base, name) for name in (
    "LvglDisplay::LvglDisplay()", "void LvglDisplay::SetStatus",
    "void LvglDisplay::ShowNotification(const char*", "void LvglDisplay::UpdateStatusBar",
)]
methods += [method(lcd, name) for name in (
    "bool LcdDisplay::PresentLessonFramebuffer", "void LcdDisplay::SetLessonMode",
    "void LcdDisplay::SetEmotion", "void LcdDisplay::SetLessonCaption", "void LcdDisplay::SetHideSubtitle",
)]
for name in ("void LcdDisplay::SetChatMessage", "void LcdDisplay::SetPreviewImage", "void LcdDisplay::ClearChatMessages"):
    first = lcd.index(name)
    methods.append(method(lcd[first + len(name):], name))
source = output / "selected-production-display.cc"
source.write_text((ROOT / "tests/native/lesson_display_lvgl_test.cc").read_text().replace(
    "// PRODUCTION_METHODS", "\n".join(methods)))
config = output / "lv_conf.h"
config.write_text("""#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
""")
commands = [
    ["cmake", "-S", str(ROOT / "managed_components/lvgl__lvgl"), "-B", str(build / "lvgl"),
     "-DCMAKE_BUILD_TYPE=Debug", "-DCONFIG_LV_BUILD_DEMOS=OFF", "-DCONFIG_LV_BUILD_EXAMPLES=OFF",
     "-DCONFIG_LV_USE_THORVG_INTERNAL=OFF", f"-DLV_BUILD_CONF_PATH={config}",
     "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
     "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"],
    ["cmake", "--build", str(build / "lvgl"), "-j", "6"],
    ["c++", "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
     "-I", str(ROOT / "managed_components/lvgl__lvgl"), "-I", str(ROOT / "main"),
     f'-DLV_CONF_PATH="{config}"', str(source), str(build / "lvgl/lib/liblvgl.a"),
     "-o", str(build / "display")],
    [str(build / "display")],
]
pins = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [selected / "main/display/lcd_display.cc", selected / "main/display/lcd_display.h",
                     selected / "main/display/lvgl_display/lvgl_display.cc",
                     selected / "main/display/lvgl_display/lvgl_display.h",
                     ROOT / "tests/native/lesson_display_lvgl_test.cc"]}
(output / "input-pins.json").write_text(json.dumps(pins, indent=2) + "\n")
(output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
for index, command in enumerate(commands):
    with (output / f"command-{index}.log").open("w") as log:
        result = subprocess.run(command, cwd=output, stdout=log, stderr=subprocess.STDOUT)
    print(f"command {index}: exit {result.returncode}", flush=True)
    if result.returncode:
        print((output / f"command-{index}.log").read_text()[-6000:])
        raise SystemExit(result.returncode)
shutil.copy2(build / "display", output / "display")
print((output / "command-3.log").read_text())
(output / "result.json").write_text(json.dumps({
    "status": "PASS", "scope": "actual LVGL 9.5.0 software tree/draw/flush with host ESP service adapters",
    "pixelsChecked": 153600, "physicalTft": False,
    "artifacts": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir() if p.is_file()},
}, indent=2) + "\n")
