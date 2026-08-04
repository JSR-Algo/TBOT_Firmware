import hashlib
import json
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest
from test_lesson_cinematic_hil_log_verifier import CANONICAL_CUES, boot, line

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "lesson_cinematic_hil_evidence_verify.py"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def iso(dt: datetime) -> str:
    return dt.isoformat().replace("+00:00", "Z")


def make_evidence(tmp_path: Path) -> tuple[Path, dict]:
    binary = tmp_path / "xiaozhi.bin"
    binary.write_bytes(b"app-only-image")
    readback = tmp_path / "xiaozhi.readback.bin"
    readback.write_bytes(binary.read_bytes())
    flasher_args = tmp_path / "flasher_args.json"
    flasher_args.write_text(
        json.dumps(
            {"app": {"offset": "0x20000", "file": binary.name, "encrypted": "false"}}
        ),
        encoding="utf-8",
    )
    flash_app_args = tmp_path / "flash_app_args"
    flash_app_args.write_text("--flash_mode dio\n0x20000 xiaozhi.bin\n", encoding="utf-8")
    readback_transcript = tmp_path / "readback.log"
    readback_command = f"read_flash 0x20000 {binary.stat().st_size} {readback}"
    readback_transcript.write_text(f"esptool.py {readback_command}\n", encoding="utf-8")

    base = datetime(2026, 8, 4, 8, 0, tzinfo=timezone.utc)
    digest = sha256(binary)
    runs = []
    for index in range(5):
        serial_log = tmp_path / f"serial-run-{index + 1}.log"
        nonce = f"0x{index + 1:x}"
        serial_text = boot() + "".join(line(cue) for cue in CANONICAL_CUES)
        serial_log.write_text(
            f"capture_run={index + 1}\n" + serial_text.replace("boot_nonce=0x1", f"boot_nonce={nonce}"),
            encoding="utf-8",
        )
        server_log = tmp_path / f"server-run-{index + 1}.log"
        server_log.write_text(
            "Google Live session identity model=gemini-3.1-flash-live-preview "
            "voice=Kore language=vi-VN\n"
            "interactive child response retry stepId=barn intent=wrong expected=['barn']\n"
            f"child response inactive; reprompt stepId=hay run={index + 1}\n",
            encoding="utf-8",
        )
        started = base + timedelta(minutes=index * 2 + 1)
        ended = started + timedelta(minutes=1)
        runs.append(
            {
                "id": f"run-{index + 1}",
                "status": "PASS",
                "startedAt": iso(started),
                "endedAt": iso(ended),
                "serialLog": str(serial_log),
                "serialSha256": sha256(serial_log),
                "serverLog": str(server_log),
                "serverSha256": sha256(server_log),
                "retryEvidence": {
                    "wrong": {"gentle": True, "prompt": "Nice try. Listen once more: barn."},
                    "silence": {"gentle": True, "prompt": "Take your time. Try barn when ready."},
                },
            }
        )
    manifest = {
        "schemaVersion": "lesson-cinematic-hil-evidence.v1",
        "flash": {
            "mode": "app-only",
            "offset": "0x20000",
            "binary": str(binary),
            "sha256": digest,
            "readbackSha256": digest,
            "readbackBinary": str(readback),
            "readbackArgs": [
                "read_flash",
                "0x20000",
                str(binary.stat().st_size),
                str(readback),
            ],
            "readbackTranscript": str(readback_transcript),
            "completedAt": iso(base),
            "flasherArgs": str(flasher_args),
            "flashAppArgs": str(flash_app_args),
        },
        "serialCapture": {
            "port": "/dev/cu.usbmodem1101",
            "baud": 115200,
            "openedAt": iso(base + timedelta(seconds=30)),
            "closedAt": iso(base + timedelta(minutes=11, seconds=30)),
        },
        "runs": runs,
    }
    manifest_path = tmp_path / "evidence.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path, manifest


def run_verifier(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(path)],
        text=True,
        capture_output=True,
        check=False,
    )


def test_evidence_verifier_accepts_five_fresh_ordered_passes(tmp_path):
    path, _manifest = make_evidence(tmp_path)

    result = run_verifier(path)

    assert result.returncode == 0, result.stderr
    assert "verified 5 consecutive cinematic HIL passes" in result.stdout


def test_evidence_verifier_rejects_missing_fifth_pass(tmp_path):
    path, manifest = make_evidence(tmp_path)
    manifest["runs"].pop()
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "expected exactly 5 runs" in result.stderr


