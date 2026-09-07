#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go launcher art.

The artwork (data/icons/src/fujinet-go-coleco-foreground.png) is the exact
same transparent FujiNet mark the CoCo/ADAM/Apple II/Intv desktop apps use --
byte-identical, copied from fujinet-go-intv-desktop's own copy -- composited
over this product's own background colour, so the whole family reads as one
product line while each target still gets a distinct badge colour.

Background is dark red (#8b0000), per an explicit user request (2026-09-04).
Deliberately much darker than the Apple II target's bright red (#f44336) so
the two stay distinct in a dock. Everything else about the composite
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

# Olive/moss green: the COMPLEMENT of fujinet-go-adam-desktop's #8D84E5.
#
# The ADAM app is the closest relative -- same emulator core, same VDP -- so
# its icon is what this one is read against, and the brief was "the complement
# colour of the ADAM version". Derivation, so the number is reproducible
# rather than magic:
#
#   ADAM  #8D84E5  ->  H 245.6 deg, S 0.651, HSL L 0.708
#   here  #8C9720  ->  H  65.6 deg  (exactly 180 deg opposite), S 0.651
#
# The lightness is NOT simply carried across. Every app in the family
# composites the SAME foreground artwork (all six data/icons/src/*.png are
# byte-identical), so what has to be preserved is the contrast against it.
# Yellow-green is intrinsically far more luminous than blue-violet at equal
# HSL lightness, so the literal complement #DCE584 has 2.6x ADAM's relative
# luminance and drops foreground contrast from 3.20:1 to 1.35:1 -- the
# wordmark washes out. HSL L is instead chosen so relative luminance matches
# ADAM's 0.278 exactly, which puts the contrast ratio back at 3.20:1.
#
# Per user note 2026-09-07.
BACKGROUND = (0x8C, 0x97, 0x20, 0xFF)
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
