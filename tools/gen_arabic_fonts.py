"""Bake Cairo into 1 bit Arabic glyph tables for the shared text component.

Run with `pixi run text-fonts`. Reads the string table in
firmware/common/text/src/os_strings.inc, shapes every Egyptian Arabic string
the same way os_text.cpp does, and bakes exactly the presentation forms that
come out, plus the Arabic marks, punctuation and digits. Writes one source
per size class, firmware/common/text/src/os_arabic_fonts_s_gen.cpp for the
400 x 300 class and os_arabic_fonts_l_gen.cpp for 800 x 480, so an image
links only the sizes its font set names, and a preview PNG under the ignored
artifacts/text-fonts/. The output is committed, so a firmware build needs no
Python. `--check` fails when the committed output is stale.

Glyph rows are MSB first and padded to a byte, a set bit is black, the same
layout as the board fonts. Glyphs are rendered at 8 times the size and box
filtered down with unhinted advances, so the joining strokes of neighbouring
letters meet on the pixel grid.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageFont

ROOT = Path(__file__).resolve().parents[1]
TEXT = ROOT / "firmware/common/text"
TTF = TEXT / "vendor/cairo/CairoVariable.ttf"
STRINGS = TEXT / "src/os_strings.inc"
PREVIEW = ROOT / "artifacts/text-fonts/arabic.png"

SUPERSAMPLE = 8
SIZEOF_GLYPH = 16  # sizeof(opensensor::text::Glyph), checked by the host test.

# Baked in every size whatever the strings need. Arabic comma, semicolon and
# question mark, tatweel, the common marks from fathatan to sukun, the
# Arabic-Indic digits and the Arabic percent, decimal and thousands signs.
ALWAYS = (
    [0x060C, 0x061B, 0x061F, 0x0640]
    + list(range(0x064B, 0x0653))
    + list(range(0x0660, 0x066D))
)
MARKS = set(range(0x064B, 0x0660)) | {0x0670} | set(range(0x0610, 0x061B))
SUKUN = 0x0652


@dataclass
class Spec:
    name: str
    px: int
    weight: int
    threshold: int = 110  # Coverage above this is black.
    mark_threshold: int = 50  # Thin marks vanish at the normal threshold.


SPECS = [
    Spec("ar_body_s", 16, 600, threshold=100),
    Spec("ar_heading_s", 21, 700),
    Spec("ar_body_l", 26, 600),
    Spec("ar_heading_l", 34, 700),
]

# One generated source per size class, named by the spec suffix.
CLASSES = ("s", "l")


def out_path(size_class: str) -> Path:
    return TEXT / f"src/os_arabic_fonts_{size_class}_gen.cpp"

# ---- Strings and shaping, a port of os_text.cpp ---------------------------

ENTRY = re.compile(r'^OS_TEXT(_LTR)?\((\w+),\s*"([^"\\]*)"(?:,\s*"([^"\\]*)")?\)\s*$')


def read_strings() -> list[tuple[str, bool, str, str]]:
    """(id, keep_ltr, en, ar_EG) for every row of os_strings.inc."""
    rows = []
    for n, line in enumerate(STRINGS.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        m = ENTRY.match(line)
        if m is None:
            raise SystemExit(f"{STRINGS.relative_to(ROOT)}:{n}: cannot parse {line!r}")
        ltr, sid, en, ar = m.groups()
        keep_ltr = ltr is not None
        if keep_ltr == (ar is not None):
            raise SystemExit(f"{STRINGS.relative_to(ROOT)}:{n}: wrong argument count")
        rows.append((sid, keep_ltr, en, en if keep_ltr else ar))
    return rows


RIGHT = {0x0622, 0x0623, 0x0624, 0x0625, 0x0627, 0x0629, 0x062F, 0x0630,
         0x0631, 0x0632, 0x0648, 0x0649}
LAM_ALEF = {0x0622: 0xFEF5, 0x0623: 0xFEF7, 0x0625: 0xFEF9, 0x0627: 0xFEFB}


def _letters() -> dict[int, tuple[str, int]]:
    out = {}
    at = 0xFE80
    for c in range(0x0621, 0x064B):
        if 0x063B <= c <= 0x0640:
            continue
        join = "U" if c == 0x0621 else ("R" if c in RIGHT else "D")
        out[c] = (join, at)
        at += 1 if join == "U" else (2 if join == "R" else 4)
    return out


LETTERS = _letters()
# Cairo maps no isolated presentation form, the base letter is that glyph.
ISOLATED = {forms: c for c, (_, forms) in LETTERS.items()}


def is_mark(c: int) -> bool:
    return (0x0300 <= c <= 0x036F or 0x0591 <= c <= 0x05BD or 0x0610 <= c <= 0x061A
            or 0x064B <= c <= 0x065F or c == 0x0670 or 0x06D6 <= c <= 0x06DC
            or 0x06DF <= c <= 0x06E4 or 0x06E7 <= c <= 0x06E8
            or 0x06EA <= c <= 0x06ED or 0xFE20 <= c <= 0xFE2F)


def joining(c: int) -> str:
    if c == 0x0640:
        return "D"
    return LETTERS[c][0] if c in LETTERS else "U"


def shape(text: str) -> list[int]:
    t = [ord(ch) for ch in text]
    out: list[int] = []
    before = "U"
    i = 0
    while i < len(t):
        c = t[i]
        if is_mark(c):
            out.append(c)
            i += 1
            continue
        jt = joining(c)
        k = i + 1
        while k < len(t) and is_mark(t[k]):
            k += 1
        nxt = t[k] if k < len(t) else 0
        joins_before = jt != "U" and before == "D"
        if c == 0x0644 and nxt in LAM_ALEF:
            out.append(LAM_ALEF[nxt] + (1 if joins_before else 0))
            out.extend(t[i + 1:k])
            before = "R"
            i = k + 1
            continue
        before = jt
        if c not in LETTERS:
            out.append(c)
            i += 1
            continue
        joins_after = jt == "D" and nxt != 0 and joining(nxt) != "U"
        form = (3 if joins_after else 1) if joins_before else (2 if joins_after else 0)
        out.append(LETTERS[c][1] + form)
        i += 1
    return out


def arabic_block(c: int) -> bool:
    """Arabic and its presentation forms. Everything else, Latin, digits,
    units and subscripts, comes from the board's Latin fonts."""
    return 0x0600 <= c <= 0x06FF or 0xFB50 <= c <= 0xFDFF or 0xFE70 <= c <= 0xFEFF


