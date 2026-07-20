#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDKCONFIG="${ROOT}/sdkconfig"
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-goal2-full-python.XXXXXX")"
SDKCONFIG_BACKUP="${STATE_DIR}/sdkconfig"
SDKCONFIG_WAS_PRESENT=0
SDKCONFIG_MODE=""
RESTORED=0
VERIFYING=0

file_mode() {
  python3 -c 'import os, stat, sys; print(f"{stat.S_IMODE(os.stat(sys.argv[1]).st_mode):04o}")' "$1"
}

snapshot_tree() {
  git ls-files -z --cached --others --exclude-standard | python3 -c '
import hashlib
import os
import stat
import sys

root = os.fsencode(os.getcwd())
paths = sorted(path for path in sys.stdin.buffer.read().split(b"\0") if path)
for path in paths:
    full = os.path.join(root, path)
    try:
        metadata = os.lstat(full)
    except FileNotFoundError:
        print(f"{path.hex()} missing")
        continue
    mode = f"{stat.S_IMODE(metadata.st_mode):04o}"
    if stat.S_ISREG(metadata.st_mode):
        kind = "file"
        with open(full, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
    elif stat.S_ISLNK(metadata.st_mode):
        kind = "symlink"
        digest = hashlib.sha256(os.readlink(full)).hexdigest()
    else:
        kind = "other"
        digest = hashlib.sha256(b"").hexdigest()
    print(f"{path.hex()} {kind} {mode} {digest}")
'
}

cleanup() {
  if [[ "${RESTORED}" -eq 0 ]]; then
    if [[ "${SDKCONFIG_WAS_PRESENT}" -eq 1 ]]; then
      cp -p "${SDKCONFIG_BACKUP}" "${SDKCONFIG}"
      chmod "${SDKCONFIG_MODE}" "${SDKCONFIG}"
    else
      rm -f "${SDKCONFIG}"
    fi
    RESTORED=1
  fi
  if [[ "${VERIFYING}" -eq 0 ]]; then
    rm -rf "${STATE_DIR}"
  fi
}
trap cleanup EXIT INT TERM

cd "${ROOT}"
snapshot_tree > "${STATE_DIR}/tree-before"
if [[ -e "${SDKCONFIG}" ]]; then
  if [[ ! -f "${SDKCONFIG}" || -L "${SDKCONFIG}" ]]; then
    echo "sdkconfig fixture target must be absent or a regular file" >&2
    exit 2
  fi
  SDKCONFIG_WAS_PRESENT=1
  SDKCONFIG_MODE="$(file_mode "${SDKCONFIG}")"
  cp -p "${SDKCONFIG}" "${SDKCONFIG_BACKUP}"
fi

printf '%s\n' 'CONFIG_JD_USE_ROM=y' > "${SDKCONFIG}"
set +e
python3 -m pytest -q
PYTEST_RC=$?
set -e

VERIFYING=1
cleanup
trap - EXIT INT TERM
if [[ "${SDKCONFIG_WAS_PRESENT}" -eq 1 ]]; then
  cmp -s "${SDKCONFIG_BACKUP}" "${SDKCONFIG}"
  [[ "$(file_mode "${SDKCONFIG}")" == "${SDKCONFIG_MODE}" ]]
else
  [[ ! -e "${SDKCONFIG}" ]]
fi
snapshot_tree > "${STATE_DIR}/tree-after"

if ! cmp -s "${STATE_DIR}/tree-before" "${STATE_DIR}/tree-after"; then
  echo "worktree changed during full Python gate" >&2
  diff -u "${STATE_DIR}/tree-before" "${STATE_DIR}/tree-after" || true
  rm -rf "${STATE_DIR}"
  exit 3
fi
WORKTREE_SNAPSHOT_SHA256="$(python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "${STATE_DIR}/tree-after")"
WORKTREE_SNAPSHOT_ENTRIES="$(python3 -c 'import sys; print(sum(1 for _ in open(sys.argv[1], "rb")))' "${STATE_DIR}/tree-after")"
echo "WORKTREE_SNAPSHOT_SHA256=${WORKTREE_SNAPSHOT_SHA256}"
echo "WORKTREE_SNAPSHOT_ENTRIES=${WORKTREE_SNAPSHOT_ENTRIES}"
rm -rf "${STATE_DIR}"
exit "${PYTEST_RC}"
