"""Generate src/standby_anim.c/.h: a small 1-bit animation for the standby screen.

The ATmega328P has no room for a GIF decoder (LZW needs a 4096-entry
dictionary, ~12 KB of RAM), so the GIF is decoded here on the PC instead
and stored as plain 1-bit frames in flash:

  - every frame is resized to fit a SIZE x SIZE box (aspect kept),
  - converted to grayscale and thresholded to two colors
    (1 = foreground, drawn in the theme color; 0 = black background),
  - packed 8 pixels per byte, MSB = leftmost pixel, rows padded to a byte,
  - its GIF delay is kept (clamped to >= 60 ms, about the time the AVR
    needs to draw one 2x-scaled 64 x 64 frame).

Flash cost: ceil(W/8) * H bytes per frame, e.g. 512 B for 64 x 64, so
12 frames = 6 KB.

Without a GIF argument, a default animation (a radar scope with a rotating
sweep line) is generated so the firmware always builds.

Usage:
  python tools/gen_anim.py                       # default animation
  python tools/gen_anim.py my.gif                # convert a GIF
  python tools/gen_anim.py my.gif --invert       # dark subject on light bg
  python tools/gen_anim.py my.gif --threshold 100 --size 64 --frames 12
  python tools/gen_anim.py my.gif --dither --crop 100,0,380,280 --preview p.png
  python tools/gen_anim.py my.gif --local        # src/local/, not committed
                                                 # (delete src/local/ to undo)
"""

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageSequence

ROOT = Path(__file__).resolve().parent.parent
OUT_C = ROOT / "src" / "standby_anim.c"
OUT_H = ROOT / "src" / "standby_anim.h"
MIN_DELAY_MS = 60


def default_frames(size, n=12):
    """Radar scope: ring, crosshair, a sweep line and a short trail."""
    frames = []
    c = (size - 1) / 2
    r = size / 2 - 2
    for i in range(n):
        img = Image.new("L", (size, size), 0)
        d = ImageDraw.Draw(img)
        d.ellipse([c - r, c - r, c + r, c + r], outline=255, width=2)
        d.ellipse([c - r / 2, c - r / 2, c + r / 2, c + r / 2], outline=255)
        d.line([c - r, c, c + r, c], fill=255)
        d.line([c, c - r, c, c + r], fill=255)
        for k, w in ((0, 3), (1, 1)):          # sweep line, then a trail
            a = 2 * math.pi * (i - k * 0.5) / n
            d.line([c, c, c + r * math.sin(a), c - r * math.cos(a)], fill=255, width=w)
        frames.append((img.point(lambda v: 1 if v >= 128 else 0, "1"), 90))
    return frames


def gif_frames(path, size, max_frames, threshold, invert, dither, crop, frange):
    src = Image.open(path)
    raw = []
    for i, fr in enumerate(ImageSequence.Iterator(src)):
        if frange and not (frange[0] <= i < frange[1]):
            continue
        delay = int(fr.info.get("duration", 100) or 100)
        rgba = fr.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255) if not invert else (255,) * 4)
        bg.alpha_composite(rgba)                # transparent pixels -> background
        gray = bg.convert("L")
        if crop:                                # x0,y0,x1,y1 in source pixels
            gray = gray.crop(crop)
        gray.thumbnail((size, size), Image.LANCZOS)
        raw.append((gray, delay))

    # Too many frames: keep an even subset and stretch the delays to match,
    # so the loop keeps its original length.
    if len(raw) > max_frames:
        total = sum(d for _, d in raw)
        step = len(raw) / max_frames
        raw = [raw[int(i * step)] for i in range(max_frames)]
        each = total // max_frames
        raw = [(g, each) for g, _ in raw]

    if threshold is None:                       # mean brightness of all frames
        pixels = b"".join(g.tobytes() for g, _ in raw)
        threshold = sum(pixels) // len(pixels)

    frames = []
    for gray, delay in raw:
        if dither:                              # Floyd-Steinberg: more detail
            src = gray.point(lambda v: 255 - v) if invert else gray
            bits = src.convert("1")             # (dither ignores --threshold)
        elif invert:
            bits = gray.point(lambda v: 1 if v < threshold else 0, "1")
        else:
            bits = gray.point(lambda v: 1 if v >= threshold else 0, "1")
        frames.append((bits, max(MIN_DELAY_MS, delay)))
    return frames, threshold


def pack(img):
    w, h = img.size
    px = img.load()
    out = []
    for y in range(h):
        for xb in range(0, w, 8):
            b = 0
            for k in range(8):
                x = xb + k
                if x < w and px[x, y]:
                    b |= 0x80 >> k
            out.append(b)
    return out


