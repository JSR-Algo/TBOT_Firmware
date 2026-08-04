#!/usr/bin/env python3
"""Verify firmware production cinematic evidence logs."""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

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

ALLOWED_TERMINAL_REASONS = {"natural"}
ALLOWED_LOOP_TERMINAL_REASONS = {"natural", "replacement", "stop"}

FAULT_FIELDS = [
    "queue_errors",
    "parser_errors",
    "header_crc_errors",
    "frame_crc_errors",
    "io_errors",
    "dma_errors",
    "queue_timeouts",
    "watchdog_faults",
    "unexpected_reset_faults",
    "late_ticks",
    "missed_periods",
]

ALLOWED_RESET_REASONS = {"poweron", "software", "external", "deepsleep"}
NUMERIC_FIELDS = [
    "seq",
    "latency_ms",
    "read_count",
    "read_ge70ms",
    "read_max_ms",
    "panel_latency_ms",
    "queue_errors",
    "queue_timeouts",
    "dma_errors",
    "parser_errors",
    "header_crc_errors",
    "frame_crc_errors",
    "io_errors",
    "watchdog_faults",
    "unexpected_reset_faults",
    "late_ticks",
    "missed_periods",
    "internal_heap_min",
    "lifetime_internal_heap_min",
    "psram_heap_min",
]


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def parse_uint(fields: dict[str, str], field: str, line_no: int, problems: list[str]) -> int | None:
    value = fields.get(field)
    if value is None:
        problems.append(f"line {line_no}: missing {field}")
        return None
    if not value.isdecimal():
        problems.append(f"line {line_no}: malformed {field}={value!r}")
        return None
    return int(value, 10)


def parse_boot_nonce(fields: dict[str, str], line_no: int, problems: list[str]) -> int | None:
    value = fields.get("boot_nonce")
    if value is None:
        problems.append(f"line {line_no}: missing boot_nonce")
        return None
    if not value.startswith("0x"):
        problems.append(f"line {line_no}: malformed boot_nonce={value!r}")
        return None
    try:
        parsed = int(value, 0)
    except ValueError:
        problems.append(f"line {line_no}: malformed boot_nonce={value!r}")
        return None
    if parsed == 0:
        problems.append(f"line {line_no}: zero boot_nonce")
        return None
    return parsed


def parse_histogram(fields: dict[str, str], line_no: int, problems: list[str]) -> dict[int, int] | None:
    value = fields.get("read_hist_ms")
    if value is None:
        problems.append(f"line {line_no}: missing read_hist_ms")
        return None
    buckets: dict[int, int] = {}
    for token in value.split(","):
        if ":" not in token:
            problems.append(f"line {line_no}: malformed read_hist_ms={value!r}")
            return None
        key, count = token.split(":", 1)
        if not key.isdecimal() or not count.isdecimal():
            problems.append(f"line {line_no}: malformed read_hist_ms={value!r}")
            return None
        bucket = int(key, 10)
        if bucket in buckets:
            problems.append(f"line {line_no}: duplicate read_hist_ms bucket={bucket}")
            return None
        buckets[bucket] = int(count, 10)
    if set(buckets) != {0, 70, 100}:
        problems.append(f"line {line_no}: malformed read_hist_ms buckets={value!r}")
        return None
    return buckets


