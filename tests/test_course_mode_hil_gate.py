import base64
import hashlib
import json
import os
import pty
import select
import subprocess
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "scripts" / "run_course_mode_hil_gate.sh"
PROBES = (
    "identity", "protocol", "capability", "tftTestPattern", "sdReadCache",
    "audioDrain", "motionAck", "stopRest", "reconnect", "rebootRecovery",
)
ASSET_PATH = "candidate/background.jpg"
ASSET_SHA256 = "a" * 64


def _command(raw: bytes) -> dict | None:
    if not raw.startswith(b"course_mode_hil "):
        return None
    token = raw.removeprefix(b"course_mode_hil ")
    try:
        decoded = base64.urlsafe_b64decode(token + b"=" * (-len(token) % 4))
        return json.loads(decoded)
    except (ValueError, json.JSONDecodeError):
        return None


def _response(command: dict) -> dict:
    response = {
        "type": "course_mode_hil_evidence", "schemaVersion": 1,
        "nonce": command["nonce"], "probe": command["probe"], "status": "PASS",
    }
    if command["probe"] == "identity":
        response.update(deviceId="91deb5af-c1c0-416b-956d-266d510eac5e",
                        chip="esp32s3", firmwareSha="a" * 64,
                        bootId="0123456789abcdef0123456789abcdef",
                        resetReason="software", bootCount=4)
    elif command["probe"] == "protocol":
        response.update(baud=115200, protocolVersion="teebot-lesson-renderer.v5")
    elif command["probe"] == "capability":
        response.update(lessonRendererV5=True)
    elif command["probe"] == "sdReadCache":
        response.update(bytes=43599, sha256=command["assetSha256"],
                        cacheOutcome="verifiedReadback")
    return response


def _serve_pty(master: int, stop: threading.Event, mutate=None, commands=None,
               reboot_transition=None) -> None:
    pending = b""
    while not stop.is_set():
        ready, _, _ = select.select([master], [], [], 0.1)
        if not ready:
            continue
        try:
            pending += os.read(master, 4096)
        except OSError:
            continue
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            raw = raw.strip()
            command = _command(raw)
            if command is None:
                continue
            if commands is not None:
                commands.append(command)
            if command.get("type") == "course_mode_hil_safety":
                response = {
                    "type": "course_mode_hil_safety_ack", "schemaVersion": 1,
                    "nonce": command["nonce"], "status": "PASS",
                }
                os.write(master, (json.dumps(response, separators=(",", ":")) + "\n").encode())
                continue
            response = _response(command)
            if mutate is not None:
                response = mutate(command, response)
            os.write(master, (json.dumps(response, separators=(",", ":")) + "\n").encode())
            if command.get("probe") == "rebootRecovery" and reboot_transition is not None:
                stable_port, next_tty = reboot_transition
                stable_port.unlink()
                stable_port.symlink_to(next_tty)
                return


def _gate_args(port: str, report: Path, lease_dir: Path, timeout: str = "2") -> list[str]:
    return [
        "bash", str(GATE), "--port", port, "--candidate-id", "course-mode-task7-pty",
        "--expected-firmware-sha", "a" * 64, "--lease-dir", str(lease_dir),
        "--timeout-sec", timeout, "--candidate-asset-relative-path", ASSET_PATH,
        "--candidate-asset-sha256", ASSET_SHA256, "--report", str(report),
    ]