def write(frames, source, local=False):
    out_c, out_h = OUT_C, OUT_H
    if local:                                   # git-ignored copy, see Makefile
        out_c = ROOT / "src" / "local" / "standby_anim.c"
        out_h = ROOT / "src" / "local" / "standby_anim.h"
        out_c.parent.mkdir(exist_ok=True)
    w, h = frames[0][0].size
    stride = (w + 7) // 8
    lines = [
        "/*",
        " * standby_anim.c - 1-bit animation frames for the standby screen.",
        " *",
        f" * GENERATED by tools/gen_anim.py from {source}. Do not edit.",
        f" * {len(frames)} frames, {w} x {h} px, {stride * h} bytes each,",
        " * rows packed MSB = leftmost pixel.",
        " */",
        "",
        '#include "standby_anim.h"',
        "",
        "const uint8_t anim_bits[ANIM_FRAMES][ANIM_H * ANIM_STRIDE] PROGMEM = {",
    ]
    for i, (img, _) in enumerate(frames):
        data = pack(img)
        lines.append(f"    {{   /* frame {i} */")
        for j in range(0, len(data), 16):
            lines.append("        " + ", ".join(f"0x{b:02X}" for b in data[j : j + 16]) + ",")
        lines.append("    },")
    lines += [
        "};",
        "",
        "const uint16_t anim_delay_ms[ANIM_FRAMES] PROGMEM = {",
        "    " + ", ".join(str(d) for _, d in frames),
        "};",
        "",
    ]
    out_c.write_text("\n".join(lines), encoding="utf-8", newline="\n")

    out_h.write_text(
        "\n".join([
            "/*",
            " * standby_anim.h - size of the standby animation.",
            " * GENERATED by tools/gen_anim.py. Do not edit.",
            " */",
            "",
            "#ifndef STANDBY_ANIM_H",
            "#define STANDBY_ANIM_H",
            "",
            "#include <stdint.h>",
            "#include <avr/pgmspace.h>",
            "",
            f"#define ANIM_W       {w}",
            f"#define ANIM_H       {h}",
            f"#define ANIM_STRIDE  {stride}      /* bytes per row */",
            f"#define ANIM_FRAMES  {len(frames)}",
            "",
            "extern const uint8_t  anim_bits[ANIM_FRAMES][ANIM_H * ANIM_STRIDE] PROGMEM;",
            "extern const uint16_t anim_delay_ms[ANIM_FRAMES] PROGMEM;",
            "",
            "#endif /* STANDBY_ANIM_H */",
            "",
        ]),
        encoding="utf-8",
        newline="\n",
    )
    print(f"{len(frames)} frames, {w}x{h}, {stride * h * len(frames)} bytes of flash")
    print(f"wrote {out_c.relative_to(ROOT)}; run make clean before make")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gif", nargs="?", help="GIF to convert (default: built-in radar)")
    ap.add_argument("--size", type=int, default=64, help="fit inside SIZE x SIZE px")
    ap.add_argument("--frames", type=int, default=12, help="max frames kept")
    ap.add_argument("--threshold", type=int, help="0-255 cut-off (default: mean)")
    ap.add_argument("--invert", action="store_true", help="dark subject on light background")
    ap.add_argument("--dither", action="store_true", help="dither instead of a hard cut-off (photos)")
    ap.add_argument("--range", help="a:b keep only GIF frames a..b-1 (a short loop)")
    ap.add_argument("--crop", help="x0,y0,x1,y1 crop in source pixels before resizing")
    ap.add_argument("--local", action="store_true",
                    help="write src/local/ (git-ignored) instead of src/")
    ap.add_argument("--preview", help="also save a PNG strip of the frames (2x) here")
    args = ap.parse_args()

    if args.gif:
        crop = tuple(int(v) for v in args.crop.split(",")) if args.crop else None
        frange = tuple(int(v) for v in args.range.split(":")) if args.range else None
        frames, thr = gif_frames(args.gif, args.size, args.frames, args.threshold,
                                 args.invert, args.dither, crop, frange)
        print("dithered" if args.dither else f"threshold {thr}")
        write(frames, Path(args.gif).name, args.local)
    else:
        frames = default_frames(args.size)
        write(frames, "the built-in radar animation", args.local)
    if args.preview:
        w, h = frames[0][0].size
        strip = Image.new("L", (len(frames) * (2 * w + 4), 2 * h), 60)
        for i, (img, _) in enumerate(frames):
            big = img.convert("L").resize((2 * w, 2 * h), Image.NEAREST)
            strip.paste(big, (i * (2 * w + 4), 0))
        strip.save(args.preview)
        print(f"preview: {args.preview}")


if __name__ == "__main__":
    main()
