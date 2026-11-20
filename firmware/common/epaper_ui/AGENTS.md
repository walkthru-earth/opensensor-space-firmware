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

NVS `ospanel/ui`, version 2, 12 bytes, layout in `settings.h`. Version 1
(11 bytes, no rotation) decodes with every field kept and rotation at its
default, and the next save writes version 2. Never erase it. Going back to
an image that knows only version 1 gives defaults.

## Goldens

`tools/test_epaper_ui.py` holds the sha256 of every 400x300 r0 frame, the
frames the CrowPanel sends. The CrowPanel's `display_profile.h` must equal
`kHost400x300`, the host test checks it. A golden moves only with a
deliberate change to what the CrowPanel shows. Look at the new PNG first,
then paste the hash the failure prints, and say so in the PR.

## Text and i18n

Every drawn word is a `Str` id in `strings.h`, English only. `tr()` gives
a `string_view`, `tr_fmt()` a C string for `snprintf` format ids. A
translation is a second table of the same ids plus its glyphs in the font
sets. Right to left or joined scripts also change `shape()` in `canvas.h`.
Nothing translates yet. No colons, semicolons or em dashes in panel copy.

## Fonts

`tools/epaper_gen_fonts.py` bakes `src/fonts_compact_gen.cpp` and
`src/fonts_large_gen.cpp` from the vendored Inter in `vendor/inter/`
(SIL OFL 1.1). Regenerate with `pixi run epaper-fonts`, and
`epaper-fonts-test` fails on stale output.

## Rules

- No heap. The canvas, history and presenter work in buffers the board
  allocates once. No `std::format`, keep `snprintf`. Never pass a
  `string_view.data()` to `%s`.
- New sources go in `CMakeLists.txt`, `tools/lint.sh` and are picked up by
  the host test from `src/`.
