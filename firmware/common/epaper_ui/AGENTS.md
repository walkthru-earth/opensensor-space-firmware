# Shared e-paper UI

Board-neutral pages, charts, fonts, the settings popup, rotation and the
refresh policy for every e-paper Thing. Pure C++23 with no ESP-IDF API, so
`pixi run epaper-ui-test` compiles the same sources the boards link. One UI
change here reaches every e-paper board.

## What a board supplies

- A `static constexpr ui::DisplayProfile` next to its driver (`profile.h`).
  Width and height of the glass, `ColorDepth` (Mono1 only renders),
  `RefreshCaps` (fast, partial, partial window, partials per full), the
  `FontScale`, the landscape and portrait `Layout` tables, the mount
  rotation and an `OrientationSource`. `validate()` refuses a profile the
  UI cannot draw. No board name and no pin lives here.
- A driver that meets the `epd::Panel` concept (`panel.h`),
  `show(span frame, Refresh, Rect window)` returning
  `std::expected<void, epd::Error>` plus `sleep()`. A concept, not a
  virtual base. The frame convention is in `panel.h`. A driver refuses a
  mode it lacks with `Unsupported`.
- Three PSRAM buffers for `ui::Presenter::create()`, the logical frame
  (`logical_frame_bytes()`) and two panel frames (`frame_bytes()`).

## The draw loop

`presenter.canvas(rotation)`, then `render_screen(...)`, then `compose()`
(false means the glass already shows it), then `plan()` and
`present(panel, plan)`. `RefreshPolicy` never asks for a mode the caps lack.
The first and every forced update are Full, a page change is Fast where the
panel has it, else Full. The rest are Partial, and after
`partials_per_full` of those the next is Full.

## Size classes

`layout_400x300` and `layout_300x400` use the Compact fonts,
`layout_800x480` and `layout_480x800` the Large ones. Each table and each
font set is its own source, so `--gc-sections` keeps only what a profile
names. A new size class is a new layout source plus host profile in
`tools/epaper_ui_host_profiles.h`, tuned against `pixi run epaper-ui-preview`.

## Rotation

- Pages draw a logical frame, `rotate_frame()` turns it into the panel frame.
  R0 is a plain copy. A rotation other than the one on the glass forces Full.
- `effective_rotation()` is the user setting on top of the mount. Keys keep
  their logical meaning at every rotation.
- Auto exists only when `OrientationSource::present`. The board reads its
  accelerometer and feeds `OrientationTracker::update()` one `Gravity` per
  poll from the display task. `SensorAxes` maps sensor axes to the panel.
  A turn needs about 2 s of stability past a 15 degree hysteresis, and a
  flat device or no reading keeps the rotation. Auto ignores the mount.

## Settings blob

NVS `ospanel/ui`, version 3, 13 bytes, layout in `settings.h`. Byte 11 is
the rotation and byte 12 the language (0 English, 1 Arabic). Version 2 (12
bytes, no language) and version 1 (11 bytes, no rotation either) decode with
every field they hold kept and the rest at their defaults, English for the
language, and the next save writes version 3. Never erase it. Going back to
an image that knows only an older version gives defaults.

## Goldens

`tools/test_epaper_ui.py` holds the sha256 of every English 400x300 r0
frame, the frames the CrowPanel sends, and of four Arabic ones under
`ar/400x300/r0` (dashboard, status, chart and settings). The preview writes
every frame in both languages, English at `<size>/r<deg>` and Arabic at
`ar/<size>/r<deg>`. The CrowPanel's `display_profile.h` must equal
`kHost400x300`, the host test checks it. A golden moves only with a
deliberate change to what the CrowPanel shows. Look at the new PNG first,
then paste the hash the failure prints, and say so in the PR.

## Text and i18n

- Every drawn word is a `Str` id, which is `text::StringId` from the one
  string table `firmware/common/text/src/os_strings.inc`, English and
  Egyptian Arabic. Add or change a word there, then run
  `pixi run text-fonts` so the Cairo tables hold its glyphs. Rules for the
  table are in `firmware/common/text/AGENTS.md`.
- `Settings::language` picks the language and the popup's Language row
  toggles it with a full refresh. `render_screen()` calls `set_language()`
  first, so `tr()` and `tr_fmt()` answer in it. Only the display task calls
  `set_language()`. A board that draws outside `render_screen()` calls it
  itself. The row value names each language in itself.
- `shape()` in `canvas.h` sends Arabic lines through `text::to_visual()`,
  which joins the letters and reorders the line, and draws each code point
  from the Latin font or its Cairo fallback (`_s` for Compact, `_l` for
  Large). The hero font has no fallback, it draws digits only.
- In Arabic the pages mirror, text right aligned and labels on the right of
  the popup, the status rows, the tiles and the chart heading. Plots, the
  time axis, the header and the hero stay as they are. A value and its unit
  are their own left to right runs. Use `text_fit()` and `mx()` in
  `pages.cpp` for anything new.
- `epaper-ui-test` checks every id in both languages, that every code point
  an Arabic string draws is baked, and that the Arabic fits the popup, the
  hints, the footnotes, the status names and the tile labels at 400x300,
  300x400, 800x480 and 480x800. Look at the Arabic previews after a string
  change.
- No colons, semicolons or em dashes in panel copy, in either language.

## Fonts

`tools/epaper_gen_fonts.py` bakes `src/fonts_compact_gen.cpp` and
`src/fonts_large_gen.cpp` from the vendored Inter in `vendor/inter/`
(SIL OFL 1.1). Regenerate with `pixi run epaper-fonts`, and
`epaper-fonts-test` fails on stale output. The Arabic glyphs come from the
`text` component, so a board that links this one adds `../common/text` to
its `EXTRA_COMPONENT_DIRS`.

## Rules

- No heap. The canvas, history and presenter work in buffers the board
  allocates once. No `std::format`, keep `snprintf`. Never pass a
  `string_view.data()` to `%s`.
- New sources go in `CMakeLists.txt`, `tools/lint.sh` and are picked up by
  the host test from `src/`.
