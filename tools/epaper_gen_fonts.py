"""Bake Inter into 1 bit glyph tables for the shared e-paper UI.

Run with `pixi run epaper-fonts`. Writes one generated file per font set into
firmware/common/epaper_ui/src/, fonts_compact_gen.cpp for the 400 x 300 class
and fonts_large_gen.cpp for the 800 x 480 class, and a preview PNG per set
under the ignored root artifacts/epaper-ui-preview/. The output is committed,
so a firmware build needs no Python. Glyph rows are MSB first and padded to a
byte, set bit is black.

`--check` regenerates in memory and fails when a committed file differs, the
gate runs it as `pixi run epaper-fonts-test`.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageFont

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/common/epaper_ui"
TTF = COMPONENT / "vendor/inter/InterVariable.ttf"
PREVIEW_DIR = ROOT / "artifacts/epaper-ui-preview"

ASCII = list(range(0x20, 0x7F))
# Units and marks the pages print, micro, superscript three, degree,
# subscript two, middle dot, en dash and the up and down arrows.
EXTRA = [0x00B5, 0x00B3, 0x00B0, 0x2082, 0x00B7, 0x2013, 0x2191, 0x2193]
DIGITS = [ord(c) for c in " 0123456789.-"]


@dataclass
class Spec:
    name: str
    px: int
    weight: int
    opsz: float
    charset: list[int]
    threshold: int = 110  # Alpha above this is black.


@dataclass
class FontSet:
    name: str  # The C++ name, opensensor::ui::fonts::<name>.
    size: tuple[int, int]  # The preview canvas, the class's panel size.
    specs: list[Spec]

    @property
    def out(self) -> Path:
        return COMPONENT / f"src/fonts_{self.name}_gen.cpp"


# The five roles a layout draws with, in FontSet order. The compact sizes are
# the ones the 400 x 300 CrowPanel shipped with. The large sizes scale them
# for 800 x 480, where the panel is about 1.6 times as tall and is read from
# further away. Inter's optical size axis tops out at 32.
SETS = [
    FontSet(
        "compact",
        (400, 300),
        [
            Spec("small", 13, 500, 14, ASCII + EXTRA, threshold=90),
            Spec("label", 14, 600, 14, ASCII + EXTRA),
            Spec("title", 18, 700, 18, ASCII + EXTRA),
            Spec("value", 28, 700, 28, ASCII + EXTRA),
            Spec("hero", 64, 700, 32, DIGITS),
        ],
    ),
    FontSet(
        "large",
        (800, 480),
        [
            Spec("small", 20, 500, 20, ASCII + EXTRA, threshold=90),
            Spec("label", 22, 600, 22, ASCII + EXTRA),
            Spec("title", 30, 700, 30, ASCII + EXTRA),
            Spec("value", 46, 700, 32, ASCII + EXTRA),
            Spec("hero", 112, 700, 32, DIGITS),
        ],
    ),
]


def load(spec: Spec) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(TTF), spec.px)
    values = []
    for axis in font.get_variation_axes():
        name = axis["name"].decode() if isinstance(axis["name"], bytes) else axis["name"]
        if "eight" in name:
            values.append(spec.weight)
        elif "ptical" in name:
            values.append(spec.opsz)
        else:
            values.append(axis["default"])
    font.set_variation_by_axes(values)
    return font


def degree_ring(px: int):
    """A hollow ring for U+00B0. Inter's degree sign fills in below 20 px."""
    d = max(4, round(px * 0.32))
    rows = []
    for y in range(d):
        row = 0
        for x in range(d):
            # Distance from the centre, keep a one pixel band.
            cx = cy = (d - 1) / 2
            r = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d / 2 - 1.05 <= r <= d / 2 - 0.1:
                row |= 0x80 >> x
        rows.append(row)
    return d, rows


