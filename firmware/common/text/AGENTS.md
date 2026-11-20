# Shared text and i18n

- `os_text` is pure C++ with no IO, clocks, heap, locale or board names. It
  decodes UTF-8, shapes Arabic to Presentation Forms-B and reorders one line
  into visual order, all inside a caller span. A board formats with
  `snprintf` first, then calls `to_visual` once per drawn line and draws the
  code points left to right.
- Malformed UTF-8 is rejected by default. `OnMalformed::Replace` writes one
  U+FFFD per maximal invalid subpart. Errors are `Error::Malformed` and
  `Error::Overflow`, never a silent cut.
- Every letter of U+0621 to U+064A comes out as a presentation form, the
  isolated one too, so a font only needs FE80 to FEFC plus marks, tatweel,
  punctuation and digits. Lam-alef is one ligature and marks are transparent
  to joining. Persian and Urdu letters past U+064A pass through unshaped.
- The bidi pass is a reduced UAX #9 for UI labels, levels 0 to 2 with weak
  and neutral rules, trailing white space, combining marks and bracket
  mirroring. It has no embeddings, isolates or bracket pairing. A number
  after Arabic is an Arabic number, so a sign or unit after it, like `°C`, can
  land on the wrong side. Draw a value and its unit as their own left to
  right run next to an Arabic label.
- `kMaxLine` bounds the bidi scratch, two bytes per code point on the stack.
  Raise it only with a stack measurement on the task that draws.
- `os_strings.inc` is the single string table. English must match the board
  pages word for word. `OS_TEXT_LTR` rows are clocks, units, metric names and
  number formats that stay left to right in every language. Every printf
  conversion appears in both languages in the same order, and the host test
  checks it. Units, sensor names and protocol names stay in Latin script.
- Never pass `text()` to `%s`, use `c_str()`, which is a NUL terminated
  literal.
- `os_arabic_fonts_gen.cpp` is generated from the vendored Cairo by
  `pixi run text-fonts`. Never edit it by hand. After changing an Arabic
  string run that task and commit both files. `pixi run text-fonts-test`
  fails when the output is stale, and `pixi run text-test` fails when a
  shaped string needs a glyph that is not baked.
- Glyph tables use the board bitmap layout, rows MSB first, padded to a byte,
  set bit black. Fonts cost flash only in an image that links this component.
- Cairo stays unmodified under `vendor/cairo/` with its OFL and is listed in
  `THIRD_PARTY_NOTICES.md`. A new version updates the README hash, the
  notices row and the baked tables in one change.
- Host gate is `pixi run text-test` with ASan and UBSan, and
  `pixi run text-fonts-test`.
