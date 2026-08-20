#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tbot-goal2-full-python.XXXXXX")"

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
  rm -rf "${STATE_DIR}"
}
trap cleanup EXIT INT TERM

cd "${ROOT}"
snapshot_tree > "${STATE_DIR}/tree-before"
set +e
python3 -m pytest -q "$@"
PYTEST_RC=$?
set -e

snapshot_tree > "${STATE_DIR}/tree-after"

if ! cmp -s "${STATE_DIR}/tree-before" "${STATE_DIR}/tree-after"; then
  echo "worktree changed during full Python gate" >&2
  diff -u "${STATE_DIR}/tree-before" "${STATE_DIR}/tree-after" || true
  exit 3
fi
WORKTREE_SNAPSHOT_SHA256="$(python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "${STATE_DIR}/tree-after")"
WORKTREE_SNAPSHOT_ENTRIES="$(python3 -c 'import sys; print(sum(1 for _ in open(sys.argv[1], "rb")))' "${STATE_DIR}/tree-after")"
echo "WORKTREE_SNAPSHOT_SHA256=${WORKTREE_SNAPSHOT_SHA256}"
echo "WORKTREE_SNAPSHOT_ENTRIES=${WORKTREE_SNAPSHOT_ENTRIES}"
exit "${PYTEST_RC}"
