from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "main" / "lesson_layered_cinematic_renderer.cc"
V3_SOURCE = Path(__file__).resolve().parents[1] / "main" / "lesson_cinematic_renderer.cc"


def test_prepare_logs_each_layer_decode_boundary_with_elapsed_and_deadline():
    source = SOURCE.read_text()

    assert '"prepare decode layer=background frame=static elapsed_ms=%"' in source
    assert '"prepare decode layer=teachingObject frame=static elapsed_ms=%"' in source
    assert '"prepare decode layer=robotOverlay frame=%u elapsed_ms=%"' in source
    assert '" deadline_ms=%"' in source
    assert 'config.phase_id' in source
    assert source.index("const bool decoded = ops_.decode_video") < source.index(
        '"prepare decode layer=robotOverlay frame=%u elapsed_ms=%"'
    ) < source.index("if (!decoded)")


def test_v3_logs_every_mp4_decode_attempt_before_returning_the_error():
    source = V3_SOURCE.read_text()
    decode_call = "const bool decoded = ops_.decode("
    diagnostic = '"decode layer=%u frame=%u elapsed_ms=%"'

    assert decode_call in source
    assert diagnostic in source
    assert '" deadline_ms=%"' in source
    assert '" decoded=%d operation_error=%u"' in source
    assert source.index(decode_call) < source.index(diagnostic) < source.index("if (!decoded)")
