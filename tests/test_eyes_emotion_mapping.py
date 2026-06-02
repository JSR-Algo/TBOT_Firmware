from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EMOJI_COLLECTION_CC = ROOT / "main/display/lvgl_display/emoji_collection.cc"


def test_eyes_collection_maps_speaking_emotions_to_multiple_faces():
    source = EMOJI_COLLECTION_CC.read_text().split(
        "EyesEmojiCollection::EyesEmojiCollection()", 1
    )[1]
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

    used_faces = set()
    for emotion in speaking_emotions:
        marker = f'AddEmoji("{emotion}",'
        start = source.find(marker)
        assert start != -1, f"missing mapping for {emotion}"
        line = source[start : source.find("\n", start)]
        for face in (
            "eye_openeyes",
            "eye_halfsleepeyes",
            "eye_lefteyes",
            "eye_righteyes",
            "eye_lookdoweyes",
            "eye_sadeyes",
            "eye_happy_gif_data",
        ):
            if face in line:
                used_faces.add(face)

    assert len(used_faces) >= 4