def verify(path: Path, *, min_internal_heap: int, min_psram_heap: int) -> list[str]:
    problems: list[str] = []
    counts = {cue: 0 for cue in CANONICAL_CUES}
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    cue_end_lines = [
        (line_no, line)
        for line_no, line in enumerate(lines, 1)
        if line.startswith("CINE_EVIDENCE ") and " event=cue_end " in f" {line} "
    ]
    cue_order = [parse_fields(line).get("cue") for _line_no, line in cue_end_lines]
    if cue_order != CANONICAL_CUES:
        problems.append("cue_end order does not match canonical sequence")
    boot_lines = [
        (line_no, parse_fields(line))
        for line_no, line in enumerate(lines, 1)
        if line.startswith("CINE_EVIDENCE ") and " event=boot " in f" {line} "
    ]
    if len(boot_lines) != 1:
        problems.append(
            f"expected exactly one CINE_EVIDENCE boot, found {len(boot_lines)}"
        )
    if not boot_lines:
        problems.append("missing CINE_EVIDENCE boot")
    boot_nonces: set[int] = set()
    for line_no, fields in boot_lines:
        parsed_nonce = parse_boot_nonce(fields, line_no, problems)
        if parsed_nonce is not None:
            boot_nonces.add(parsed_nonce)
        if fields.get("reset_reason") not in ALLOWED_RESET_REASONS:
            problems.append(f"line {line_no}: unexpected reset_reason={fields.get('reset_reason')}")
        parse_uint(fields, "lifetime_internal_heap_min", line_no, problems)
        parse_uint(fields, "psram_heap_min", line_no, problems)
    if len(boot_nonces) != 1:
        problems.append(f"expected one nonzero boot_nonce in boot lines, found {sorted(boot_nonces)}")
    boot_nonce = next(iter(boot_nonces), None)
    for line_no, line in cue_end_lines:
        if not line.endswith("cue_end"):
            problems.append(f"line {line_no}: cue_end line missing terminal cue_end token")
        fields = parse_fields(line)
        cue = fields.get("cue")
        if cue not in counts:
            problems.append(f"line {line_no}: unexpected cue {cue!r}")
            continue
        counts[cue] += 1
        fault = fields.get("fault")
        if fault != "none":
            problems.append(f"line {line_no}: fault must be 'none', got {fault!r}")
        reason = fields.get("reason")
        allowed_reasons = ALLOWED_LOOP_TERMINAL_REASONS if cue in LOOP_CUES else ALLOWED_TERMINAL_REASONS
        if reason not in allowed_reasons:
            problems.append(
                f"line {line_no}: unexpected terminal reason {reason!r} for {cue}; "
                f"expected one of {sorted(allowed_reasons)}"
            )
        parsed_nonce = parse_boot_nonce(fields, line_no, problems)
        if parsed_nonce is not None and boot_nonce is not None and parsed_nonce != boot_nonce:
            problems.append(f"line {line_no}: mixed boot_nonce 0x{parsed_nonce:x} != 0x{boot_nonce:x}")
        parsed_numbers: dict[str, int] = {}
        for field in NUMERIC_FIELDS:
            parsed = parse_uint(fields, field, line_no, problems)
            if parsed is not None:
                parsed_numbers[field] = parsed
        histogram = parse_histogram(fields, line_no, problems)
        for field in FAULT_FIELDS:
            if parsed_numbers.get(field) != 0:
                problems.append(f"line {line_no}: {field}={fields.get(field)}")
        read_count = parsed_numbers.get("read_count")
        read_ge70ms = parsed_numbers.get("read_ge70ms")
        read_max_ms = parsed_numbers.get("read_max_ms")
        if read_count is not None and read_count <= 0:
            problems.append(f"line {line_no}: read_count must be >0")
        if histogram is not None and read_count is not None and read_ge70ms is not None:
            if sum(histogram.values()) != read_count:
                problems.append(f"line {line_no}: read_hist_ms total does not match read_count")
            if histogram[70] + histogram[100] != read_ge70ms:
                problems.append(f"line {line_no}: read_ge70ms does not match read_hist_ms")
            if read_max_ms is not None:
                slow_bucket_count = histogram[70] + histogram[100]
                if slow_bucket_count == 0 and read_max_ms >= 70:
                    problems.append(f"line {line_no}: read_max_ms={read_max_ms} conflicts with all-fast histogram")
                if histogram[100] == 0 and read_max_ms >= 100:
                    problems.append(f"line {line_no}: read_max_ms={read_max_ms} conflicts with empty 100ms histogram bucket")
                if slow_bucket_count > 0 and read_max_ms < 70:
                    problems.append(f"line {line_no}: read_max_ms={read_max_ms} is below populated slow histogram bucket")
                if histogram[100] > 0 and read_max_ms < 100:
                    problems.append(f"line {line_no}: read_max_ms={read_max_ms} is below populated 100ms histogram bucket")
            if read_count > 0:
                nearest_rank = math.ceil(read_count * 0.99)
                if nearest_rank > histogram[0]:
                    problems.append(f"line {line_no}: read_p99_ms bucket is >=70")
        for field, threshold in (
            ("internal_heap_min", min_internal_heap),
            ("psram_heap_min", min_psram_heap),
        ):
            parsed = parsed_numbers.get(field)
            if parsed is not None and parsed < threshold:
                problems.append(f"line {line_no}: {field}={parsed} below minimum {threshold}")
        if fields.get("reset_reason") not in ALLOWED_RESET_REASONS:
            problems.append(f"line {line_no}: unexpected reset_reason={fields.get('reset_reason')}")

    for cue, count in counts.items():
        if count == 0:
            problems.append(f"missing cue_end for {cue}")
        elif count > 1:
            problems.append(f"duplicate cue_end for {cue}: {count}")
    if len(cue_end_lines) != len(CANONICAL_CUES):
        problems.append(f"expected 19 cue_end lines, found {len(cue_end_lines)}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--min-internal-heap", type=int, default=20480)
    parser.add_argument("--min-psram-heap", type=int, default=1)
    parser.add_argument("log", type=Path)
    args = parser.parse_args(argv)

    problems = verify(
        args.log,
        min_internal_heap=args.min_internal_heap,
        min_psram_heap=args.min_psram_heap,
    )
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1
    print("verified 19 cue_end lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