def _run(tmp_path: Path, mutate=None, commands=None, *, transition_on_reboot=True,
         new_boot=True) -> tuple[subprocess.CompletedProcess[str], dict]:
    opened = [pty.openpty(), pty.openpty()]
    masters = [master for master, _ in opened]
    tty_names = [os.ttyname(slave) for _, slave in opened]
    for _, slave in opened:
        os.close(slave)
    stable_port = tmp_path / "course-mode-board"
    stable_port.symlink_to(tty_names[0])
    stop = threading.Event()

    def serve_with_reboot() -> None:
        identity_calls = 0

        def boot_aware_mutate(command: dict, response: dict) -> dict:
            nonlocal identity_calls
            if command.get("probe") == "identity":
                identity_calls += 1
                if identity_calls > 2 and new_boot:
                    response["bootId"] = "fedcba9876543210fedcba9876543210"
                    response["bootCount"] = 5
            if mutate is not None:
                response = mutate(command, response)
            return response

        transition = (stable_port, tty_names[1]) if transition_on_reboot else None
        _serve_pty(masters[0], stop, boot_aware_mutate, commands,
                   reboot_transition=transition)
        if not stop.is_set():
            _serve_pty(masters[1], stop, boot_aware_mutate, commands)

    worker = threading.Thread(target=serve_with_reboot, daemon=True)
    worker.start()
    report = tmp_path / "report.json"
    try:
        result = subprocess.run(
            _gate_args(str(stable_port), report, tmp_path / "leases"),
            cwd=ROOT, text=True, capture_output=True, check=False, timeout=30,
        )
    finally:
        stop.set()
        worker.join(timeout=2)
        for master in masters:
            try: os.close(master)
            except OSError: pass
    return result, json.loads(report.read_text(encoding="utf-8"))


def test_hil_gate_collects_challenge_bound_evidence_from_serial_pty(tmp_path: Path) -> None:
    commands = []
    result, report = _run(tmp_path, commands=commands)
    assert result.returncode == 0, result.stdout + result.stderr
    assert report["verdict"] == "PASS"
    assert report["candidateId"] == "course-mode-task7-pty"
    assert report["board"]["deviceId"] == "91deb5af-c1c0-416b-956d-266d510eac5e"
    assert report["board"]["firmwareSha"] == "a" * 64
    assert report["board"]["bootId"] == "0123456789abcdef0123456789abcdef"
    assert report["board"]["bootCount"] == 4
    assert report["serialProtocol"] == {
        "baud": 115200, "protocolVersion": "teebot-lesson-renderer.v5",
    }
    assert set(report["probes"]) == set(PROBES)
    assert all(item["status"] == "PASS" for item in report["probes"].values())
    assert report["flashingAttempted"] is False
    assert report["exclusiveLease"] is True
    assert report["safetyStopRest"] is True
    motion = next(command for command in commands if command.get("probe") == "motionAck")
    assert motion["safeTestProtocol"] == "course-mode-hil.v1"
    assert motion["maxMotionMs"] == 750 and motion["requireRestAfter"] is True
    sd = next(command for command in commands if command.get("probe") == "sdReadCache")
    assert sd["assetRelativePath"] == ASSET_PATH and sd["assetSha256"] == ASSET_SHA256
    assert commands[-1]["type"] == "course_mode_hil_safety"
    assert commands[-1]["action"] == "stopAndRest"
    assert all("nonce" not in evidence for evidence in report["probes"].values())