def charset(rows) -> list[int]:
    need = set(ALWAYS)
    for _, keep_ltr, _, ar in rows:
        if not keep_ltr:
            need.update(c for c in shape(ar) if arabic_block(c))
    return sorted(need)


# ---- Baking ---------------------------------------------------------------


def load(px: int, weight: int) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(TTF), px, layout_engine=ImageFont.Layout.BASIC)
    values = []
    for axis in font.get_variation_axes():
        name = axis["name"].decode() if isinstance(axis["name"], bytes) else axis["name"]
        values.append(weight if "eight" in name else axis["default"])
    font.set_variation_by_axes(values)
    return font


def coverage(font: ImageFont.FreeTypeFont, ch: str):
    """The glyph's coverage at 1x, box filtered from the supersampled render,
    with the pen relative position of its top left pixel."""
    mask, (ox, oy) = font.getmask2(ch, mode="L", anchor="ls")
    w, h = mask.size
    if not w or not h:
        return None, 0, 0
    s = SUPERSAMPLE
    left, top = (ox // s) * s, (oy // s) * s
    cw = -(-(ox + w - left) // s) * s
    ch_ = -(-(oy + h - top) // s) * s
    canvas = Image.new("L", (cw, ch_), 0)
    canvas.paste(Image.frombytes("L", (w, h), bytes(mask)), (ox - left, oy - top))
    return canvas.reduce(s), left // s, top // s


def render(spec: Spec, cps: list[int]):
    big = load(spec.px * SUPERSAMPLE, spec.weight)
    notdef = bytes(big.getmask2("\uffff", mode="L", anchor="ls")[0])
    glyphs = []
    bitmap = bytearray()
    for cp in cps:
        ch = chr(ISOLATED.get(cp, cp))
        if bytes(big.getmask2(ch, mode="L", anchor="ls")[0]) == notdef:
            raise SystemExit(f"{spec.name}: U+{cp:04X} is missing from Cairo")
        mark = cp in MARKS
        advance = 0 if mark else round(big.getlength(ch) / SUPERSAMPLE)
        cut = spec.mark_threshold if mark and cp != SUKUN else spec.threshold
        img, px0, py0 = coverage(big, ch)
        bbox = None
        if img is not None:
            bbox = img.point(lambda v, cut=cut: 255 if v > cut else 0).getbbox()
        if bbox is None or img is None:
            raise SystemExit(f"{spec.name}: U+{cp:04X} has no ink at {spec.px} px")
        x0, y0, x1, y1 = bbox
        stride = (x1 - x0 + 7) // 8
        start = len(bitmap)
        for y in range(y0, y1):
            row = bytearray(stride)
            for x in range(x0, x1):
                if img.getpixel((x, y)) > cut:
                    row[(x - x0) >> 3] |= 0x80 >> ((x - x0) & 7)
            bitmap += row
        glyphs.append((cp, start, x1 - x0, y1 - y0, px0 + x0, py0 + y0, advance))
    # Ascent and descent are the ink extents of the baked set.
    ascent = max(-yo for _, _, _, _, _, yo, _ in glyphs)
    descent = max(yo + h for _, _, _, h, _, yo, _ in glyphs)
    return ascent, descent, glyphs, bitmap


def emit(results) -> str:
    out = [
        "// Generated by tools/gen_arabic_fonts.py from Cairo 3.130 (SIL OFL 1.1).",
        "// Do not edit, run `pixi run text-fonts`.",
        "// clang-format off",
        '#include "os_text_font.h"',
        "",
        "namespace opensensor::text::fonts {",
        "namespace {",
    ]
    for spec, (_, _, glyphs, bitmap) in results:
        out.append(f"constexpr std::uint8_t k_{spec.name}_bits[] = {{")
        for i in range(0, len(bitmap), 20):
            out.append("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i : i + 20]) + ",")
        out.append("};")
        out.append(f"constexpr Glyph k_{spec.name}_glyphs[] = {{")
        for cp, off, w, h, xo, yo, adv in glyphs:
            out.append(f"    {{0x{cp:04x}, {off}, {w}, {h}, {xo}, {yo}, {adv}}},")
        out.append("};")
    out.append("}  // namespace")
    out.append("")
    for spec, (ascent, descent, _, _) in results:
        out.append(
            f"const Font {spec.name}{{k_{spec.name}_glyphs, k_{spec.name}_bits, "
            f"{ascent}, {descent}, nullptr}};"
        )
    out.append("}  // namespace opensensor::text::fonts")
    out.append("// clang-format on")
    out.append("")
    return "\n".join(out)


def visual(text: str) -> list[int]:
    """Shaped and reversed, enough for an all Arabic preview line."""
    return list(reversed(shape(text)))


def preview(results, rows, path: Path) -> None:
    ar = [r[3] for r in rows if not r[1] and not any(ch.isascii() and ch.isalnum() for ch in r[3])]
    img = Image.new("1", (800, 480), 1)
    y = 4
    for spec, (ascent, descent, glyphs, bitmap) in results:
        table = {g[0]: g for g in glyphs}
        for sample in ar[:3]:
            y += ascent
            x = 796
            line = visual(sample)
            width = sum(table[c][6] if c in table else spec.px // 3 for c in line)
            x -= width
            for c in line:
                g = table.get(c)
                if g is None:
                    x += spec.px // 3
                    continue
                _, off, w, h, xo, yo, adv = g
                stride = (w + 7) // 8
                for r in range(h):
                    for col in range(w):
                        if bitmap[off + r * stride + (col >> 3)] & (0x80 >> (col & 7)):
                            px, py = x + xo + col, y + yo + r
                            if 0 <= px < 800 and 0 <= py < 480:
                                img.putpixel((px, py), 0)
                x += adv
            y += descent + 2
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when a committed os_arabic_fonts_*_gen.cpp differs")
    args = parser.parse_args()
    rows = read_strings()
    cps = charset(rows)
    results = [(s, render(s, cps)) for s in SPECS]
    outputs = {
        out_path(c): emit([r for r in results if r[0].name.endswith(f"_{c}")])
        for c in CLASSES
    }
    if args.check:
        for path, text in outputs.items():
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                raise SystemExit(f"{path.relative_to(ROOT)} is stale, run pixi run text-fonts")
        print(f"arabic fonts match the generator, {len(cps)} glyphs per size")
        return
    for path, text in outputs.items():
        path.write_text(text, encoding="utf-8")
    preview(results, rows, PREVIEW)
    total = 0
    for spec, (ascent, descent, glyphs, bitmap) in results:
        size = len(bitmap) + len(glyphs) * SIZEOF_GLYPH
        total += size
        print(f"{spec.name:12} {spec.px:3}px wght={spec.weight} glyphs={len(glyphs):3} "
              f"bitmap={len(bitmap):6} B flash={size:6} B ascent={ascent} descent={descent}")
    print(f"total flash {total} B, preview {PREVIEW.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
