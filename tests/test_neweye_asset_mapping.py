import importlib.util
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location(
    'build_default_assets', Path(__file__).resolve().parents[1] / 'scripts/build_default_assets.py'
)
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)

EMOTIONS = set('neutral happy laughing funny sad angry crying loving embarrassed surprised shocked thinking winking cool relaxed delicious kissy confident sleepy silly confused'.split())
NEW = {'laughing', 'cool', 'confused', 'crying'}


def test_all_emotions_use_only_new_gifs(tmp_path):
    source = tmp_path / 'tbot-neon-faces' / 'gif'
    source.mkdir(parents=True)
    output = tmp_path / 'output'
    output.mkdir()
    for name in EMOTIONS:
        (source / f'{name}.gif').write_bytes(name.encode())
    entries = assets.process_emoji_collection(str(source), str(output))
    mapping = {entry['name']: entry['file'] for entry in entries}
    assert set(mapping) == EMOTIONS
    assert len(entries) == len(mapping)
    assert set(mapping.values()) == {f'{name}.gif' for name in NEW}
    assert {p.name for p in output.iterdir()} == set(mapping.values())
    for name in NEW:
        assert mapping[name] == f'{name}.gif'
        assert (output / mapping[name]).read_bytes() == name.encode()
    assert mapping['neutral'] == 'cool.gif'
    assert mapping['happy'] == 'laughing.gif'
    assert mapping['thinking'] == 'confused.gif'
    assert mapping['sad'] == 'crying.gif'


def test_missing_new_gif_fails_instead_of_packing_old_faces(tmp_path):
    source = tmp_path / 'tbot-neon-faces' / 'gif'
    source.mkdir(parents=True)
    (source / 'neutral.gif').write_bytes(b'old')
    output = tmp_path / 'output'
    output.mkdir()
    with pytest.raises(FileNotFoundError):
        assets.process_emoji_collection(str(source), str(output))
    assert not list(output.iterdir())


def test_other_collections_unchanged(tmp_path):
    source = tmp_path / 'other-faces' / 'gif'
    source.mkdir(parents=True)
    output = tmp_path / 'output'
    output.mkdir()
    for name in ('neutral', 'happy', 'laughing'):
        (source / f'{name}.gif').write_bytes(name.encode())
    entries = assets.process_emoji_collection(str(source), str(output))
    assert {e['name']: e['file'] for e in entries} == {
        name: f'{name}.gif' for name in ('neutral', 'happy', 'laughing')
    }
