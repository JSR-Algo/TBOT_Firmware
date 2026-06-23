"""Contracts for the packaged lesson asset pack.

The ESP/firmware path must not depend on inline media or oversized per-step
payloads. These tests pin the concrete files used by the barn lesson render
contract: all layer assets exist, match CHECKSUMS.sha256, and stay below the
single-image and per-step compressed-byte budgets the firmware can safely fetch.
"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LESSON_ROOT = ROOT / "lesson"
CHECKSUMS = LESSON_ROOT / "CHECKSUMS.sha256"
RENDER_CONTRACT = json.loads((LESSON_ROOT / "render-contract.json").read_text(encoding="utf-8"))
LESSON = json.loads((LESSON_ROOT / "lesson.json").read_text(encoding="utf-8"))["lesson"]

MAX_LESSON_IMAGE_BYTES = 512 * 1024
MAX_STEP_LAYER_BYTES = 512 * 1024
MAX_ESPTFT_DECODED_RGB565_BYTES = 320 * 240 * 2
ESPTFT_DISPLAY_WIDTH = 480
ESPTFT_DISPLAY_HEIGHT = 320
ESPTFT_MAX_BODY_PX = RENDER_CONTRACT["robot"]["profiles"]["espTft"]["maxBodyPx"]


def _checksums() -> dict[str, str]:
    checksums: dict[str, str] = {}
    for line in CHECKSUMS.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        digest, rel = line.split(maxsplit=1)
        checksums[rel.removeprefix("./")] = digest
    return checksums


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _step_pose(step: dict) -> str:
    return str(step["pose"])


def _render_layer_paths_for_step(step: dict) -> tuple[str, str, str]:
    pose_assets = RENDER_CONTRACT["robot"]["poseAssets"]
    return (
        RENDER_CONTRACT["backgroundVideo"]["poster"],
        RENDER_CONTRACT["teachingObject"]["asset"],
        pose_assets[_step_pose(step)],
    )

def _render_contract_asset_refs(value) -> set[str]:
    refs: set[str] = set()
    if isinstance(value, dict):
        for child in value.values():
            refs.update(_render_contract_asset_refs(child))
    elif isinstance(value, list):
        for child in value:
            refs.update(_render_contract_asset_refs(child))
    elif isinstance(value, str) and value.startswith("assets/"):
        refs.add(value)
    return refs


def _image_dimensions(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        width, height = struct.unpack(">II", data[16:24])
        return width, height

    if data[:2] == b"\xff\xd8":
        offset = 2
        while offset + 9 < len(data):
            while offset < len(data) and data[offset] == 0xFF:
                offset += 1
            marker = data[offset]
            offset += 1
            if marker in {0xD8, 0xD9}:
                continue
            if offset + 2 > len(data):
                break
            length = int.from_bytes(data[offset:offset + 2], "big")
            if length < 2 or offset + length > len(data):
                break
            if marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
                height = int.from_bytes(data[offset + 3:offset + 5], "big")
                width = int.from_bytes(data[offset + 5:offset + 7], "big")
                return width, height
            offset += length

    raise AssertionError(f"unsupported image header: {path}")


def _decoded_rgb565_bytes(path: Path) -> int:
    width, height = _image_dimensions(path)
    return width * height * 2


def test_render_contract_layer_assets_exist_and_match_checksums():
    checksums = _checksums()
    required = {
        RENDER_CONTRACT["backgroundVideo"]["poster"],
        RENDER_CONTRACT["teachingObject"]["asset"],
        *RENDER_CONTRACT["teachingObject"]["supportAssets"].values(),
        *(RENDER_CONTRACT["robot"]["poseAssets"][step["pose"]] for step in RENDER_CONTRACT["stepRenderMap"]),
    }

    for rel in sorted(required):
        path = LESSON_ROOT / rel
        assert path.exists(), rel
        assert rel in checksums, rel
        assert _sha256(path) == checksums[rel]

def test_all_render_contract_asset_references_exist_and_match_checksums():
    checksums = _checksums()
    refs = _render_contract_asset_refs(RENDER_CONTRACT)

    assert refs
    assert RENDER_CONTRACT["backgroundVideo"]["file"] in refs
    assert set(RENDER_CONTRACT["referenceImages"]).issubset(refs)
    for rel in sorted(refs):
        path = LESSON_ROOT / rel
        assert path.exists(), rel
        assert rel in checksums, rel
        assert _sha256(path) == checksums[rel]

def test_lesson_package_checksums_cover_every_packaged_file_without_stale_entries():
    checksums = _checksums()
    packaged_files = {
        str(path.relative_to(LESSON_ROOT))
        for path in LESSON_ROOT.rglob("*")
        if path.is_file() and path.name != "CHECKSUMS.sha256"
    }

    assert set(checksums) == packaged_files
    for rel, digest in checksums.items():
        assert _sha256(LESSON_ROOT / rel) == digest


def test_render_contract_object_png_metadata_matches_packaged_file():
    rel = RENDER_CONTRACT["teachingObject"]["asset"]
    metadata = RENDER_CONTRACT["teachingObject"]["objectPng"]
    width, height = _image_dimensions(LESSON_ROOT / rel)

    assert metadata["width"] == width
    assert metadata["height"] == height
    assert metadata["sha256"] == _sha256(LESSON_ROOT / rel)


def test_render_contract_image_assets_fit_single_fetch_budget():
    for step in RENDER_CONTRACT["stepRenderMap"]:
        for rel in _render_layer_paths_for_step(step):
            size = (LESSON_ROOT / rel).stat().st_size
            assert size <= MAX_LESSON_IMAGE_BYTES, f"{rel} is {size} bytes"


def test_esptft_render_layer_images_fit_decoded_pixel_budget():
    background_rel = RENDER_CONTRACT["backgroundVideo"]["poster"]
    object_rels = {
        RENDER_CONTRACT["teachingObject"]["asset"],
        *RENDER_CONTRACT["teachingObject"]["supportAssets"].values(),
    }
    pose_rels = set(RENDER_CONTRACT["robot"]["poseAssets"].values())
    runtime_rels = {background_rel, *object_rels, *pose_rels}

    for rel in sorted(runtime_rels):
        path = LESSON_ROOT / rel
        width, height = _image_dimensions(path)
        decoded_bytes = _decoded_rgb565_bytes(path)
        assert decoded_bytes <= MAX_ESPTFT_DECODED_RGB565_BYTES, (
            f"{rel} decodes to {decoded_bytes} RGB565 bytes at {width}x{height}"
        )

        if rel == background_rel:
            assert width <= ESPTFT_DISPLAY_WIDTH and height <= ESPTFT_DISPLAY_HEIGHT, (
                f"{rel} is {width}x{height}, larger than espTft display "
                f"{ESPTFT_DISPLAY_WIDTH}x{ESPTFT_DISPLAY_HEIGHT}"
            )
        elif rel in object_rels or rel in pose_rels:
            assert max(width, height) <= ESPTFT_MAX_BODY_PX, (
                f"{rel} is {width}x{height}, larger than espTft maxBodyPx "
                f"{ESPTFT_MAX_BODY_PX}"
            )


def test_each_lesson_step_layer_pack_stays_under_compressed_byte_budget():
    for step in RENDER_CONTRACT["stepRenderMap"]:
        layer_paths = _render_layer_paths_for_step(step)
        total = sum((LESSON_ROOT / rel).stat().st_size for rel in layer_paths)
        assert total <= MAX_STEP_LAYER_BYTES, f"{step['stepId']} layer pack is {total} bytes: {layer_paths}"

def test_full_story_render_contract_maps_every_packaged_step_to_three_image_layers():
    lesson_steps = LESSON["steps"]
    render_steps = RENDER_CONTRACT["stepRenderMap"]
    assert [step["id"] for step in lesson_steps] == [step["stepId"] for step in render_steps]
    assert [step["type"] for step in lesson_steps] == [step["stepType"] for step in render_steps]

    expected_pose_by_step = {
        "s1": "teach",
        "s2": "teach",
        "s3": "teach",
        "s4": "teach",
        "s5": "listening",
        "s6": "listening",
        "s7": "thinking",
        "s8": "teach",
        "s9": "celebrate",
    }
    for step in render_steps:
        background, teaching_object, robot_overlay = _render_layer_paths_for_step(step)
        assert background == "assets/background/barn-round-field-poster.jpg"
        assert teaching_object == "assets/objects/barn.png"
        assert step["pose"] == expected_pose_by_step[step["stepId"]]
        assert robot_overlay == f"assets/robot/poses/bright-{step['pose']}.png"


def test_esptft_render_steps_use_only_poster_not_full_video_background():
    video = LESSON_ROOT / RENDER_CONTRACT["backgroundVideo"]["file"]
    assert video.exists()
    assert video.stat().st_size > MAX_LESSON_IMAGE_BYTES

    for step in RENDER_CONTRACT["stepRenderMap"]:
        background, _, _ = _render_layer_paths_for_step(step)
        assert background == RENDER_CONTRACT["backgroundVideo"]["poster"]
        assert background != RENDER_CONTRACT["backgroundVideo"]["file"]
