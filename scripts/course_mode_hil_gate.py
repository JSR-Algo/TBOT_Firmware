#!/usr/bin/env python3
"""Collect non-flashing renderer-v5 HIL evidence directly from a serial board."""

from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import json
import math
import os
import re
import secrets
import select
import termios
import time
from pathlib import Path


PROBES = (
    "identity", "protocol", "capability", "tftTestPattern", "sdReadCache",
    "audioDrain", "motionAck", "stopRest", "reconnect", "rebootRecovery",
)
SHA256 = re.compile(r"[0-9a-f]{64}")
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")
SAFE_ASSET_COMPONENT = re.compile(r"[A-Za-z0-9_.-]+")
MAX_DECODED_COMMAND_BYTES = 2048
MAX_ENCODED_COMMAND_BYTES = 2731


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--expected-firmware-sha", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--lease-dir", required=True)
    parser.add_argument("--timeout-sec", type=float, default=5.0)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--candidate-asset-relative-path", required=True)
    parser.add_argument("--candidate-asset-sha256", required=True)
    return parser.parse_args()


def atomic_report(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise RuntimeError("report destination must not be a symlink")
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(fd, payload)
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(temporary, path)


def claim_lease(port: str, directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    lease = directory / f"{hashlib.sha256(port.encode()).hexdigest()}.lock"
    fd = os.open(lease, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(fd, f"pid={os.getpid()} port={port}\n".encode())
        os.fsync(fd)
    finally:
        os.close(fd)
    return lease


def open_serial(port: str, baud: int) -> int:
    if baud != 115200:
        raise RuntimeError("only 115200 baud is supported")
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, termios.TIOCEXCL)
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)
        return fd
    except Exception:
        os.close(fd)
        raise


def encode_command(command: dict) -> bytes:
    payload = json.dumps(command, separators=(",", ":"), ensure_ascii=True).encode()
    if not payload or len(payload) > MAX_DECODED_COMMAND_BYTES:
        raise RuntimeError("commandSize")
    token = base64.urlsafe_b64encode(payload).rstrip(b"=")
    if len(token) > MAX_ENCODED_COMMAND_BYTES:
        raise RuntimeError("commandEncodingSize")
    return b"course_mode_hil " + token + b"\n"


def reopen_serial(port: str, baud: int, timeout: float) -> int:
    deadline = time.monotonic() + timeout
    last_error: OSError | RuntimeError | None = None
    while time.monotonic() < deadline:
        try:
            return open_serial(port, baud)
        except (OSError, RuntimeError) as error:
            last_error = error
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(f"serialReopen:{last_error or 'timeout'}")


def tty_identity(port: str) -> tuple[int, int, int]:
    value = os.stat(port)
    return value.st_dev, value.st_ino, value.st_rdev


def safe_asset_path(value: str) -> bool:
    components = value.split("/")
    return bool(value) and len(value) <= 256 and all(
        component not in {"", ".", ".."} and SAFE_ASSET_COMPONENT.fullmatch(component)
        for component in components
    )


def wait_for_port_transition(
    port: str, original: tuple[int, int, int], timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            current = tty_identity(port)
        except FileNotFoundError:
            pass
        except OSError:
            pass
        else:
            if current != original:
                return
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
    raise RuntimeError("rebootRecovery.portTransition")


def collect_probe(
    fd: int, probe: str, timeout: float, asset_path: str = "", asset_sha256: str = "",
) -> dict:
    nonce = secrets.token_hex(16)
    command = {
        "type": "course_mode_hil_probe", "schemaVersion": 1,
        "probe": probe, "nonce": nonce, "flashingAllowed": False,
        "safeTestProtocol": "course-mode-hil.v1",
    }
    if probe == "motionAck":
        command.update(maxMotionMs=750, requireRestAfter=True)
    if probe == "rebootRecovery":
        command["action"] = "rebootAndRecover"
    if probe == "sdReadCache":
        command.update(assetRelativePath=asset_path, assetSha256=asset_sha256)
    os.write(fd, encode_command(command))
    deadline = time.monotonic() + timeout
    pending = b""
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], min(0.1, deadline - time.monotonic()))
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            continue
        pending += chunk
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            try:
                response = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if response.get("type") != "course_mode_hil_evidence" or response.get("probe") != probe:
                continue
            if response.get("nonce") != nonce:
                raise RuntimeError(f"probes.{probe}.nonce")
            if response.get("schemaVersion") != 1:
                raise RuntimeError(f"probes.{probe}.schemaVersion")
            return response
    raise RuntimeError(f"probes.{probe}.timeout")


