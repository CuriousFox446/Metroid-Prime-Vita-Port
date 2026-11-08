#!/usr/bin/env python3
"""Turn PS Vita button art into the square icons tools/make_prompt_glyphs.py reads.

The Vita build shows the Vita's own buttons in the game's prompts (the "vita" device in
make_prompt_glyphs.py). Its art is not Kenney's: the user supplied it (November 2026), each
picture at its own size and framing (100x100 face buttons and D-pad, sticks of about 520px on
taller canvases, wide L and R). make_prompt_glyphs.py scales an icon's whole canvas to a square,
so here each picture is cropped to its art and centred on a square canvas, its longer edge taking
as much of the side as Kenney's icons take of theirs (75% for the buttons, sticks and D-pad; the
wide Start, Select, L and R a little more, so their lettering stays readable). The results are
written to tools/prompt_icons/vita_<name>.png.

    tools/vita_prompt_icons.py <folder with the art>

The folder's diagonal stick pictures (PS_Vita_L_up_left.png...) are not used: the game's
prompts have four directions.
"""
import os
import sys
from PIL import Image

SIDE = 128
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_icons")

# Source picture -> vendored name, and the share of the side its longer edge takes.
SOURCES = {
    "ButtonIcon-PSvita-Cross.png": ("vita_button_cross", 0.75),
    "ButtonIcon-PSvita-Circle.png": ("vita_button_circle", 0.75),
    "ButtonIcon-PSvita-Square.png": ("vita_button_square", 0.75),
    "ButtonIcon-PSvita-Triangle.png": ("vita_button_triangle", 0.75),
    "ButtonIcon-PSvita-Start.png": ("vita_button_start", 0.85),
    "ButtonIcon-PSvita-Select.png": ("vita_button_select", 0.85),
    # The shoulder buttons and the sticks at rest came without names of their own.
    "file_000000004d5c81f5adf548894c75a322_copy_1086x724.png": ("vita_button_l", 0.85),
    "file_000000004d5c81f5adf548894c75a322_copy_1086x724_1.png": ("vita_button_r", 0.85),
    "file_00000000224081f59f5d40aaaadd11da.png": ("vita_stick_l", 0.75),
    "file_00000000224081f59f5d40aaaadd11da(1).png": ("vita_stick_r", 0.75),
    "ButtonIcon-PSvita-Dpad.png": ("vita_dpad", 0.75),
}
for way, name in (("Up", "up"), ("Down", "down"), ("Left", "left"), ("Right", "right")):
    SOURCES[f"ButtonIcon-PSvita-Dpad_{way}.png"] = (f"vita_dpad_{name}", 0.75)
    SOURCES[f"PS_Vita_L_{name}.png"] = (f"vita_stick_l_{name}", 0.75)
    SOURCES[f"PS_Vita_R_{name}.png"] = (f"vita_stick_r_{name}", 0.75)


def square_icon(path, fill):
    """The picture's art (alpha over 8), scaled so its longer edge is `fill` of SIDE, centred."""
    image = Image.open(path).convert("RGBA")
    box = image.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    if box is not None:
        image = image.crop(box)
    scale = fill * SIDE / max(image.width, image.height)
    image = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                         Image.LANCZOS)
    tile = Image.new("RGBA", (SIDE, SIDE), (0, 0, 0, 0))
    tile.alpha_composite(image, ((SIDE - image.width) // 2, (SIDE - image.height) // 2))
    return tile


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: vita_prompt_icons.py <folder with the art>")
    folder = sys.argv[1]
    missing = [name for name in SOURCES if not os.path.isfile(os.path.join(folder, name))]
    if missing:
        sys.exit("missing from " + folder + ": " + ", ".join(missing))
    for source, (name, fill) in SOURCES.items():
        square_icon(os.path.join(folder, source), fill).save(os.path.join(OUT, name + ".png"))
    print(f"{len(SOURCES)} Vita icons -> {OUT}")


if __name__ == "__main__":
    main()
