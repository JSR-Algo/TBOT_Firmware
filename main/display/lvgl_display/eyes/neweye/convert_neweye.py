"""Optimize the supplied TJBot GIFs for the firmware's default face collection.

Run with Python 3 and Pillow. Missing source GIFs leave existing faces intact.
"""

from pathlib import Path

from PIL import Image


SOURCE = Path(__file__).resolve().parent
DESTINATION = SOURCE.parent.parent / "tbot-neon-faces" / "gif"
EMOTIONS = {
    "02-Laughing": "laughing",
    "04-Sunglasses": "cool",
    "07-Dizzy": "confused",
    "08-Crying": "crying",
}


def main():
    for name, emotion in EMOTIONS.items():
        source = SOURCE / f"TJBot-{name}-PR-Talking-480x320.gif"
        if not source.exists():
            continue
        frames, durations = [], []
        with Image.open(source) as animation:
            for index in range(animation.n_frames):
                animation.seek(index)
                duration = animation.info.get("duration", 40)
                frame = Image.new("RGBA", animation.size, "black")
                frame.alpha_composite(animation.convert("RGBA"))
                frame = frame.convert("RGB").resize((240, 160), Image.Resampling.LANCZOS)
                frames.append(frame)
                durations.append(duration)
        # A shared palette preserves delta compression and avoids color flicker.
        samples = frames[::12]
        palette_sheet = Image.new("RGB", (240, 160 * len(samples)))
        for index, frame in enumerate(samples):
            palette_sheet.paste(frame, (0, index * 160))
        palette = palette_sheet.quantize(
            colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
        )
        frames = [frame.quantize(palette=palette, dither=Image.Dither.NONE)
                  for frame in frames]
        destination = DESTINATION / f"{emotion}.gif"
        frames[0].save(
            destination, save_all=True, append_images=frames[1:],
            duration=durations, loop=0, optimize=True, disposal=1,
        )
        print(f"{emotion}: {len(frames)} frames, {sum(durations)} ms, "
              f"{destination.stat().st_size} bytes")


if __name__ == "__main__":
    main()
