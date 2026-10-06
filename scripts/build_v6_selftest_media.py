#!/usr/bin/env python3
"""Packs the attended-lab renderer v6 self-test media image for the "v6media" partition.

Usage: build_v6_selftest_media.py --scene SCENE.json --frame-vectors FRAME_STATE_VECTORS.json
           --cache-key KEY --size BYTES --out IMAGE [--original ASSET_VERSION_ID=FILE ...]

Every original named by the scene must be supplied and match the scene's sha256 and
byte count. Layout: see main/lesson_original_source_device_selftest.h.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

MAGIC = b"TBV6MED1"
NAME_BYTES = 96
ENTRY_BYTES = NAME_BYTES + 4 + 4 + 32
PLAN = "selftest.json"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--frame-vectors", required=True, type=Path)
    parser.add_argument("--cache-key", required=True)
    parser.add_argument("--size", required=True, type=lambda v: int(v, 0))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--original", action="append", default=[])
    args = parser.parse_args()

    scene_bytes = args.scene.read_bytes()
    scene = json.loads(scene_bytes)
    supplied = dict(pair.split("=", 1) for pair in args.original)
    entries = [("scene.original-source.v1", scene_bytes)]
    for original in scene["originals"]:
        version = original["assetVersionId"]
        data = Path(supplied.pop(version)).read_bytes()
        if hashlib.sha256(data).hexdigest() != original["sha256"] or len(data) != original["bytes"]:
            raise SystemExit(f"original {version} does not match the scene")
        entries.append((f"original.{version}", data))
    if supplied:
        raise SystemExit(f"originals not in the scene: {sorted(supplied)}")
    cues = [{"cueId": cue["cueId"], "durationMs": cue["durationMs"]}
            for cue in json.loads(args.frame_vectors.read_text())["cues"]]
    plan = {"cacheKey": args.cache_key, "sceneSha256": hashlib.sha256(scene_bytes).hexdigest(),
            "sceneBytes": len(scene_bytes), "cues": cues}
    entries.append((PLAN, json.dumps(plan, separators=(",", ":")).encode()))

    data_start = 16 + ENTRY_BYTES * len(entries)
    index = bytearray(MAGIC + struct.pack("<II", len(entries), 0))
    data = bytearray()  # bytes from data_start
    for name, payload in entries:
        encoded = name.encode()
        if len(encoded) >= NAME_BYTES:
            raise SystemExit(f"name too long: {name}")
        offset = (data_start + len(data) + 3) & ~3
        data += b"\0" * (offset - data_start - len(data))
        index += encoded.ljust(NAME_BYTES, b"\0") + struct.pack("<II", offset, len(payload))
        index += hashlib.sha256(payload).digest()
        data += payload
    image = bytes(index) + bytes(data)
    if len(image) > args.size:
        raise SystemExit(f"image {len(image)} bytes exceeds partition {args.size}")
    args.out.write_bytes(image)
    print(json.dumps({"bytes": len(image), "partition": args.size, "entries": len(entries), "cues": len(cues),
                      "sceneSha256": plan["sceneSha256"], "imageSha256": hashlib.sha256(image).hexdigest()}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
