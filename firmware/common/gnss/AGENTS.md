# Optional board-neutral GNSS components

Two components. `gnss/` (`src/os_gnss.*`) is pure, host-tested C++ with the
`Fix` type, the receiver dialects, the fix-to-H3 step and the row source rule.
`../gnss_reader/` (`src/os_gnss_reader.*`) is the reusable UART reader task.
The logger depends on `gnss` only. A board that has a receiver adds
`gnss_reader` to its own `REQUIRES`.

- No board names, no Kconfig board conditionals and no hardcoded GPIO or UART
  numbers here. A board passes them in `ReaderConfig`. `tools/test_gnss.py`
  checks this.
- A raw latitude or longitude lives only on the stack between `parse_fix()`
  and `to_fix()`. Never log, persist, queue or send a `Reading`. Only the H3
  res 8 cell, its resolution and the fix monotonic time leave this code (#139).
- On a moving Thing a phone cell (`location.phone_cell`, source 3) wins on
  every row while it is set (#140). The runtime clears it 300 s after the last
  SET of the key, so `select_location` never checks its age. Without it, a
  fix labels a row only while `fresh()` holds (`kFixMaxAgeUs`, 15 s), and a
  moving Thing without a fresh fix writes nulls. Never fall back to the
  configured cell or an older fix. A fixed Thing ignores GNSS and the phone
  cell and writes its configured cell with source 1.
- GNSS cells never enter `LocationSample.location`, so they never split a file.
- A new receiver family is a new `Dialect`, for example NMEA for u-blox. An AT
  dialect returns its commands, a streaming dialect returns empty commands.
  The logger and the source rule stay untouched.
- The reader task keeps fixed `std::array` line buffers, bounded timeouts and
  no heap after `start()`. Keep `ReaderConfig::reader_stack_bytes` at its
  default, `tools/stack_budget.py` tracks the `os-gnss` task by it.
- The power callback switches the receiver supply on and must never switch it
  off. Only read-only receiver queries plus the engine power-on command are
  sent. Never send network, SIM, band, NVM, CFUN or reset commands.
- GNSS UTC is not a clock anchor. A fix never sets UTC. #83 closed without it
  and no open issue tracks it yet.
