#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${TMPDIR:?owned TMPDIR required}"
: "${CJSON_DIR:?qualified cJSON required}"
BUILD_DIR="$(mktemp -d "${TMPDIR}/retained-mcp.XXXXXX")"
export TBOT_RETAINED_TEST_STATE_PATH="${BUILD_DIR}/selection.record"
python3 - "${ROOT}" "${BUILD_DIR}" <<'PY'
from pathlib import Path
import sys
root, build = map(Path, sys.argv[1:])
source = (root / 'main/mcp_server.cc').read_text()
helpers = source[source.index('const char* JsonStringField('):source.index('void DownloadLessonAssetToVerifiedFile(')]
tools = source[source.index('    AddUserOnlyTool("self.lesson_assets.selection_state"'):]
tools = tools[:tools.index('\n#endif')]
# Only storage roots change; generated exact handler/validator text is retained.
owned = str(build / 'sdcard')
header = 'constexpr size_t kLessonAssetSyncMaxAssets = 64;\nconstexpr const char* kLessonAssetPackRoot = "'+owned+'/tbot/lesson-assets/";\n'
(build / 'retained_mcp_handlers.inc').write_text(header + helpers + '\nvoid Register() {\n' + tools + '\n}\n')
policy = (root / 'main/lesson_asset_sync_path_policy.cc').read_text().replace('/sdcard', owned)
(build / 'lesson_asset_sync_path_policy.cc').write_text(policy)
PY
"${CC:-clang}" -std=c99 -I"${CJSON_DIR}" -c "${CJSON_DIR}/cJSON.c" -o "${BUILD_DIR}/cJSON.o"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DTBOT_LESSON_ASSET_CACHE_EVICT_TESTING \
  -DTBOT_LESSON_ASSET_ROOT="\"${BUILD_DIR}/sdcard/tbot/lesson-assets\"" \
  -I"${BUILD_DIR}" -I"${ROOT}/tests/native_stubs_mcp" -I"${ROOT}/tests/native_stubs" \
  -I"${ROOT}/tests/native_stubs_transfer" -I"${ROOT}/main" -I"${CJSON_DIR}" \
  "${ROOT}/tests/native/retained_mcp_host_test.cc" \
  "${BUILD_DIR}/lesson_asset_sync_path_policy.cc" \
  "${ROOT}/main/lesson_asset_retained_parser.cc" \
  "${ROOT}/main/lesson_asset_retained_selection.cc" \
  "${ROOT}/main/lesson_asset_cache_evict.cc" \
  "${ROOT}/main/lesson_asset_pack_activation.cc" \
  "${ROOT}/main/lesson_asset_storage_coordinator.cc" \
  "${ROOT}/main/sd_fat_session_guard.cc" \
  "${ROOT}/main/lesson_asset_sync_attestation.cc" \
  "${BUILD_DIR}/cJSON.o" -o "${BUILD_DIR}/retained-mcp-test"
"${BUILD_DIR}/retained-mcp-test" "${RETAINED_CONTRACT_VECTORS:?shared vectors required}"
