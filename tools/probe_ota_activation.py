#!/usr/bin/env python3
"""
probe_ota_activation.py — does the live TBOT OTA endpoint speak a 6-digit code?

Closes the residual gap in docs/findings/2026-06-16-xiaozhi-activation-code-vestigial.md:
the xiaozhi activation path is purely server-driven — the robot speaks/shows a
6-digit code on boot IFF the OTA CheckVersion response contains an `activation`
object. This tool replays that exact request and reports whether the endpoint
emits one, without needing a robot to actually boot.

Two modes:

  probe     Replay the firmware's OTA CheckVersion POST against a URL (preset or
            custom) using a synthetic fake MAC, and report whether the response
            carries an `activation` object (the thing that makes the robot talk).

  read-nvs  Best-effort: dump the device's NVS partition over serial (esptool)
            and recover the seeded `wifi/ota_url` / `provisioning_url` so you know
            which host THIS unit actually hits. Then feed that URL back into `probe`.

Faithful to firmware:
  - main/ota.cc:46-72  GetCheckVersionUrl + SetupHttp headers
  - main/boards/common/board.cc:112-158  GetSystemInfoJson body
  - server trigger: esp32-server DeviceServiceImpl.checkDeviceActive — an
    `activation` object is returned when getDeviceByMacAddress(MAC) == null.

Dependencies: Python 3 stdlib only (urllib, json). esptool only for read-nvs.

SAFETY: `probe` defaults to a synthetic locally-administered MAC (02:..:..) so it
never touches a real device's binding. Pass --mac <real-mac> only if you want to
prove the "spoken on every boot even after pairing" recurrence for a known unit.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

# Candidate OTA endpoints seen across the repos (finding §"Residual uncertainty").
# All /tbot/ota/ variants are the Java esp32-server Spring context-path; only that
# server emits `activation`. The /v1/ota/ example is a different path shape.
PRESETS = {
    "kconfig":      "https://tbot-backend-8wmh.onrender.com/tbot/ota/",          # firmware compile-time default
    "render-tunnel":"https://luggage-spears-louisville-psychology.trycloudflare.com/tbot/ota/",  # render.yaml TBOT_OTA_URL
    "skylabs":      "https://api.skylabs.vn/v1/ota/",                            # bootstrap.controller.ts Swagger example
}

# NVS partition for ESP32-S3 image (partitions: nvs, data, nvs, 0x9000, 0x4000).
NVS_OFFSET = 0x9000
NVS_SIZE = 0x4000


def synth_mac() -> str:
    """A clearly-synthetic, locally-administered MAC (02:..) that can't collide
    with a manufactured device address. Random per run (script, not workflow)."""
    b = bytearray(os.urandom(6))
    b[0] = 0x02  # locally administered, unicast
    return ":".join(f"{x:02x}" for x in b)


def synth_uuid() -> str:
    r = os.urandom(16)
    h = r.hex()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"


def build_body(mac: str, uuid: str, app_version: str, board_type: str) -> dict:
    """Mirror board.cc GetSystemInfoJson closely enough that the server populates
    buildActivation() normally. Only the Device-Id header MAC gates `activation`,
    but we send a faithful body so a strict server doesn't reject the report."""
    return {
        "version": 2,
        "language": "en-US",
        "flash_size": 16777216,
        "minimum_free_heap_size": 100000,
        "mac_address": mac,
        "uuid": uuid,
        "chip_model_name": "esp32s3",
        "chip_info": {"model": 9, "cores": 2, "revision": 0, "features": 18},
        "application": {
            "name": "tbot",
            "version": app_version,
            "compile_time": "2026-01-01T00:00:00Z",
            "idf_version": "v5.5.4",
            "elf_sha256": "0" * 64,
        },
        "ota": {"label": "ota_0"},
        "board": {"type": board_type, "name": board_type},
    }


