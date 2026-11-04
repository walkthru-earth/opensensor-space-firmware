# Shared sensor drivers

- One driver per part, header-only unless a parser needs a `.cpp`. Drivers are
  bus agnostic and borrow `*Io` callbacks (`context`, `read`, optional `write`
  and timing). They never create or configure an I2C/UART controller, take a
  bus lock or allocate heap memory.
- Return raw words plus explicit validity flags. Board policy such as battery
  presence, warm-up, staleness, sample cadence and field mapping stays in the
  board trial. No board names or pins here.
- Cite the datasheet and mark project heuristics as such. Every driver has a
  fake-bus host test under `tools/` that runs with ASan and UBSan through a
  Pixi task listed in `tools/AGENTS.md`.
- Each shared part also has a plugin manifest at `modules/<id>/module.json`
  (#17), validated against `docs/shared/plugins/v0.1.0/`. It names the
  columns, units and semantics a board pulls in through `board.json`
  segments. The checked-in `<id>_module.h` beside it is generated. Never edit
  it by hand, change the manifest and run `pixi run plugin-headers`.
  `plugin-contract-test` and `sensor-module-test` fail on a stale header.
- A board-only part keeps its manifest under the board's own `modules/`
  folder, not here. See `docs/shared/plugins/README.md`.