def test_hil_gate_rejects_replayed_or_caller_authored_pass_evidence(tmp_path: Path) -> None:
    def stale_nonce(_command: dict, response: dict) -> dict:
        response["nonce"] = "caller-authored-pass"
        return response

    result, report = _run(tmp_path, mutate=stale_nonce)
    assert result.returncode == 1
    assert report["verdict"] == "FAIL"
    assert any("nonce" in failure for failure in report["failures"])

    source = tmp_path / "forged.json"
    source.write_text(json.dumps({"verdict": "PASS"}), encoding="utf-8")
    forged = subprocess.run(
        ["bash", str(GATE), "--input", str(source), "--report", str(tmp_path / "forged-report.json")],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert forged.returncode == 2


def test_hil_gate_fails_without_board_or_exclusive_lease(tmp_path: Path) -> None:
    missing_report = tmp_path / "missing.json"
    missing = subprocess.run(
        ["bash", str(GATE), "--port", str(tmp_path / "not-a-board"),
         "--candidate-id", "candidate", "--expected-firmware-sha", "a" * 64,
         "--lease-dir", str(tmp_path / "leases"), "--timeout-sec", "0.1",
         "--candidate-asset-relative-path", ASSET_PATH,
         "--candidate-asset-sha256", ASSET_SHA256,
         "--report", str(missing_report)],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert missing.returncode == 1
    assert json.loads(missing_report.read_text(encoding="utf-8"))["verdict"] == "FAIL"

    master, slave = pty.openpty()
    port = os.ttyname(slave)
    os.close(slave)
    lease_dir = tmp_path / "busy-leases"
    lease_dir.mkdir()
    (lease_dir / (hashlib.sha256(port.encode()).hexdigest() + ".lock")).write_text(
        "owned-by-another-task\n", encoding="utf-8")
    busy_report = tmp_path / "busy.json"
    try:
        busy = subprocess.run(
            ["bash", str(GATE), "--port", port, "--candidate-id", "candidate",
             "--expected-firmware-sha", "a" * 64, "--lease-dir", str(lease_dir),
             "--timeout-sec", "0.1", "--candidate-asset-relative-path", ASSET_PATH,
             "--candidate-asset-sha256", ASSET_SHA256, "--report", str(busy_report)],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
    finally:
        os.close(master)
    assert busy.returncode == 1
    report = json.loads(busy_report.read_text(encoding="utf-8"))
    assert report["exclusiveLease"] is False
    assert "exclusiveLease" in report["failures"]


def test_hil_gate_is_non_flashing_and_uses_serial_collection() -> None:
    source = GATE.read_text(encoding="utf-8")
    collector = (ROOT / "scripts" / "course_mode_hil_gate.py").read_text(encoding="utf-8")
    combined = source + collector
    for forbidden in ("esptool", "write_flash", "idf.py flash"):
        assert forbidden not in combined
    assert "--input" not in source
    assert "TIOCEXCL" in collector
    assert "course_mode_hil_probe" in collector
    assert '"course_mode_hil "' in collector


def test_firmware_tft_probe_renders_real_rgb_test_bars() -> None:
    application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
    body = application.split("bool Application::RunCourseModeHilTftPattern()", 1)[1]
    body = body.split("bool Application::RunCourseModeHilSdRead()", 1)[0]
    assert "lv_screen_active()" in body
    assert "0xFF0000" in body
    assert "0x00FF00" in body
    assert "0x0000FF" in body


def test_default_firmware_artifact_excludes_all_hil_diagnostic_sources() -> None:
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    prefix, gated = cmake.split("if(CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS)", 1)
    gated = gated.split("endif()", 1)[0]
    assert "course_mode_hil_diagnostic.cc" not in prefix
    assert "course_mode_hil_console.cc" not in prefix
    assert "course_mode_hil_sd_reader.cc" not in prefix
    assert "course_mode_hil_diagnostic.cc" in gated
    assert "course_mode_hil_console.cc" in gated
    assert "course_mode_hil_sd_reader.cc" in gated


def test_hil_gate_revalidates_identity_and_capability_after_reconnect_and_reboot(
    tmp_path: Path,
) -> None:
    commands = []
    result, report = _run(tmp_path, commands=commands)
    assert result.returncode == 0, result.stdout + result.stderr
    probes = [command.get("probe") for command in commands]
    assert probes.count("identity") == 3
    assert probes.count("capability") == 3
    assert report["probes"]["reconnect"]["status"] == "PASS"
    assert report["probes"]["rebootRecovery"]["status"] == "PASS"


def test_hil_gate_reopens_a_reenumerated_stable_tty_path(tmp_path: Path) -> None:
    opened = [pty.openpty() for _ in range(3)]
    masters = [master for master, _ in opened]
    tty_names = [os.ttyname(slave) for _, slave in opened]
    for _, slave in opened:
        os.close(slave)
    stable_port = tmp_path / "course-mode-board"
    stable_port.symlink_to(tty_names[0])
    commands = []
    stop = threading.Event()

    def serve_reenumerations() -> None:
        index = 0
        pending = b""
        while not stop.is_set() and index < len(masters):
            master = masters[index]
            ready, _, _ = select.select([master], [], [], 0.1)
            if not ready:
                continue
            try:
                pending += os.read(master, 4096)
            except OSError:
                continue
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                raw = raw.strip()
                command = _command(raw)
                if command is None:
                    continue
                commands.append(command)
                if command.get("type") == "course_mode_hil_safety":
                    response = {
                        "type": "course_mode_hil_safety_ack", "schemaVersion": 1,
                        "nonce": command["nonce"], "status": "PASS",
                    }
                else:
                    response = _response(command)
                    if command.get("probe") == "identity" and index == 2:
                        response["bootId"] = "fedcba9876543210fedcba9876543210"
                        response["bootCount"] = 5
                transition = command.get("probe") in {"reconnect", "rebootRecovery"}
                if transition:
                    stable_port.unlink()
                    stable_port.symlink_to(tty_names[index + 1])
                os.write(master, (json.dumps(response, separators=(",", ":")) + "\n").encode())
                if transition:
                    index += 1
                    pending = b""
                    break

    worker = threading.Thread(target=serve_reenumerations, daemon=True)
    worker.start()
    report_path = tmp_path / "reenumerated.json"
    try:
        result = subprocess.run(
            _gate_args(str(stable_port), report_path, tmp_path / "leases", "1"),
            cwd=ROOT, text=True, capture_output=True, check=False, timeout=30,
        )
    finally:
        stop.set()
        worker.join(timeout=2)
        for master in masters:
            try: os.close(master)
            except OSError: pass
    assert result.returncode == 0, result.stdout + result.stderr
    probes = [command.get("probe") for command in commands]
    assert probes.count("identity") == 3
    assert probes.count("capability") == 3


def test_hil_gate_rejects_same_firmware_board_swap_after_reconnect(tmp_path: Path) -> None:
    def swapped_identity(command: dict, response: dict) -> dict:
        if command.get("probe") == "identity":
            swapped_identity.calls += 1
            if swapped_identity.calls > 1:
                response["deviceId"] = "82deb5af-c1c0-416b-956d-266d510eac5e"
        return response

    swapped_identity.calls = 0
    result, report = _run(tmp_path, mutate=swapped_identity)
    assert result.returncode == 1
    assert report["verdict"] == "FAIL"
    assert "reconnect.deviceId" in report["failures"]


def test_hil_gate_rejects_reboot_ack_without_tty_reenumeration(tmp_path: Path) -> None:
    result, report = _run(tmp_path, transition_on_reboot=False)
    assert result.returncode == 1
    assert report["verdict"] == "FAIL"
    assert "rebootRecovery.portTransition" in report["failures"]


def test_hil_gate_rejects_reenumeration_without_new_boot_identity(tmp_path: Path) -> None:
    result, report = _run(tmp_path, new_boot=False)
    assert result.returncode == 1
    assert report["verdict"] == "FAIL"
    assert "rebootRecovery.bootId" in report["failures"]


def test_hil_gate_fails_closed_when_reboot_loses_the_only_safety_channel(
    tmp_path: Path,
) -> None:
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    os.close(slave)
    stop = threading.Event()

    def disconnect_on_reboot() -> None:
        pending = b""
        while not stop.is_set():
            ready, _, _ = select.select([master], [], [], 0.1)
            if not ready:
                continue
            try:
                pending += os.read(master, 4096)
            except OSError:
                return
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                raw = raw.strip()
                command = _command(raw)
                if command is None:
                    continue
                response = _response(command)
                os.write(master, (json.dumps(response, separators=(",", ":")) + "\n").encode())
                if command.get("probe") == "rebootRecovery":
                    os.close(master)
                    return

    worker = threading.Thread(target=disconnect_on_reboot, daemon=True)
    worker.start()
    report_path = tmp_path / "lost-channel.json"
    try:
        result = subprocess.run(
            _gate_args(port, report_path, tmp_path / "leases", "0.2"),
            cwd=ROOT, text=True, capture_output=True, check=False, timeout=30,
        )
    finally:
        stop.set()
        worker.join(timeout=2)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert result.returncode == 1
    assert report["verdict"] == "FAIL"
    assert report["safetyStopRest"] is False
    assert "safetyStopUnconfirmed" in report["failures"]
