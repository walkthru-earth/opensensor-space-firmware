# Cairo 3.130

`CairoVariable.ttf` is unmodified `ofl/cairo/Cairo[slnt,wght].ttf` from
<https://github.com/google/fonts> at commit
`d2528f6d1f43e7d9d0d2e1794afe2ad6fd7d56ba`, renamed only. That build is
Cairo 3.130 from <https://github.com/Gue3bara/Cairo> commit
`73d16933c6a0f341c27a69e401da83dcb0d53114`.
It is licensed under the SIL Open Font License 1.1, see [`OFL.txt`](OFL.txt).
Copyright 2009 The Cairo Project Authors.

SHA-256 `667c987182391c91f4e57a2f455b1794fb5e3ee6ca4ef3383e86bb690fa9c964`

The firmware never links this file. `tools/gen_arabic_fonts.py` bakes glyph
bitmaps from it into `../../src/os_arabic_fonts_s_gen.cpp` and
`../../src/os_arabic_fonts_l_gen.cpp`, and `pixi run
text-fonts-test` fails when that output is stale. The bitmaps are a rendering
of the font and stay under the OFL.