def test_evidence_verifier_rejects_wrong_app_offset(tmp_path):
    path, manifest = make_evidence(tmp_path)
    manifest["flash"]["offset"] = "0x10000"
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "flash offset must be 0x20000" in result.stderr


def test_evidence_verifier_rejects_noncanonical_app_binary_name(tmp_path):
    path, manifest = make_evidence(tmp_path)
    binary = Path(manifest["flash"]["binary"])
    renamed = binary.with_name("other.bin")
    binary.rename(renamed)
    manifest["flash"]["binary"] = str(renamed)
    manifest["flash"]["flasherArgs"] = str(Path(manifest["flash"]["flasherArgs"]))
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "app binary must be named xiaozhi.bin" in result.stderr


def test_evidence_verifier_rejects_reused_boot_nonce(tmp_path):
    path, manifest = make_evidence(tmp_path)
    second_log = Path(manifest["runs"][1]["serialLog"])
    second_log.write_text(
        second_log.read_text(encoding="utf-8").replace("boot_nonce=0x2", "boot_nonce=0x1"),
        encoding="utf-8",
    )
    manifest["runs"][1]["serialSha256"] = sha256(second_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "boot nonces must be distinct and nonzero" in result.stderr


def test_evidence_verifier_rejects_symlink_alias_for_log_path(tmp_path):
    path, manifest = make_evidence(tmp_path)
    first_log = Path(manifest["runs"][0]["serialLog"])
    alias = tmp_path / "serial-alias.log"
    alias.symlink_to(first_log)
    manifest["runs"][1]["serialLog"] = str(alias)
    manifest["runs"][1]["serialSha256"] = sha256(alias)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "canonical evidence log paths must be unique" in result.stderr


def test_evidence_verifier_rejects_noncanonical_log_path(tmp_path):
    path, manifest = make_evidence(tmp_path)
    (tmp_path / "unused").mkdir()
    manifest["runs"][0]["serialLog"] = str(
        tmp_path / "unused" / ".." / "serial-run-1.log"
    )
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "serial log path must be canonical and resolved" in result.stderr


def test_evidence_verifier_rejects_copied_serial_log_content(tmp_path):
    path, manifest = make_evidence(tmp_path)
    first_log = Path(manifest["runs"][0]["serialLog"])
    second_log = Path(manifest["runs"][1]["serialLog"])
    second_log.write_bytes(first_log.read_bytes())
    manifest["runs"][1]["serialSha256"] = sha256(second_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "serial log SHA-256 values must be distinct" in result.stderr


def test_evidence_verifier_rejects_copied_server_log_content(tmp_path):
    path, manifest = make_evidence(tmp_path)
    first_log = Path(manifest["runs"][0]["serverLog"])
    second_log = Path(manifest["runs"][1]["serverLog"])
    second_log.write_bytes(first_log.read_bytes())
    manifest["runs"][1]["serverSha256"] = sha256(second_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "server log SHA-256 values must be distinct" in result.stderr


def test_evidence_verifier_rejects_missing_readback_binary(tmp_path):
    path, manifest = make_evidence(tmp_path)
    Path(manifest["flash"]["readbackBinary"]).unlink()

    result = run_verifier(path)

    assert result.returncode != 0
    assert "missing app readback binary" in result.stderr


def test_evidence_verifier_rejects_readback_bytes_that_do_not_match_app(tmp_path):
    path, manifest = make_evidence(tmp_path)
    readback = Path(manifest["flash"]["readbackBinary"])
    readback.write_bytes(b"forged-readback")
    manifest["flash"]["readbackSha256"] = sha256(readback)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "app readback bytes do not match app binary" in result.stderr


def test_evidence_verifier_rejects_read_flash_with_wrong_size(tmp_path):
    path, manifest = make_evidence(tmp_path)
    manifest["flash"]["readbackArgs"][2] = "1"
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "readback args must prove read_flash 0x20000 with exact app size" in result.stderr


def test_evidence_verifier_rejects_readback_transcript_without_exact_range(tmp_path):
    path, manifest = make_evidence(tmp_path)
    transcript = Path(manifest["flash"]["readbackTranscript"])
    transcript.write_text("esptool.py read_flash 0x20000 1 out.bin\n", encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "readback transcript must prove exact read_flash range" in result.stderr


def test_evidence_verifier_rejects_nonconsecutive_timestamps(tmp_path):
    path, manifest = make_evidence(tmp_path)
    manifest["runs"][1]["startedAt"] = manifest["runs"][0]["endedAt"]
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "runs must be strictly consecutive" in result.stderr


def test_evidence_verifier_rejects_post_boot_reset_signature(tmp_path):
    path, manifest = make_evidence(tmp_path)
    serial_log = Path(manifest["runs"][2]["serialLog"])
    serial_log.write_text(serial_log.read_text(encoding="utf-8") + "Guru Meditation Error\n", encoding="utf-8")
    manifest["runs"][2]["serialSha256"] = sha256(serial_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "post-boot reset signature" in result.stderr


@pytest.mark.parametrize(
    "signature",
    ["OOM", "malloc failed", "memory allocation failed"],
)
def test_evidence_verifier_rejects_memory_allocation_failure(tmp_path, signature):
    path, manifest = make_evidence(tmp_path)
    serial_log = Path(manifest["runs"][2]["serialLog"])
    serial_log.write_text(
        serial_log.read_text(encoding="utf-8") + f"{signature}\n",
        encoding="utf-8",
    )
    manifest["runs"][2]["serialSha256"] = sha256(serial_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "memory allocation failure signature" in result.stderr


def test_evidence_verifier_rejects_renderer_degraded_signature(tmp_path):
    path, manifest = make_evidence(tmp_path)
    server_log = Path(manifest["runs"][3]["serverLog"])
    server_log.write_text(
        server_log.read_text(encoding="utf-8") + "renderer degraded=true\n",
        encoding="utf-8",
    )
    manifest["runs"][3]["serverSha256"] = sha256(server_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "renderer fallback/degraded signature" in result.stderr


def test_evidence_verifier_rejects_renderer_fallback_signature(tmp_path):
    path, manifest = make_evidence(tmp_path)
    server_log = Path(manifest["runs"][3]["serverLog"])
    server_log.write_text(
        server_log.read_text(encoding="utf-8") + "renderer fallback=staticGreet\n",
        encoding="utf-8",
    )
    manifest["runs"][3]["serverSha256"] = sha256(server_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "renderer fallback/degraded signature" in result.stderr


@pytest.mark.parametrize("log_kind", ["serial", "server"])
@pytest.mark.parametrize(
    "signature",
    ["fallback=staticGreet", "degraded=true"],
)
def test_evidence_verifier_rejects_standalone_renderer_degradation(
    tmp_path, log_kind, signature
):
    path, manifest = make_evidence(tmp_path)
    run = manifest["runs"][3]
    log_key = f"{log_kind}Log"
    hash_key = f"{log_kind}Sha256"
    log_path = Path(run[log_key])
    log_path.write_text(
        log_path.read_text(encoding="utf-8") + f"{signature}\n",
        encoding="utf-8",
    )
    run[hash_key] = sha256(log_path)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "renderer fallback/degraded signature" in result.stderr


@pytest.mark.parametrize("log_kind", ["serial", "server"])
@pytest.mark.parametrize(
    "signature",
    [
        "malloc returned null",
        "malloc returned nullptr",
        "malloc ENOMEM",
        "frame allocation returned null",
    ],
)
def test_evidence_verifier_rejects_allocation_null_signatures(
    tmp_path, log_kind, signature
):
    path, manifest = make_evidence(tmp_path)
    run = manifest["runs"][2]
    log_key = f"{log_kind}Log"
    hash_key = f"{log_kind}Sha256"
    log_path = Path(run[log_key])
    log_path.write_text(
        log_path.read_text(encoding="utf-8") + f"{signature}\n",
        encoding="utf-8",
    )
    run[hash_key] = sha256(log_path)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "memory allocation failure signature" in result.stderr


def test_evidence_verifier_rejects_missing_google_live_identity(tmp_path):
    path, manifest = make_evidence(tmp_path)
    server_log = Path(manifest["runs"][0]["serverLog"])
    server_log.write_text(server_log.read_text().replace("voice=Kore", "voice=Puck"), encoding="utf-8")
    manifest["runs"][0]["serverSha256"] = sha256(server_log)
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "missing Google Live vi-VN/Kore identity" in result.stderr


def test_evidence_verifier_rejects_unsafe_retry_attestation(tmp_path):
    path, manifest = make_evidence(tmp_path)
    manifest["runs"][4]["retryEvidence"]["wrong"]["prompt"] = "Wrong. Say barn."
    path.write_text(json.dumps(manifest), encoding="utf-8")

    result = run_verifier(path)

    assert result.returncode != 0
    assert "wrong retry prompt is not gentle" in result.stderr
