from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "main" / "lesson_layered_cinematic_renderer.cc"


def test_prepare_logs_each_layer_decode_boundary_with_elapsed_and_deadline():
    source = SOURCE.read_text()

    assert '"prepare decode layer=background frame=static elapsed_ms=%"' in source
    assert '"prepare decode layer=teachingObject frame=static elapsed_ms=%"' in source
    assert '"prepare decode layer=robotOverlay frame=%u elapsed_ms=%"' in source
    assert '" deadline_ms=%"' in source
    assert 'config.phase_id' in source