def force_stop_and_rest(fd: int, timeout: float) -> bool:
    nonce = secrets.token_hex(16)
    command = {
        "type": "course_mode_hil_safety", "schemaVersion": 1,
        "action": "stopAndRest", "nonce": nonce, "flashingAllowed": False,
        "safeTestProtocol": "course-mode-hil.v1",
    }
    try:
        os.write(fd, encode_command(command))
        deadline = time.monotonic() + min(timeout, 1.0)
        pending = b""
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], min(0.1, deadline - time.monotonic()))
            if not ready:
                continue
            pending += os.read(fd, 4096)
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                try:
                    response = json.loads(raw)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if response.get("type") == "course_mode_hil_safety_ack":
                    return response.get("nonce") == nonce and response.get("status") == "PASS"
    except OSError:
        return False
    return False


def sanitized_probe(probe: str, response: dict) -> dict:
    result = {"status": response.get("status")}
    allowed = {
        "identity": ("deviceId", "chip", "firmwareSha"),
        "protocol": ("baud", "protocolVersion"),
        "capability": ("lessonRendererV5",),
        "sdReadCache": ("bytes", "sha256", "cacheOutcome"),
    }.get(probe, ())
    if probe == "identity":
        allowed = (*allowed, "bootId", "resetReason", "bootCount")
    for key in allowed:
        result[key] = response.get(key)
    return result


def validate(args: argparse.Namespace, evidence: dict[str, dict]) -> list[str]:
    failures = []
    for probe in PROBES:
        if evidence.get(probe, {}).get("status") != "PASS":
            failures.append(f"probes.{probe}.status")
    identity = evidence.get("identity", {})
    protocol = evidence.get("protocol", {})
    capability = evidence.get("capability", {})
    if not UUID.fullmatch(str(identity.get("deviceId", ""))): failures.append("board.deviceId")
    if identity.get("chip") != "esp32s3": failures.append("board.chip")
    if identity.get("firmwareSha") != args.expected_firmware_sha: failures.append("board.firmwareSha")
    if not re.fullmatch(r"[0-9a-f]{32}", str(identity.get("bootId", ""))): failures.append("board.bootId")
    if not isinstance(identity.get("bootCount"), int) or identity.get("bootCount", 0) <= 0:
        failures.append("board.bootCount")
    if not isinstance(identity.get("resetReason"), str) or not identity.get("resetReason"):
        failures.append("board.resetReason")
    if protocol.get("baud") != args.baud: failures.append("serialProtocol.baud")
    if protocol.get("protocolVersion") != "teebot-lesson-renderer.v5": failures.append("serialProtocol.protocolVersion")
    if capability.get("lessonRendererV5") is not True: failures.append("board.lessonRendererV5")
    sd = evidence.get("sdReadCache", {})
    if sd.get("sha256") != args.candidate_asset_sha256: failures.append("sdReadCache.sha256")
    if sd.get("cacheOutcome") != "verifiedReadback": failures.append("sdReadCache.cacheOutcome")
    if not isinstance(sd.get("bytes"), int) or sd.get("bytes", 0) <= 0:
        failures.append("sdReadCache.bytes")
    return failures


def revalidate_recovered_board(
    fd: int, args: argparse.Namespace, label: str, expected_identity: dict,
    require_new_boot: bool,
) -> None:
    identity = sanitized_probe("identity", collect_probe(fd, "identity", args.timeout_sec))
    capability = sanitized_probe(
        "capability", collect_probe(fd, "capability", args.timeout_sec))
    if identity.get("status") != "PASS" or capability.get("status") != "PASS":
        raise RuntimeError(f"{label}.status")
    if identity.get("deviceId") != expected_identity.get("deviceId"):
        raise RuntimeError(f"{label}.deviceId")
    if identity.get("chip") != expected_identity.get("chip"):
        raise RuntimeError(f"{label}.chip")
    if identity.get("firmwareSha") != expected_identity.get("firmwareSha") or \
            identity.get("firmwareSha") != args.expected_firmware_sha:
        raise RuntimeError(f"{label}.firmwareSha")
    if identity.get("chip") != "esp32s3" or not UUID.fullmatch(
        str(identity.get("deviceId", ""))
    ):
        raise RuntimeError(f"{label}.identity")
    if not re.fullmatch(r"[0-9a-f]{32}", str(identity.get("bootId", ""))):
        raise RuntimeError(f"{label}.bootId")
    if not isinstance(identity.get("bootCount"), int) or identity.get("bootCount", 0) <= 0:
        raise RuntimeError(f"{label}.bootCount")
    if not isinstance(identity.get("resetReason"), str) or not identity.get("resetReason"):
        raise RuntimeError(f"{label}.resetReason")
    if capability.get("lessonRendererV5") is not True:
        raise RuntimeError(f"{label}.capability")
    if require_new_boot:
        if identity.get("bootId") == expected_identity.get("bootId"):
            raise RuntimeError(f"{label}.bootId")
        if not isinstance(identity.get("bootCount"), int) or \
                identity.get("bootCount", 0) <= expected_identity.get("bootCount", 0):
            raise RuntimeError(f"{label}.bootCount")
    elif identity.get("bootId") != expected_identity.get("bootId") or \
            identity.get("bootCount") != expected_identity.get("bootCount"):
        raise RuntimeError(f"{label}.unexpectedBoot")