def render(spec: Spec):
    font = load(spec)
    ascent, descent = font.getmetrics()
    glyphs = []
    bitmap = bytearray()
    for cp in sorted(set(spec.charset)):
        ch = chr(cp)
        if cp == 0x00B0 and spec.px < 20:
            d, rows = degree_ring(spec.px)
            cap = font.getbbox("H", anchor="ls")[1]  # Negative, top of caps.
            glyphs.append((cp, len(bitmap), d, d, 1, cap, d + 2))
            bitmap += bytes(rows)
            continue
        advance = round(font.getlength(ch))
        mask, offset = font.getmask2(ch, mode="L", anchor="ls")
        bbox = None
        img = None
        if mask.size[0] and mask.size[1]:
            img = Image.frombytes("L", mask.size, bytes(mask))
            bbox = img.point(lambda v: 255 if v > spec.threshold else 0).getbbox()
        if bbox is None or img is None:
            if cp != 0x20:
                raise SystemExit(f"{spec.name}: U+{cp:04X} has no ink in Inter")
            glyphs.append((cp, len(bitmap), 0, 0, 0, 0, advance))
            continue
        x0, y0, x1, y1 = bbox
        stride = (x1 - x0 + 7) // 8
        start = len(bitmap)
        for y in range(y0, y1):
            row = bytearray(stride)
            for x in range(x0, x1):
                if img.getpixel((x, y)) > spec.threshold:
                    row[(x - x0) >> 3] |= 0x80 >> ((x - x0) & 7)
            bitmap += row
        # x offset from the pen, y offset of the top row from the baseline.
        glyphs.append((cp, start, x1 - x0, y1 - y0, offset[0] + x0, offset[1] + y0, advance))
    # The Glyph fields are uint8 and int8, so a size that outgrows them fails
    # here and not as a silent wrap in the C++.
    for cp, _, w, h, xo, yo, adv in glyphs:
        if not (0 <= w <= 255 and 0 <= h <= 255 and 0 <= adv <= 255
                and -128 <= xo <= 127 and -128 <= yo <= 127):
            raise SystemExit(f"{spec.name} {spec.px}px: U+{cp:04X} does not fit a Glyph")
    return ascent, descent, glyphs, bitmap


def emit(fs: FontSet, results) -> str:
    out = [
        "// Generated by tools/epaper_gen_fonts.py from Inter 4.1 (SIL OFL 1.1).",
        "// Do not edit, run `pixi run epaper-fonts`.",
        "// clang-format off",
        '#include "epaper_ui/fonts.h"',
        "",
        "namespace opensensor::ui::fonts {",
        "namespace {",
    ]
    for spec, (_, _, glyphs, bitmap) in results:
        out.append(f"constexpr std::uint8_t k_{spec.name}_bits[] = {{")
        for i in range(0, len(bitmap), 20):
            out.append("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i : i + 20]) + ",")
        if not bitmap:
            out.append("    0x00,")
        out.append("};")
        out.append(f"constexpr Glyph k_{spec.name}_glyphs[] = {{")
        for cp, off, w, h, xo, yo, adv in glyphs:
            out.append(f"    {{0x{cp:04x}, {off}, {w}, {h}, {xo}, {yo}, {adv}}},")
        out.append("};")
    out.append("")
    for spec, (ascent, descent, glyphs, _) in results:
        out.append(
            f"constexpr Font k_{spec.name}{{k_{spec.name}_glyphs, k_{spec.name}_bits, "
            f"{ascent}, {descent}}};"
        )
    out.append("}  // namespace")
    out.append("")
    roles = ", ".join(f"k_{spec.name}" for spec, _ in results)
    out.append(f"const FontSet {fs.name}{{{roles}}};")
    out.append("}  // namespace opensensor::ui::fonts")
    out.append("// clang-format on")
    out.append("")
    return "\n".join(out)


def preview(fs: FontSet, results, path: Path) -> None:
    width, height = fs.size
    img = Image.new("1", (width, height), 1)
    y = 4
    for spec, (ascent, descent, glyphs, bitmap) in results:
        table = {g[0]: g for g in glyphs}
        sample = "0123.45-6789" if spec.name == "hero" else f"{spec.name} PM2.5 12.4 µg/m³ CO₂ 612 24.3°C"
        x = 4
        y += ascent
        for ch in sample:
            g = table.get(ord(ch))
            if g is None:
                continue
            _, off, w, h, xo, yo, adv = g
            stride = (w + 7) // 8
            for r in range(h):
                for c in range(w):
                    if bitmap[off + r * stride + (c >> 3)] & (0x80 >> (c & 7)):
                        px, py = x + xo + c, y + yo + r
                        if 0 <= px < width and 0 <= py < height:
                            img.putpixel((px, py), 0)
            x += adv
        y += descent + 4
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when a committed fonts_<set>_gen.cpp differs")
    args = parser.parse_args()
    stale = []
    for fs in SETS:
        results = [(s, render(s)) for s in fs.specs]
        text = emit(fs, results)
        if args.check:
            if not fs.out.exists() or fs.out.read_text() != text:
                stale.append(str(fs.out.relative_to(ROOT)))
            continue
        fs.out.write_text(text)
        png = PREVIEW_DIR / f"fonts-{fs.name}.png"
        preview(fs, results, png)
        total = 0
        print(f"{fs.name}:")
        for spec, (ascent, descent, glyphs, bitmap) in results:
            size = len(bitmap) + len(glyphs) * 12
            total += size
            print(f"  {spec.name:6} {spec.px:3}px glyphs={len(glyphs):3} bitmap={len(bitmap):6} B "
                  f"ascent={ascent} descent={descent}")
        print(f"  total flash about {total} B, preview {png}")
    if args.check:
        if stale:
            raise SystemExit(f"stale, run pixi run epaper-fonts: {', '.join(stale)}")
        print("e-paper fonts match the generator")


if __name__ == "__main__":
    main()
