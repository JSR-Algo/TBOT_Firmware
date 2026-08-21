#!/usr/bin/env bash
# repo: robot/TBOT-Firmware
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

source = Path("main/mcp_server.cc").read_text(encoding="utf-8")
start = source.index('AddUserOnlyTool("self.lesson_assets.sync_to_sd"')
end = source.index('AddUserOnlyTool("self.assets.set_download_url"', start)
body = source[start:end]
reuse_start = body.index("if (reusable != nullptr)")
reuse_end = body.index("} else {", reuse_start)
reuse = body[reuse_start:reuse_end]

assert "reused += 1;" in reuse
assert "skipped += 1;" not in reuse
PY
