#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo "usage: $0 T09-clip-catalog.json T07-selected-media.json NEW-output-directory [full|static|inventory]" >&2
    exit 2
fi
CATALOG="$1"
SELECTED="$2"
OUTPUT="$3"
MODE="${4:-full}"
mkdir "${OUTPUT}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-actual-media.XXXXXX")"
trap 'rm -rf "${BUILD_DIR}"' EXIT
JPEG="${ROOT}/managed_components/espressif__esp_jpeg"
PNG="${ROOT}/managed_components/lvgl__lvgl/src/libs/lodepng"
mkdir -p "${BUILD_DIR}/lvgl/src/libs/lodepng" "${BUILD_DIR}/lvgl/src/core"
cp "${PNG}/lodepng.c" "${PNG}/lodepng.h" "${BUILD_DIR}/lvgl/src/libs/lodepng/"
cp "${ROOT}/tests/native_stubs_actual_media/lvgl.h" "${BUILD_DIR}/lvgl/lvgl.h"
touch "${BUILD_DIR}/lvgl/src/core/lv_global.h"
python3 - "${CATALOG}" "${SELECTED}" "${OUTPUT}" "${ROOT}" <<'PY'
import hashlib, json, pathlib, sys
catalog_path, selected_path, output, root = map(pathlib.Path, sys.argv[1:])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
catalog, selected = json.loads(catalog_path.read_text()), json.loads(selected_path.read_text())
assert sha(selected_path) == 'a4313875d87ee0776c9559d34458de1c51e4e98c77cfc553e00065039b4254a1'
assets, lines = [], []
for kind, records in [('image', catalog['images']['registeredReviewFixtures']),
                      ('video', list(catalog['phases'].values()))]:
    for record in records:
        if 'localPath' not in record:
            continue
        path = pathlib.Path(record['localPath'])
        assert sha(path) == record['sha256'], path
        m = record['metadata']; rect = m['rect']
        phase = next((name for name, item in catalog['phases'].items() if item is record), '')
        name = phase or ('background' if m['mediaType'] == 'image/jpeg' else record['assetKey'].split('.')[-1])
        assets.append(dict(name=name, path=str(path), sha256=sha(path), bytes=path.stat().st_size, metadata=m))
        lines.append('\t'.join(map(str, [kind, name, path, m['width'], m['height'],
                                       rect['x'], rect['y'], rect['width'], rect['height'],
                                       m.get('fps',0), m.get('frameCount',0), m.get('durationMs',0)])))
assert sum(a['metadata'].get('frameCount',0) for a in assets) == 333
by_sha = {}
for path in catalog_path.parent.rglob('*.mp4'):
    by_sha.setdefault(sha(path), path)
for record in selected['assets']:
    if record.get('mediaType') != 'video/mp4':
        continue
    path = by_sha.get(record['sha256'])
    assert path is not None, record['assetKey']
    m = record['metadata']; rect = m['rect']; name = record['assetKey']
    assets.append(dict(scope='selectedT07', name=name, path=str(path), sha256=sha(path), metadata=m))
    lines.append('\t'.join(map(str, ['selected_video', name, path, m['width'], m['height'],
                                   rect['x'], rect['y'], rect['width'], rect['height'],
                                   m['fps'], m['frameCount'], m['durationMs']])))
source_paths = ['main/lesson_layered_cinematic_renderer.cc', 'main/lesson_chroma_compositor.cc',
 'main/lesson_mjpeg_mp4.cc', 'main/display/lvgl_display/jpg/jpeg_to_image.c',
 'managed_components/espressif__esp_jpeg/jpeg_decoder.c',
 'managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c',
 'managed_components/lvgl__lvgl/src/libs/lodepng/lodepng.c',
 'managed_components/lvgl__lvgl/src/libs/lodepng/lodepng.h',
 'tests/native_stubs_actual_media/lvgl.h', 'tests/native_stubs_actual_media/esp_check.h',
 'tests/native_stubs_jpeg/sdkconfig.h', 'tests/native/lesson_layered_actual_media_test.cc',
 'scripts/run_host_native_lesson_layered_actual_media_test.sh']
evidence = dict(scope='Host pixel evidence; external TJPGD is not ESP32-S3 ROM and present is not TFT transport',
 catalog=dict(path=str(catalog_path),sha256=sha(catalog_path)),
 predecessorSelectedMetadata=dict(path=str(selected_path),sha256=sha(selected_path)),
 assets=assets, sources=[dict(path=str(root/p),sha256=sha(root/p)) for p in source_paths],
 selectedVideoDifferences=[dict(phase=a['name'],current=a['sha256'],selected=s['sha256'])
 for a in assets for s in selected['assets'] if a['name'] in catalog['phases']
 and s.get('assetKey') == 'cpr-t09.robot.'+a['name'] and a['sha256'] != s['sha256']],
 releaseEligible=False)
(output/'inputs.tsv').write_text('\n'.join(lines)+'\n')
(output/'identity.json').write_text(json.dumps(evidence,indent=2)+'\n')
PY
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
INC=(-I"${ROOT}/tests/native_stubs_actual_media" -I"${ROOT}/tests/native_stubs_jpeg" -I"${ROOT}/main/display/lvgl_display/jpg"
     -I"${JPEG}/include" -I"${JPEG}/tjpgd" -I"${ROOT}/main"
     -I"${BUILD_DIR}/lvgl/src/libs/lodepng")
COMPAT=(-include stdint.h -include stdbool.h -include assert.h -include stdlib.h -include esp_heap_caps.h)
"${CC:-clang}" --version > "${OUTPUT}/compiler.txt"
"${CC:-clang}" -std=c11 -O0 -g "${SAN[@]}" "${INC[@]}" \
    -c "${ROOT}/main/display/lvgl_display/jpg/jpeg_to_image.c" -o "${BUILD_DIR}/jpeg_to_image.o"
"${CC:-clang}" -std=c11 -O0 -g "${SAN[@]}" "${INC[@]}" "${COMPAT[@]}" \
    -Wno-incompatible-function-pointer-types -c "${JPEG}/jpeg_decoder.c" -o "${BUILD_DIR}/jpeg_decoder.o"
"${CC:-clang}" -std=c11 -O0 -g "${SAN[@]}" "${INC[@]}" "${COMPAT[@]}" \
    -c "${JPEG}/tjpgd/tjpgd.c" -o "${BUILD_DIR}/tjpgd.o"
"${CC:-clang}" -std=c11 -O0 -g "${SAN[@]}" -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ENCODER \
    -c "${BUILD_DIR}/lvgl/src/libs/lodepng/lodepng.c" -o "${BUILD_DIR}/lodepng.o"
"${CXX:-clang++}" -std=c++17 -O0 -g -Wall -Wextra -Werror "${SAN[@]}" "${INC[@]}" \
    -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ENCODER \
    "${ROOT}/tests/native/lesson_layered_actual_media_test.cc" \
    "${ROOT}/main/lesson_layered_cinematic_renderer.cc" "${ROOT}/main/lesson_chroma_compositor.cc" \
    "${ROOT}/main/lesson_mjpeg_mp4.cc" "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
    "${ROOT}/main/sd_fat_session_guard.cc" "${BUILD_DIR}"/*.o -o "${BUILD_DIR}/actual_media_test"
"${BUILD_DIR}/actual_media_test" "${OUTPUT}/inputs.tsv" "${OUTPUT}" "${MODE}"
