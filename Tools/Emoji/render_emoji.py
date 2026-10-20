"""Renders the drone's emoji to PNGs from Windows' own colour emoji font, Segoe UI Emoji.

    python Tools/Emoji/render_emoji.py

Writes Tools/Emoji/png/T_Emoji_<Name>.png, 256x256 with alpha, each emoji centred with a little room
round it. Import them into /Game/Emoji (sRGB on) and put them in the drone's Emoji component.
Needs Pillow.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 256
PADDING = 10
FONT = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "seguiemj.ttf")

# In the order they go round the menu, clockwise from the top.
EMOJI = [
    ("Grin", "\U0001F600"),
    ("Joy", "\U0001F602"),
    ("HeartEyes", "\U0001F60D"),
    ("Wow", "\U0001F62E"),
    ("Cry", "\U0001F622"),
    ("Angry", "\U0001F621"),
    ("ThumbsUp", "\U0001F44D"),
    ("Heart", "\u2764\uFE0F"),
    ("Fire", "\U0001F525"),
]


def render(text, out_path):
    # Draw big, then fit: the font's own sizes leave the glyph's box a little off its em square.
    font = ImageFont.truetype(FONT, 400)
    canvas = Image.new("RGBA", (600, 600), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text((100, 50), text, font=font, embedded_color=True)

    box = canvas.getbbox()
    if box is None:
        raise RuntimeError(f"{out_path}: the font drew nothing")

    glyph = canvas.crop(box)
    scale = (SIZE - 2 * PADDING) / max(glyph.size)
    glyph = glyph.resize((max(1, round(glyph.width * scale)), max(1, round(glyph.height * scale))), Image.LANCZOS)

    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    image.alpha_composite(glyph, ((SIZE - glyph.width) // 2, (SIZE - glyph.height) // 2))
    image.save(out_path)


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "png")
    os.makedirs(out_dir, exist_ok=True)

    for name, text in EMOJI:
        path = os.path.join(out_dir, f"T_Emoji_{name}.png")
        render(text, path)
        print(path)


if __name__ == "__main__":
    sys.exit(main())
