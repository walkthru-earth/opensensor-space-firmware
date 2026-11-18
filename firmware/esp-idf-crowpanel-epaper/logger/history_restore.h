#pragma once

// Seeds the chart history at boot from this Thing's own finalized Parquet
// files of earlier boots, so a reset does not leave the 2 h and 24 h charts
// empty. Read only, it opens NVS "parquet" with NVS_READONLY and reads the
// card, never writes, renames or deletes anything.
//
// Where rows come from. Unsynced files of the boots in the NVS boot ring and
// dated files in the two newest day folders. A ".partial" (a power cut
// before finalize) and every other non ".parquet" entry are skipped.
//
// Boot order. Monotonic time restarts at 0 each boot and the board has no
// RTC, so the proven cross boot order on the card is the ring (append order
// of boots whose first file was unsynced) and the UTC in dated folder and
// file names. Ring boots keep ring order, dated boots keep UTC order, and a
// boot that is in both bridges the two. Where that proves nothing, the
// sd_used_bytes footer bounds decide, since card usage only grows while the
// firmware never deletes files. A boot that still cannot be placed ends the
// restore there, older boots stay out. A ring boot with no finalized file
// is left out. The pure ordering is in restore_order.h.
//
// Budget. At most 24 h of rows in total, newest boot first, each boot's
// newest rows first, a restart point between boots. About 8 s at most, the
// rows read by then are kept. Every failure only leaves less history, it
// never stops logging.
//
// Call once from the board task after the card is mounted and before
// oslogger::begin(), so no storage worker shares the card yet, and before the
// UI task that owns `history` starts. Logs one "OS HISTORY" line.

#include "history.h"

namespace opensensor::crowpanel {

void restore_history(ui::History &history, const char *output_directory);

} // namespace opensensor::crowpanel
