from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EMOJI_COLLECTION_CC = ROOT / "main/display/lvgl_display/emoji_collection.cc"
NEON_FACES = ROOT / "main/display/lvgl_display/tbot-neon-faces/gif"


def test_eyes_collection_maps_speaking_emotions_to_multiple_faces():
    source = EMOJI_COLLECTION_CC.read_text()
    assert "EyesEmojiCollection (static eye PNGs + happy GIF) was removed" in source
    assert "tbot-neon-faces GIF collection" in source

    speaking_emotions = [
        "happy",
        "laughing",
        "funny",
        "loving",
        "confident",
        "cool",
        "silly",
        "thinking",
        "confused",
        "surprised",
    ]

    gif_faces = set()
    for emotion in speaking_emotions:
        face = NEON_FACES / f"{emotion}.gif"
        assert face.exists(), f"missing neon GIF face for {emotion}"
        gif_faces.add(face.name)

    assert len(gif_faces) == len(speaking_emotions)
