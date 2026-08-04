import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "lesson_cinematic_hil_log_verify.py"


CANONICAL_CUES = [
    "barn-opening",
    "barn-greet",
    "barn-teach",
    "barn-listen",
    "barn-thinking",
    "barn-correct",
    "barn-retry-level-1",
    "barn-retry-level-2",
    "barn-retry-level-3",
    "barn-celebrate",
    "barn-to-hay-word-transition",
    "hay-teach",
    "hay-listen",
    "hay-thinking",
    "hay-correct",
    "hay-retry-level-1",
    "hay-retry-level-2",
    "hay-retry-level-3",
    "hay-celebrate",
]

LOOP_CUES = {
    "barn-greet",
    "barn-listen",
    "barn-thinking",
    "hay-listen",
    "hay-thinking",
}


def boot(*, internal_heap_min: int = 60000) -> str:
    return (
        "HIL_CINE event=boot boot_nonce=0x1 reset_reason=poweron "
        f"lifetime_internal_heap_min={internal_heap_min} psram_heap_min=4200000\n"
    )


def line(
    cue: str,
    *,
    reason: str = "natural",
    fault: str = "none",
    internal_heap_min: int = 60000,
    lifetime_internal_heap_min: int = 60000,
    read_count: int = 1,
    read_ge70ms: int = 0,
    read_max_ms: int = 10,
    read_hist_ms: str = "0:1,70:0,100:0",
) -> str:
    return (
        "HIL_CINE event=cue_end "
        f"cue={cue} reason={reason} fault={fault} seq=1 latency_ms=100 "
        f"read_count={read_count} read_ge70ms={read_ge70ms} read_max_ms={read_max_ms} "
        f"read_hist_ms={read_hist_ms} "
        "panel_latency_ms=12 queue_errors=0 queue_timeouts=0 dma_errors=0 "
        "parser_errors=0 header_crc_errors=0 frame_crc_errors=0 io_errors=0 "
        "watchdog_faults=0 unexpected_reset_faults=0 "
        f"late_ticks=0 missed_periods=0 internal_heap_min={internal_heap_min} "
        f"lifetime_internal_heap_min={lifetime_internal_heap_min} psram_heap_min=4200000 "
        "boot_nonce=0x1 reset_reason=poweron cue_end\n"
    )


def run_verifier(log_path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(log_path)],
        text=True,
        capture_output=True,
        check=False,
    )


def test_verifier_accepts_fault_free_loop_cues_ended_by_replacement_or_stop(tmp_path):
    log_path = tmp_path / "loop_replaced.log"
    reasons = {
        "barn-greet": "replacement",
        "barn-listen": "stop",
        "barn-thinking": "replacement",
        "hay-listen": "stop",
        "hay-thinking": "replacement",
    }
    log_path.write_text(
        boot() + "".join(line(cue, reason=reasons.get(cue, "natural")) for cue in CANONICAL_CUES),
        encoding="utf-8",
    )

    result = run_verifier(log_path)

    assert result.returncode == 0, result.stderr
    assert "verified 19 cue_end lines" in result.stdout


def test_verifier_rejects_canonical_cues_out_of_order(tmp_path):
    log_path = tmp_path / "out_of_order.log"
    cues = list(CANONICAL_CUES)
    cues[5], cues[6] = cues[6], cues[5]
    log_path.write_text(boot() + "".join(line(cue) for cue in cues), encoding="utf-8")

    result = run_verifier(log_path)

    assert result.returncode != 0
    assert "cue_end order does not match canonical sequence" in result.stderr


def test_verifier_rejects_replacement_reason_for_non_loop_cue(tmp_path):
    log_path = tmp_path / "non_loop_replaced.log"
    log_path.write_text(
        boot() + "".join(
            line(cue, reason="replacement" if cue == "barn-opening" else "natural")
            for cue in CANONICAL_CUES
        ),
        encoding="utf-8",
    )

    result = run_verifier(log_path)

    assert result.returncode != 0
    assert "unexpected terminal reason" in result.stderr


def test_verifier_rejects_faulted_loop_cue_even_when_replaced(tmp_path):
    log_path = tmp_path / "faulted_loop_replaced.log"
    log_path.write_text(
        boot() + "".join(
            line(cue, reason="replacement", fault="parser") if cue == "barn-greet" else line(cue)
            for cue in CANONICAL_CUES
        ),
        encoding="utf-8",
    )

    result = run_verifier(log_path)

    assert result.returncode != 0
    assert "fault must be 'none'" in result.stderr


def test_verifier_gates_cue_scoped_heap_not_lifetime_boot_sync_minimum(tmp_path):
    log_path = tmp_path / "cue_scoped_heap.log"
    log_path.write_text(
        boot(internal_heap_min=12000)
        + "".join(
            line(cue, internal_heap_min=20480, lifetime_internal_heap_min=12000)
            for cue in CANONICAL_CUES
        ),
        encoding="utf-8",
    )

    result = run_verifier(log_path)

    assert result.returncode == 0, result.stderr
    assert "verified 19 cue_end lines" in result.stdout


def test_verifier_keeps_20480_default_gate_for_each_cue_scoped_heap_minimum(tmp_path):
    log_path = tmp_path / "cue_heap_below_default.log"
    log_path.write_text(
        boot(internal_heap_min=12000)
        + "".join(
            line(cue, internal_heap_min=20479, lifetime_internal_heap_min=12000)
            if cue == "barn-opening"
            else line(cue, internal_heap_min=20480, lifetime_internal_heap_min=12000)
            for cue in CANONICAL_CUES
        ),
        encoding="utf-8",
    )

    result = run_verifier(log_path)

    assert result.returncode != 0
    assert "internal_heap_min=20479 below minimum 20480" in result.stderr


def test_verifier_rejects_duplicate_100ms_histogram_bucket(tmp_path):
    log_path = tmp_path / "duplicate_100ms_bucket.log"
    log_path.write_text(
        boot()
        + "".join(
            line(
                cue,
                read_count=1,
                read_ge70ms=1,
                read_max_ms=100,
                read_hist_ms="0:0,70:0,100:1,100:1",
            )
            if cue == "barn-opening"
            else line(cue)
            for cue in CANONICAL_CUES
        ),
        encoding="utf-8",
    )

    result = run_verifier(log_path)

    assert result.returncode != 0
    assert "duplicate read_hist_ms bucket=100" in result.stderr
