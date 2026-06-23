"""CR-FW-10b — IsPassiveStep firmware mirror vs ESP _is_passive_step cross-repo parity.

The firmware classifier ``IsPassiveStepType`` (main/lesson_handler.cc) and the ESP
runtime fallback set ``PASSIVE_STEP_TYPES`` (esp32-server core/lesson/runtime.py)
MUST stay byte-identical. They are the no-completionClass fallback that decides
whether a lesson step auto-advances (passive narration) or waits for the child
(interactive). If the two sets drift, firmware and robot-server disagree on
whether a step auto-advances or holds the listen window open — the robot would
either hang on a step the server already advanced past, or skip a step the server
still considers pending.

This is a pure-python SOURCE-TEXT parity test (no compilation, no hardware),
mirroring the cross-repo source-reading style of
``test_phone_robot_afsk_connect_contract.py`` (ROOT.parent / "esp32-server") and
the regex/parse style of the other ``tests/test_lesson_*.py`` files. It does NOT
trust a hardcoded expected list as the source of truth: it regex-extracts BOTH
sets from BOTH live source files and asserts set EQUALITY between them. A drift in
either repo (an added, removed, or renamed type) turns it red.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIRMWARE_LESSON_HANDLER = ROOT / "main" / "lesson_handler.cc"
ESP_LESSON_RUNTIME = (
    ROOT.parent
    / "esp32-server"
    / "main"
    / "tbot-server"
    / "core"
    / "lesson"
    / "runtime.py"
)

# Cross-check sanity floor: the v1 builtin passive narration kinds. This is NOT
# the source of truth (the two source files are) — it is a non-emptiness /
# wrong-extraction tripwire so a regex that silently matches nothing can never
# make the parity assertion pass vacuously (empty == empty).
V1_PASSIVE_BUILTINS = frozenset({"greeting", "review", "focus", "feedback", "celebrate"})


def _read(path: Path) -> str:
    assert path.exists(), f"expected source file is missing: {path}"
    return path.read_text(encoding="utf-8")


def _firmware_passive_set() -> set:
    """Parse the C++ ``IsPassiveStepType`` body and collect every step_type
    literal that is compared with ``strcmp(step_type, "...") == 0``."""
    src = _read(FIRMWARE_LESSON_HANDLER)
    fn = re.search(
        r"bool\s+IsPassiveStepType\s*\([^)]*\)\s*\{(.*?)\n\}",
        src,
        re.DOTALL,
    )
    assert fn is not None, (
        "could not locate the IsPassiveStepType(const char*) function body in "
        f"{FIRMWARE_LESSON_HANDLER}; the firmware passive classifier moved or "
        "was renamed — update this parity test to match the new source shape."
    )
    body = fn.group(1)
    literals = re.findall(
        r'strcmp\(\s*step_type\s*,\s*"([^"]+)"\s*\)\s*==\s*0',
        body,
    )
    return set(literals)


def _esp_passive_set() -> set:
    """Parse the python ``PASSIVE_STEP_TYPES = frozenset({...})`` literal and
    collect every string element."""
    src = _read(ESP_LESSON_RUNTIME)
    decl = re.search(
        r"PASSIVE_STEP_TYPES\s*=\s*frozenset\s*\(\s*\{(.*?)\}\s*\)",
        src,
        re.DOTALL,
    )
    assert decl is not None, (
        "could not locate the PASSIVE_STEP_TYPES = frozenset({...}) declaration in "
        f"{ESP_LESSON_RUNTIME}; the ESP passive fallback set moved or was renamed — "
        "update this parity test to match the new source shape."
    )
    # Accept both single- and double-quoted string elements.
    literals = re.findall(r"""['"]([^'"]+)['"]""", decl.group(1))
    return set(literals)


def test_firmware_passive_set_extracts_nonempty_v1_builtins():
    """Guard against a vacuous parity pass: the firmware extraction must yield a
    non-empty set that contains the v1 builtin passive narration kinds."""
    fw = _firmware_passive_set()
    assert fw, "regex extracted ZERO firmware passive step types — extraction is broken"
    missing = V1_PASSIVE_BUILTINS - fw
    assert not missing, f"firmware passive set is missing v1 builtins: {sorted(missing)}"


def test_esp_passive_set_extracts_nonempty_v1_builtins():
    """Guard against a vacuous parity pass: the ESP extraction must yield a
    non-empty set that contains the v1 builtin passive narration kinds."""
    esp = _esp_passive_set()
    assert esp, "regex extracted ZERO ESP passive step types — extraction is broken"
    missing = V1_PASSIVE_BUILTINS - esp
    assert not missing, f"ESP passive set is missing v1 builtins: {sorted(missing)}"


def test_firmware_and_esp_passive_step_sets_are_byte_identical():
    """The cross-repo invariant: firmware IsPassiveStepType and ESP
    PASSIVE_STEP_TYPES must classify EXACTLY the same step types as passive."""
    fw = _firmware_passive_set()
    esp = _esp_passive_set()

    only_firmware = fw - esp
    only_esp = esp - fw

    assert fw == esp, (
        "PASSIVE step-type parity DRIFT between firmware and robot-server.\n"
        f"  firmware IsPassiveStepType ({FIRMWARE_LESSON_HANDLER.name}): {sorted(fw)}\n"
        f"  ESP PASSIVE_STEP_TYPES ({ESP_LESSON_RUNTIME.name}): {sorted(esp)}\n"
        f"  only in firmware: {sorted(only_firmware)}\n"
        f"  only in ESP: {sorted(only_esp)}\n"
        "A passive step auto-advances on its render ack; an interactive step "
        "holds the child listen window. If these sets disagree, firmware and "
        "robot-server disagree on auto-advance vs wait for the drifted types."
    )
