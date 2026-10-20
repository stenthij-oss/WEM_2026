"""Renders the start screen's title, CATASTROPHE in Garamond, as a coverage mask (white letters, 0 elsewhere)."""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1]
TEXT = "CATASTROPHE"
WIDTH, HEIGHT = 4096, 1024
MARGIN = 96          # left clear on every side, so the mips do not bleed the letters into the edge
TRACKING = 0.06      # room added after every letter, as a share of the font size

font_path = os.path.join(os.environ["WINDIR"], "Fonts", "GARA.TTF")


def measure(size):
    font = ImageFont.truetype(font_path, size)
    advances = [font.getlength(c) for c in TEXT]
    width = sum(advances) + TRACKING * size * (len(TEXT) - 1)
    left, top, right, bottom = font.getbbox(TEXT)
    return font, advances, width, top, bottom


# The largest size that fits inside the margins.
size = 800
while True:
    font, advances, width, top, bottom = measure(size)
    if width <= WIDTH - 2 * MARGIN and bottom - top <= HEIGHT - 2 * MARGIN:
        break
    size -= 4

image = Image.new("L", (WIDTH, HEIGHT), 0)
draw = ImageDraw.Draw(image)
x = (WIDTH - width) / 2
y = (HEIGHT - (bottom - top)) / 2 - top
for c, advance in zip(TEXT, advances):
    draw.text((x, y), c, font=font, fill=255)
    x += advance + TRACKING * size

image.save(OUT)
print(f"size {size}px, text {width:.0f}x{bottom - top}px -> {OUT}")