def main() -> int:
    args = arguments()
    failures: list[str] = []
    evidence: dict[str, dict] = {}
    lease: Path | None = None
    fd: int | None = None
    exclusive = False
    safety_stop = False
    if not args.candidate_id.strip(): failures.append("candidateId")
    if not SHA256.fullmatch(args.expected_firmware_sha): failures.append("expectedFirmwareSha")
    if not math.isfinite(args.timeout_sec) or args.timeout_sec <= 0: failures.append("timeoutSec")
    if not safe_asset_path(args.candidate_asset_relative_path):
        failures.append("candidateAssetRelativePath")
    if not SHA256.fullmatch(args.candidate_asset_sha256):
        failures.append("candidateAssetSha256")
    try:
        lease = claim_lease(args.port, Path(args.lease_dir).resolve())
        fd = open_serial(args.port, args.baud)
        exclusive = True
        initial_identity: dict = {}
        for probe in PROBES:
            original_tty = tty_identity(args.port) if probe == "rebootRecovery" else None
            evidence[probe] = sanitized_probe(probe, collect_probe(
                fd, probe, args.timeout_sec, args.candidate_asset_relative_path,
                args.candidate_asset_sha256))
            if probe == "identity":
                initial_identity = dict(evidence[probe])
            if probe in {"reconnect", "rebootRecovery"}:
                if initial_identity.get("status") != "PASS":
                    raise RuntimeError("initialIdentityUnavailable")
                os.close(fd)
                fd = None
                if probe == "rebootRecovery":
                    if original_tty is None:
                        raise RuntimeError("rebootRecovery.portIdentity")
                    wait_for_port_transition(args.port, original_tty, args.timeout_sec)
                fd = reopen_serial(args.port, args.baud, args.timeout_sec)
                revalidate_recovered_board(
                    fd, args, probe, initial_identity, probe == "rebootRecovery")
    except FileExistsError:
        failures.append("exclusiveLease")
    except (OSError, RuntimeError) as error:
        failures.append(str(error) or error.__class__.__name__)
    finally:
        if exclusive and fd is None:
            try:
                fd = reopen_serial(args.port, args.baud, args.timeout_sec)
            except (OSError, RuntimeError):
                fd = None
        if fd is not None:
            safety_stop = force_stop_and_rest(fd, args.timeout_sec)
            try:
                os.close(fd)
            except OSError:
                pass
            fd = None
        if lease is not None:
            try: lease.unlink()
            except FileNotFoundError: pass
    failures.extend(validate(args, evidence))
    if exclusive and not safety_stop: failures.append("safetyStopUnconfirmed")
    identity = evidence.get("identity", {})
    protocol = evidence.get("protocol", {})
    report = {
        "schemaVersion": 1, "gate": "course-mode-renderer-v5-hil",
        "candidateId": args.candidate_id, "verdict": "FAIL" if failures else "PASS",
        "exclusiveLease": exclusive, "flashingAttempted": False,
        "safetyStopRest": safety_stop,
        "board": {"deviceId": identity.get("deviceId"), "chip": identity.get("chip"),
                  "firmwareSha": identity.get("firmwareSha"),
                  "bootId": identity.get("bootId"),
                  "resetReason": identity.get("resetReason"),
                  "bootCount": identity.get("bootCount"),
                  "lessonRendererV5": evidence.get("capability", {}).get("lessonRendererV5")},
        "serialProtocol": {"baud": protocol.get("baud"),
                           "protocolVersion": protocol.get("protocolVersion")},
        "probes": evidence, "failures": list(dict.fromkeys(failures)),
    }
    atomic_report(Path(args.report).resolve(), report)
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
