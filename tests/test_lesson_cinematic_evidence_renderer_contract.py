from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_production_renderer_hooks_use_release_cinematic_evidence_only():
    sources = {
        "main/main.cc": read("main/main.cc"),
        "main/lesson_flattened_cinematic_renderer.cc": read(
            "main/lesson_flattened_cinematic_renderer.cc"
        ),
        "main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc": read(
            "main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc"
        ),
    }

    for path, source in sources.items():
        assert '#include "lesson_cinematic_evidence.h"' in source, path
        assert "LessonCinematicHilTelemetry" not in source, path
        assert "LessonCinematicHilCueEndReason" not in source, path
        assert "LessonCinematicHilFault" not in source, path
        assert "HIL_CINE" not in source, path

    main = sources["main/main.cc"]
    assert main.count("LessonCinematicEvidenceBoot();") == 1

    renderer = sources["main/lesson_flattened_cinematic_renderer.cc"]
    required_renderer_hooks = {
        "LessonCinematicEvidenceBeginCue(",
        "LessonCinematicEvidenceEmitCueEnd(",
        "LessonCinematicEvidenceRecordRead(",
        "LessonCinematicEvidenceRecordLateTick(",
        "LessonCinematicEvidenceRecordQueueError();",
        "LessonCinematicEvidenceRecordQueueTimeout();",
        "LessonCinematicEvidenceRecordDmaError();",
        "LessonCinematicEvidenceRecordParserFailure();",
        "LessonCinematicEvidenceRecordIoError();",
    }
    for hook in required_renderer_hooks:
        assert hook in renderer

    for reason in ("kNatural", "kStop", "kCancel", "kReplacement", "kFailure", "kDiscard"):
        assert f"LessonCinematicCueEndReason::{reason}" in renderer

    board = sources["main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc"]
    assert "LessonCinematicEvidenceRecordFrameQueued(" in board
    assert "LessonCinematicEvidenceRecordPanelCompletion(" in board


def test_affected_native_runners_link_release_cinematic_evidence_collector():
    runners = (
        "scripts/run_host_native_lesson_handler_test.sh",
        "scripts/run_host_native_lesson_flattened_cinematic_renderer_test.sh",
        "scripts/run_host_native_lesson_renderer_trace_test.sh",
        "scripts/run_host_native_lesson_cinematic_renderer_test.sh",
    )

    for path in runners:
        script = read(path)
        assert "lesson_cinematic_evidence.cc" in script, path
        assert "lesson_cinematic_hil_telemetry.cc" not in script, path
        assert "-fsanitize=address,undefined" in script, path
        assert "-fno-omit-frame-pointer" in script, path
