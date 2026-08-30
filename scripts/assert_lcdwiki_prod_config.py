#!/usr/bin/env python3
import re
import sys
from pathlib import Path

PRODUCTION_OTA_URL = "https://esp.tjbot.vn/tbot/ota/"
PRODUCTION_WEBSOCKET_URL = "wss://esp.tjbot.vn/tbot/v1/"


def main() -> int:
    sdkconfig_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("sdkconfig")
    if not sdkconfig_path.exists():
        print(f"Missing sdkconfig: {sdkconfig_path}", file=sys.stderr)
        return 2

    sdkconfig = sdkconfig_path.read_text(encoding="utf-8")
    failures: list[str] = []

    if not re.search(r"^CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y$", sdkconfig, re.MULTILINE):
        failures.append("CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y must be selected for the LCDWiki production build")

    if not re.search(r"^CONFIG_APP_REPRODUCIBLE_BUILD=y$", sdkconfig, re.MULTILINE):
        failures.append("Reproducible application builds must be enabled for LCDWiki production builds")

    if not re.search(rf'^CONFIG_OTA_URL="{re.escape(PRODUCTION_OTA_URL)}"$', sdkconfig, re.MULTILINE):
        failures.append(f'CONFIG_OTA_URL must be "{PRODUCTION_OTA_URL}" for production robot bootstrap')

    if not re.search(
        rf'^CONFIG_WEBSOCKET_URL="{re.escape(PRODUCTION_WEBSOCKET_URL)}"$',
        sdkconfig,
        re.MULTILINE,
    ):
        failures.append(f'WebSocket URL must be "{PRODUCTION_WEBSOCKET_URL}" for production robot sessions')

    if re.search(r"^CONFIG_MBEDTLS_HARDWARE_AES=y$", sdkconfig, re.MULTILINE):
        failures.append("Hardware AES must stay disabled for LCDWiki production builds")

    if "# CONFIG_MBEDTLS_HARDWARE_AES is not set" not in sdkconfig:
        failures.append("Expected '# CONFIG_MBEDTLS_HARDWARE_AES is not set' in the LCDWiki production sdkconfig")

    if re.search(r"^CONFIG_FATFS_LFN_NONE=y$", sdkconfig, re.MULTILINE):
        failures.append("FATFS short-name-only mode must stay disabled for LCDWiki lesson assets")

    if not re.search(r"^CONFIG_FATFS_LFN_HEAP=y$", sdkconfig, re.MULTILINE):
        failures.append("CONFIG_FATFS_LFN_HEAP=y must be enabled for LCDWiki lesson assets")

    if not re.search(r"^CONFIG_TBOT_RELEASE_CINEMATIC_EVIDENCE=y$", sdkconfig, re.MULTILINE):
        failures.append("Release cinematic evidence must be enabled for LCDWiki production builds")

    if re.search(r"^CONFIG_TBOT_HIL_STORAGE_FAULTS=y$", sdkconfig, re.MULTILINE):
        failures.append("HIL storage faults must stay disabled for LCDWiki production builds")

    if re.search(r"^CONFIG_TBOT_HIL_CINEMATIC_TELEMETRY=y$", sdkconfig, re.MULTILINE):
        failures.append("Cinematic HIL telemetry must stay disabled for LCDWiki production builds")

    if failures:
        print("LCDWiki production build config gate failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print("LCDWiki production build config OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
