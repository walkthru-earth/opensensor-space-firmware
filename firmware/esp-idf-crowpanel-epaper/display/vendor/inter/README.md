# Inter 4.1

`InterVariable.ttf` is unmodified from the Inter 4.1 release,
<https://github.com/rsms/inter/releases/tag/v4.1>, file `Inter-4.1.zip`.
It is licensed under the SIL Open Font License 1.1, see [`OFL.txt`](OFL.txt).
Copyright (c) 2016 The Inter Project Authors.

SHA-256 `4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf`

The firmware never links this file. `tools/crowpanel_gen_fonts.py` bakes
glyph bitmaps from it into `../../fonts_gen.cpp`, and `pixi run
crowpanel-fonts-test` fails when that output is stale. The bitmaps are a
rendering of the font and stay under the OFL.