def do_probe(url: str, mac: str, app_version: str, board_type: str, timeout: float) -> int:
    uuid = synth_uuid()
    body = json.dumps(build_body(mac, uuid, app_version, board_type)).encode()
    headers = {
        "Activation-Version": "1",
        "Device-Id": mac,            # <-- the activation trigger key (server keys off this)
        "Client-Id": uuid,
        "User-Agent": "tbot-ota-probe/1.0",
        "Accept-Language": "en-US",
        "Content-Type": "application/json",
    }
    print(f"→ POST {url}")
    print(f"  Device-Id (MAC): {mac}   [{'SYNTHETIC' if mac.startswith('02:') else 'CALLER-SUPPLIED'}]")
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.getcode()
            raw = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        status = e.code
        raw = e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        print(f"  ✗ request failed: {e}")
        print("\nVERDICT: endpoint unreachable from here — cannot determine activation behavior.")
        return 2

    print(f"← HTTP {status}")
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError:
        print("  response was not JSON (first 300 chars):")
        print("  " + raw[:300].replace("\n", "\n  "))
        print("\nVERDICT: not a CheckVersion JSON response — this host likely does not serve OTA here.")
        return 2

    has = lambda k: isinstance(doc, dict) and k in doc
    print(f"  keys: {sorted(doc.keys()) if isinstance(doc, dict) else type(doc).__name__}")
    for k in ("firmware", "websocket", "mqtt", "server_time"):
        if has(k):
            print(f"  · {k}: present")

    act = doc.get("activation") if isinstance(doc, dict) else None
    print()
    if isinstance(act, dict):
        code = act.get("code")
        msg = (act.get("message") or "").replace("\n", " ⏎ ")
        chal = act.get("challenge")
        print("VERDICT: ⚠️  ACTIVATION OBJECT PRESENT — this endpoint makes the robot SPEAK/SHOW a code.")
        print(f"         code={code!r}  challenge={chal!r}")
        print(f"         message={msg!r}")
        print("         => xiaozhi activation path is LIVE here. Finding confirmed against this host.")
        return 1
    print("VERDICT: ✅ no `activation` object for this MAC — robot would NOT speak a code via this host.")
    print("         (If you used a REAL paired MAC and still see no activation, that host has a device")
    print("          row for it; with a synthetic MAC, this host simply isn't the xiaozhi Java OTA.)")
    return 0


def do_read_nvs(port: str, baud: int, chip: str) -> int:
    dump = "nvs_dump.bin"
    cmd = ["esptool.py", "--chip", chip, "--port", port, "--baud", str(baud),
           "read_flash", hex(NVS_OFFSET), hex(NVS_SIZE), dump]
    print("→ " + " ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        print("✗ esptool.py not found. Install ESP-IDF or `pip install esptool`.")
        return 2
    except subprocess.CalledProcessError as e:
        print(f"✗ esptool failed (rc={e.returncode}). Is the device on {port} and not held by idf monitor?")
        return 2

    data = open(dump, "rb").read()
    print(f"\nScanned {len(data)} bytes of NVS for seeded URLs:")
    urls = sorted(set(re.findall(rb"https?://[^\x00-\x20\"']{6,200}", data)))
    if not urls:
        print("  (no URL strings found — keys may be unset; firmware would fall back to CONFIG_OTA_URL)")
    for u in urls:
        print("  · " + u.decode("ascii", "replace"))
    print("\nNext: pass the ota_url above to `probe --url <that-url>` to test it directly.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="replay OTA CheckVersion and check for an activation object")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--url", help="custom OTA CheckVersion URL (…/tbot/ota/ or …/v1/ota/)")
    g.add_argument("--preset", choices=PRESETS.keys(), help="one of the known candidate hosts")
    g.add_argument("--all-presets", action="store_true", help="probe every known candidate host")
    p.add_argument("--mac", default=None, help="override MAC (default: synthetic 02:..). Use a real paired MAC to test 'every-boot' recurrence.")
    p.add_argument("--app-version", default="1.0.0")
    p.add_argument("--board-type", default="esp32s3")
    p.add_argument("--timeout", type=float, default=15.0)

    n = sub.add_parser("read-nvs", help="recover the seeded ota_url from a connected device")
    n.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodem101")
    n.add_argument("--baud", type=int, default=460800)
    n.add_argument("--chip", default="esp32s3")

    args = ap.parse_args()

    if args.cmd == "read-nvs":
        return do_read_nvs(args.port, args.baud, args.chip)

    mac = args.mac or synth_mac()
    if args.all_presets:
        rc = 0
        for name, url in PRESETS.items():
            print(f"\n===== preset: {name} =====")
            rc = max(rc, do_probe(url, mac, args.app_version, args.board_type, args.timeout))
        return rc
    url = args.url or PRESETS[args.preset]
    return do_probe(url, mac, args.app_version, args.board_type, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
