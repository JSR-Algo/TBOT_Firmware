import importlib.util
from pathlib import Path
from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location('convert_neweye', Path(__file__).resolve().parents[1] / 'main/display/lvgl_display/eyes/neweye/convert_neweye.py')
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)


def test_converter_preserves_every_frame_and_timing(tmp_path, monkeypatch):
    source = tmp_path / 'source'
    source.mkdir()
    output = tmp_path / 'out'
    output.mkdir()
    frames = []
    for x in (20, 140, 280):
        frame = Image.new('RGB', (480, 320), 'black')
        ImageDraw.Draw(frame).ellipse((x, 80, x + 90, 170), fill='cyan')
        frames.append(frame)
    frames[0].save(source / 'TJBot-02-Laughing-PR-Talking-480x320.gif', save_all=True, append_images=frames[1:], duration=[40, 50, 40], loop=0)
    monkeypatch.setattr(converter, 'SOURCE', source)
    monkeypatch.setattr(converter, 'DESTINATION', output)
    monkeypatch.setattr(converter, 'EMOTIONS', {'02-Laughing': 'laughing'})
    converter.main()
    with Image.open(output / 'laughing.gif') as result:
        assert result.n_frames == 3
        assert result.size == (240, 160)
        assert result.info['loop'] == 0
        delays = []
        for i in range(result.n_frames):
            result.seek(i)
            result.load()
            delays.append(result.info['duration'])
        assert delays == [40, 50, 40]
