#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go launcher art.

The artwork (data/icons/src/fujinet-go-coleco-foreground.png) is the exact
same transparent FujiNet mark the CoCo/ADAM/Apple II/Intv desktop apps use --
byte-identical, copied from fujinet-go-intv-desktop's own copy -- composited
over this product's own background colour, so the whole family reads as one
product line while each target still gets a distinct badge colour.

Everything else about the composite
(rounded-square mask, corner radius, foreground zoom, output sizes) matches
the sibling repos' own tools/icons/make-icons.py exactly.

The results are committed (data/icons/hicolor/..., data/icons/*.icns) so
building the project needs no image tooling; re-run this only when the
artwork or background colour changes:

    python3 tools/icons/make-icons.py
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
FOREGROUND = ROOT / "data/icons/src/fujinet-go-coleco-foreground.png"
OUTDIR = ROOT / "data/icons/hicolor"
ICNS_OUT = ROOT / "data/icons/fujinet-go-coleco.icns"
ICO_OUT = ROOT / "frontends/windows/app.ico"

# TMS9918 "dark blue" (colour 4): #5455ED.
#
# The ADAM app (same emulator core, same VDP) wears the 9918's "light blue"
# (colour 5), and this one is its pair: the 9918's dark blue. The value comes
# from the datasheet colour table the way adamcore's palette.c derives it
# (third_party/adamcore/src/palette.c: Y 0.40, R-Y 0.40, B-Y 1.00, with
# R = Y + (R-Y) - 0.47, B = Y + (B-Y) - 0.47, G = (Y - 0.30 R - 0.11 B) / 0.59):
#
#   R 0.33, G 0.337, B 0.93  ->  #5455ED
#
# White (the shared foreground mark) has a 5.4:1 contrast against it, better
# than the 3.2:1 it has against ADAM's #8D84E5, so no lightness tuning is
# needed. The same colour is COLECOSESSION_ACCENT_RGB, the UI accent.
#
# Per user request 2026-11-04; replaces the olive complement of ADAM's colour.
BACKGROUND = (0x54, 0x55, 0xED, 0xFF)
MASTER = 1024                            # render big, downsample with LANCZOS
CORNER_RADIUS = 0.22                     # fraction of the edge
FOREGROUND_ZOOM = 1.18                   # Android's mask crops; compensate a little
SIZES = (16, 24, 32, 48, 64, 128, 192, 256, 512)
ICNS_SIZES = (32, 64, 128, 256, 512, 1024)  # every size macOS's icns TOC references


def render_master() -> Image.Image:
    art = Image.open(FOREGROUND).convert("RGBA")

    # Rounded-square background on a transparent canvas, drawn at 4x and
    # downsampled so the corners are antialiased.
    scale = 4
    big = Image.new("RGBA", (MASTER * scale, MASTER * scale), (0, 0, 0, 0))
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, MASTER * scale - 1, MASTER * scale - 1),
        radius=int(MASTER * scale * CORNER_RADIUS),
        fill=BACKGROUND,
    )
    icon = big.resize((MASTER, MASTER), Image.LANCZOS)

    art_size = int(MASTER * FOREGROUND_ZOOM)
    art = art.resize((art_size, art_size), Image.LANCZOS)
    offset = (MASTER - art_size) // 2
    overlay = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    overlay.paste(art, (offset, offset), art)

    # Keep the foreground inside the rounded silhouette.
    composed = Image.alpha_composite(icon, overlay)
    composed.putalpha(Image.composite(composed.getchannel("A"),
                                      Image.new("L", (MASTER, MASTER), 0),
                                      icon.getchannel("A")))
    return composed


def main() -> int:
    if not FOREGROUND.exists():
        print(f"missing artwork: {FOREGROUND}", file=sys.stderr)
        return 1

    master = render_master()
    for size in SIZES:
        out = OUTDIR / f"{size}x{size}" / "apps" / "fujinet-go-coleco.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        master.resize((size, size), Image.LANCZOS).save(out, optimize=True)
        print(f"wrote {out.relative_to(ROOT)}")

    # Pillow's ICNS writer works on any platform (no iconutil needed): it
    # just packs PNGs into the icns TOC. Pass every non-master size in
    # explicitly, LANCZOS-downsampled from the 1024 master like the hicolor
    # set above, so nothing gets a blurry re-resize from a smaller source.
    variants = [master.resize((size, size), Image.LANCZOS)
                for size in ICNS_SIZES if size != master.width]
    master.save(ICNS_OUT, format="ICNS", append_images=variants)
    print(f"wrote {ICNS_OUT.relative_to(ROOT)}")

    # The Windows .rc-embedded icon (frontends/windows/resource.rc's own
    # IDI_APPICON). Pillow's ICO writer packs whichever sizes are passed as
    # the `sizes` kwarg, resampling from `master` itself -- matching the
    # sibling repos' own app.ico (16x16 and 32x32, both 32bpp).
    ICO_OUT.parent.mkdir(parents=True, exist_ok=True)
    master.save(ICO_OUT, format="ICO", sizes=[(16, 16), (32, 32)])
    print(f"wrote {ICO_OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
