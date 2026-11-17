// FIELD(name, physical_type, procedure, unit, validity, live_key, decimals).
// Written once from firmware/esp-idf-crowpanel-epaper/logger/board.json by `pixi run plugin-new-board`.
// Hand kept from then on. The plugin gate compares every row with the
// manifests, and nothing rewrites this file.
// live_key is the short LIVE JSON key, or "" for fields that stay out of
// LIVE. decimals is the number of digits after the point when a Float
// prints, and 0 for integer fields. Both are part of the dict digest.
// Parquet/Iceberg field_id = 1-based order of the FIELD entries below.
// Append new fields only, never reorder, remove or reuse a position.
FIELD(schema_version, Int32, logger, "1", always, "", 0)
FIELD(device_id, Int64, identity, "1", always, "", 0)
FIELD(boot_id_hi, Int64, identity, "1", always, "", 0)
FIELD(boot_id_lo, Int64, identity, "1", always, "", 0)
FIELD(sequence, Int64, logger, "count", always, "seq", 0)
FIELD(monotonic_us, Int64, clock, "us", always, "mono", 0)
FIELD(scheduled_us, Int64, clock, "us", always, "", 0)
FIELD(sample_jitter_us, Int64, clock, "us", always, "", 0)
FIELD(event_time_utc_us, Int64, clock, "us_since_unix_epoch", clock_anchored, "utc", 0)
FIELD(clock_status, Int32, clock, "code", always, "", 0)
FIELD(sen6x_status, Int32, sen63c, "code", always, "sen", 0)
FIELD(sen6x_device_status, Int64, sen63c, "bitmask", sen6x_present, "", 0)
FIELD(pm1_mass_ug_m3, Float, sen63c, "ug/m3", sen6x_valid, "m1", 1)
FIELD(pm25_mass_ug_m3, Float, sen63c, "ug/m3", sen6x_valid, "m25", 1)
FIELD(pm4_mass_ug_m3, Float, sen63c, "ug/m3", sen6x_valid, "m4", 1)
FIELD(pm10_mass_ug_m3, Float, sen63c, "ug/m3", sen6x_valid, "m10", 1)
FIELD(ambient_temperature_c, Float, sen63c, "degC", sen6x_present, "t", 1)
FIELD(relative_humidity_percent, Float, sen63c, "%", sen6x_present, "rh", 1)
FIELD(co2_ppm, Int32, sen63c, "ppm", sen6x_valid, "co2", 0)
FIELD(heap_free_bytes, Int64, esp_runtime, "byte", always, "", 0)
FIELD(heap_min_free_bytes, Int64, esp_runtime, "byte", always, "", 0)
FIELD(psram_free_bytes, Int64, esp_runtime, "byte", always, "", 0)
FIELD(sd_total_bytes, Int64, storage, "byte", sd_mounted_cached, "", 0)
FIELD(sd_used_bytes, Int64, storage, "byte", sd_mounted_cached, "", 0)
FIELD(rows_dropped, Int64, logger, "count", always, "", 0)
FIELD(sample_deadlines_missed, Int64, logger, "count", always, "", 0)
FIELD(storage_errors, Int64, storage, "count", always, "", 0)
FIELD(files_finalized, Int64, storage, "count", always, "", 0)
FIELD(last_write_us, Int64, storage, "us", always, "", 0)
FIELD(last_sync_us, Int64, storage, "us", always, "", 0)
FIELD(queue_high_water, Int32, logger, "count", always, "", 0)
FIELD(clock_epoch, Int32, clock, "count", always, "", 0)
FIELD(collection_completed_mono_us, Int64, clock, "us", always, "", 0)
FIELD(clock_anchor_mono_us, Int64, clock, "us", clock_anchored, "", 0)
FIELD(clock_anchor_utc_us, Int64, clock, "us_since_unix_epoch", clock_anchored, "", 0)
FIELD(h3_cell_id, Int64, h3_grid, "h3_index", location_present, "", 0)
FIELD(h3_resolution, Int32, h3_grid, "resolution_0_15", location_present, "", 0)
FIELD(location_source, Int32, h3_grid, "uint8_code", location_present, "", 0)
FIELD(geometry, ByteArray, h3_grid_center, "wkb_point_crs84", location_present, "", 0)
