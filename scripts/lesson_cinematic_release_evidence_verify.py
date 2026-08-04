#!/usr/bin/env python3
"""Offline verifier for a five-pass cinematic release evidence bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

if __package__:
    from .lesson_cinematic_evidence_log_verify import (
        parse_boot_nonce,
        parse_evidence_lines,
        verify as verify_cinematic_log,
    )
else:
    from lesson_cinematic_evidence_log_verify import (
        parse_boot_nonce,
        parse_evidence_lines,
        verify as verify_cinematic_log,
    )

SCHEMA = "lesson-cinematic-hil-evidence.v1"
LIVE_IDENTITY = (
    "Google Live session identity model=gemini-3.1-flash-live-preview "
    "voice=Kore language=vi-VN"
)
RESET_SIGNATURES = (
    "guru meditation",
    "panic'ed",
    "abort()",
    "task watchdog",
    "tg1wdt_sys_rst",
    "rtcwdt",
    "brownout",
    "esp-rom:",
    "rst:",
)
UNSAFE_RETRY = re.compile(r"(?i)(?:\bwrong\b|\bsai\b|không đúng)")
MEMORY_FAILURE = re.compile(
    r"(?i)(?:\boom\b|out of memory|\benomem\b|"
    r"(?:malloc|(?:memory|frame|buffer|heap)\s+allocation)"
    r"[^\n]*(?:fail|error|nullptr|null|enomem))"
)
RENDERER_DEGRADED = re.compile(r"(?i)\b(?:fallback|degraded)\b")


def _resolve(base: Path, raw: Any) -> Path:
    path = Path(str(raw or ""))
    return path if path.is_absolute() else base / path


def _canonical(path: Path) -> Path:
    return path.resolve(strict=False)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _timestamp(raw: Any, label: str, problems: list[str]) -> datetime | None:
    value = str(raw or "")
    if not value.endswith("Z"):
        problems.append(f"{label} must be a UTC Z timestamp")
        return None
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError:
        problems.append(f"{label} is not a valid timestamp")
        return None
    if parsed.tzinfo != timezone.utc:
        problems.append(f"{label} must be UTC")
        return None
    return parsed


def _check_hash(path: Path, expected: Any, label: str, problems: list[str]) -> None:
    if not path.is_file():
        problems.append(f"missing {label}: {path}")
        return
    actual = _sha256(path)
    if expected != actual:
        problems.append(f"{label} SHA-256 mismatch")


def _check_flash(manifest: dict[str, Any], base: Path, problems: list[str]) -> datetime | None:
    flash = manifest.get("flash")
    if not isinstance(flash, dict):
        problems.append("missing flash evidence")
        return None
    if flash.get("mode") != "app-only":
        problems.append("flash mode must be app-only")
    if flash.get("offset") != "0x20000":
        problems.append("flash offset must be 0x20000")
    binary = _canonical(_resolve(base, flash.get("binary")))
    if binary.name != "xiaozhi.bin":
        problems.append("app binary must be named xiaozhi.bin")
    _check_hash(binary, flash.get("sha256"), "app binary", problems)
    app_size = binary.stat().st_size if binary.is_file() else None
    app_hash = _sha256(binary) if binary.is_file() else None

    readback = _canonical(_resolve(base, flash.get("readbackBinary")))
    same_file = binary == readback
    if binary.is_file() and readback.is_file():
        binary_stat = binary.stat()
        readback_stat = readback.stat()
        same_file = same_file or (
            binary_stat.st_dev == readback_stat.st_dev
            and binary_stat.st_ino == readback_stat.st_ino
        )
    if same_file:
        problems.append("app binary and readback must be distinct files")
    if not readback.is_file():
        problems.append(f"missing app readback binary: {readback}")
    else:
        readback_hash = _sha256(readback)
        if flash.get("readbackSha256") != readback_hash:
            problems.append("app readback binary SHA-256 mismatch")
        if app_hash is not None and readback_hash != app_hash:
            problems.append("app readback bytes do not match app binary")
    if flash.get("readbackSha256") != flash.get("sha256"):
        problems.append("app readback SHA-256 does not match flashed binary")

    readback_args = flash.get("readbackArgs")
    valid_args = False
    if isinstance(readback_args, list) and readback_args.count("read_flash") == 1:
        command_index = readback_args.index("read_flash")
        command = readback_args[command_index : command_index + 4]
        if len(command) == 4 and app_size is not None:
            try:
                requested_size = int(str(command[2]), 0)
            except ValueError:
                requested_size = -1
            output_path = _canonical(_resolve(base, command[3]))
            valid_args = (
                command[1] == "0x20000"
                and requested_size == app_size
                and output_path == readback
            )
    if not valid_args:
        problems.append("readback args must prove read_flash 0x20000 with exact app size")

    transcript = _canonical(_resolve(base, flash.get("readbackTranscript")))
    if not transcript.is_file():
        problems.append(f"missing readback transcript: {transcript}")
    elif app_size is not None:
        transcript_tokens = shlex.split(
            transcript.read_text(encoding="utf-8", errors="replace")
        )
        expected_tokens = ["read_flash", "0x20000", str(app_size), str(readback)]
        if not any(
            transcript_tokens[index : index + 4] == expected_tokens
            for index in range(max(0, len(transcript_tokens) - 3))
        ):
            problems.append("readback transcript must prove exact read_flash range")

    flasher_args = _canonical(_resolve(base, flash.get("flasherArgs")))
    if flasher_args.is_file():
        try:
            app = json.loads(flasher_args.read_text(encoding="utf-8")).get("app")
        except (json.JSONDecodeError, OSError):
            app = None
        if not isinstance(app, dict) or app.get("offset") != "0x20000" or app.get("file") != binary.name:
            problems.append("flasher_args app mapping must be exactly 0x20000 xiaozhi.bin")
        elif app.get("encrypted", "false") != "false":
            problems.append("flasher_args app mapping must be unencrypted")
    else:
        problems.append(f"missing flasher args: {flasher_args}")

    flash_app_args = _canonical(_resolve(base, flash.get("flashAppArgs")))
    if flash_app_args.is_file():
        mappings = re.findall(r"(?m)^\s*(0x[0-9a-fA-F]+)\s+(\S+)\s*$", flash_app_args.read_text())
        if mappings != [("0x20000", binary.name)]:
            problems.append("flash_app_args must contain only 0x20000 xiaozhi.bin")
    else:
        problems.append(f"missing flash app args: {flash_app_args}")
    return _timestamp(flash.get("completedAt"), "flash.completedAt", problems)


def _check_retry(run: dict[str, Any], server_text: str, label: str, problems: list[str]) -> None:
    retry = run.get("retryEvidence")
    if not isinstance(retry, dict):
        problems.append(f"{label}: missing retry evidence")
        return
    markers = {
        "wrong": "interactive child response retry",
        "silence": "child response inactive; reprompt",
    }
    for kind, marker in markers.items():
        evidence = retry.get(kind)
        if not isinstance(evidence, dict):
            problems.append(f"{label}: missing {kind} retry evidence")
            continue
        prompt = str(evidence.get("prompt") or "").strip()
        if evidence.get("gentle") is not True or not prompt or UNSAFE_RETRY.search(prompt):
            problems.append(f"{label}: {kind} retry prompt is not gentle")
        if marker not in server_text:
            problems.append(f"{label}: missing {kind} retry log marker")


def verify(manifest_path: Path) -> list[str]:
    problems: list[str] = []
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot read evidence manifest: {exc}"]
    if not isinstance(manifest, dict):
        return ["evidence manifest must be an object"]
    if manifest.get("schemaVersion") != SCHEMA:
        problems.append(f"schemaVersion must be {SCHEMA}")
    base = manifest_path.resolve().parent
    flash_completed = _check_flash(manifest, base, problems)

    capture = manifest.get("serialCapture")
    if not isinstance(capture, dict):
        problems.append("missing serialCapture evidence")
        capture = {}
    if not str(capture.get("port") or "").startswith("/dev/cu."):
        problems.append("serial capture port must be an explicit /dev/cu.* device")
    if capture.get("baud") != 115200:
        problems.append("serial capture baud must be 115200")
    opened = _timestamp(capture.get("openedAt"), "serialCapture.openedAt", problems)
    closed = _timestamp(capture.get("closedAt"), "serialCapture.closedAt", problems)
    if flash_completed and opened and not flash_completed < opened:
        problems.append("serial capture must open after app flash verification")

    runs = manifest.get("runs")
    if not isinstance(runs, list):
        problems.append("runs must be an array")
        runs = []
    if len(runs) != 5:
        problems.append(f"expected exactly 5 runs, found {len(runs)}")
    previous_end = opened
    seen_ids: set[str] = set()
    seen_logs: set[Path] = set()
    serial_hashes: set[str] = set()
    server_hashes: set[str] = set()
    boot_nonces: set[int] = set()
    for index, raw_run in enumerate(runs, 1):
        label = f"run-{index}"
        if not isinstance(raw_run, dict):
            problems.append(f"{label}: run evidence must be an object")
            continue
        if raw_run.get("id") != label or label in seen_ids:
            problems.append(f"{label}: run IDs must be unique and ordered run-1 through run-5")
        seen_ids.add(str(raw_run.get("id")))
        if raw_run.get("status") != "PASS":
            problems.append(f"{label}: status must be PASS")
        started = _timestamp(raw_run.get("startedAt"), f"{label}.startedAt", problems)
        ended = _timestamp(raw_run.get("endedAt"), f"{label}.endedAt", problems)
        if started and ended and not started < ended:
            problems.append(f"{label}: startedAt must precede endedAt")
        if previous_end and started and not previous_end < started:
            problems.append("runs must be strictly consecutive")
        if ended:
            previous_end = ended

        serial_log_path = _resolve(base, raw_run.get("serialLog"))
        server_log_path = _resolve(base, raw_run.get("serverLog"))
        serial_log = _canonical(serial_log_path)
        server_log = _canonical(server_log_path)
        if serial_log_path != serial_log:
            problems.append(f"{label}: serial log path must be canonical and resolved")
        if server_log_path != server_log:
            problems.append(f"{label}: server log path must be canonical and resolved")
        for evidence_path in (serial_log, server_log):
            if evidence_path in seen_logs:
                problems.append(f"{label}: canonical evidence log paths must be unique")
            seen_logs.add(evidence_path)
        _check_hash(serial_log, raw_run.get("serialSha256"), f"{label} serial log", problems)
        _check_hash(server_log, raw_run.get("serverSha256"), f"{label} server log", problems)
        if serial_log.is_file():
            serial_hashes.add(_sha256(serial_log))
            for problem in verify_cinematic_log(
                serial_log, min_internal_heap=20480, min_psram_heap=1
            ):
                problems.append(f"{label}: {problem}")
            serial_lines = serial_log.read_text(encoding="utf-8", errors="replace").splitlines()
            parsed_lines = parse_evidence_lines(serial_lines, [])
            parsed_boots = [
                (line_no, fields)
                for line_no, _line, fields in parsed_lines
                if fields["event"] == "boot"
            ]
            boot_index = parsed_boots[0][0] - 1 if len(parsed_boots) == 1 else -1
            post_boot = "\n".join(serial_lines[boot_index + 1 :]).lower()
            if any(signature in post_boot for signature in RESET_SIGNATURES):
                problems.append(f"{label}: post-boot reset signature detected")
            if len(parsed_boots) == 1:
                nonce_problems: list[str] = []
                parsed_nonce = parse_boot_nonce(
                    parsed_boots[0][1], parsed_boots[0][0], nonce_problems
                )
                if parsed_nonce is not None:
                    boot_nonces.add(parsed_nonce)
            serial_text = "\n".join(serial_lines)
            if MEMORY_FAILURE.search(serial_text):
                problems.append(f"{label}: memory allocation failure signature detected")
            if RENDERER_DEGRADED.search(serial_text):
                problems.append(f"{label}: renderer fallback/degraded signature detected")
        if server_log.is_file():
            server_hashes.add(_sha256(server_log))
            server_text = server_log.read_text(encoding="utf-8", errors="replace")
            if LIVE_IDENTITY not in server_text:
                problems.append(f"{label}: missing Google Live vi-VN/Kore identity")
            _check_retry(raw_run, server_text, label, problems)
            if MEMORY_FAILURE.search(server_text):
                problems.append(f"{label}: memory allocation failure signature detected")
            if RENDERER_DEGRADED.search(server_text):
                problems.append(f"{label}: renderer fallback/degraded signature detected")
    if len(boot_nonces) != 5 or 0 in boot_nonces:
        problems.append("boot nonces must be distinct and nonzero across all five runs")
    if len(serial_hashes) != 5:
        problems.append("serial log SHA-256 values must be distinct")
    if len(server_hashes) != 5:
        problems.append("server log SHA-256 values must be distinct")
    if serial_hashes & server_hashes:
        problems.append("serial and server log SHA-256 values must not overlap")
    if previous_end and closed and not previous_end < closed:
        problems.append("serial capture must close after the fifth run")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args(argv)
    problems = verify(args.manifest)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1
    print("verified 5 consecutive cinematic release evidence passes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
