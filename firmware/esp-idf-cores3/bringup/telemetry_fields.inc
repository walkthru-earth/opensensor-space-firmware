// FIELD(name, physical_type, procedure, unit, validity, live_key, decimals).
// live_key is the short LIVE JSON key, or "" for fields that stay out of
// LIVE. decimals is the number of digits after the point when a Float prints,
// chosen from the sensor resolution, and 0 for integer fields. Both are part
// of the digest, so the app keeps one LIVE profile per dictionary SHA-256.
// Parquet/Iceberg field_id = 1-based order of the FIELD entries below. The
// Thing release (#83) is the baseline. From it, append new fields only and
// never reorder, remove or reuse a position (owner answer to bridge question
// 36, 2026-10-26).
// The property id of each field comes from telemetry_semantics.inc, board
// neutral, looked up by name (#99).
// Procedure identifiers describe code paths, not instrument serial numbers.
// Float decimals. BMI270 accel steps about 1/4096 g at 8 g, 3 is enough. Gyro
// steps about 0.061 deg/s at 2000 deg/s, 2 is enough. The IMU die temperature is
// not ambient air, so 1. SHT20 temperature steps 0.01 degC, so 2, and humidity
// about 0.04 percent, 1 is enough.
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
FIELD(pms_status, Int32, pmsa003, "code", always, "pms", 0)
FIELD(pms_age_ms, Int64, pmsa003, "ms", pms_present, "", 0)
FIELD(pm1_cf1_ug_m3, Int32, pmsa003, "ug/m3", pms_valid, "c1", 0)
FIELD(pm25_cf1_ug_m3, Int32, pmsa003, "ug/m3", pms_valid, "c25", 0)
FIELD(pm10_cf1_ug_m3, Int32, pmsa003, "ug/m3", pms_valid, "c10", 0)
FIELD(pm1_atmospheric_ug_m3, Int32, pmsa003, "ug/m3", pms_valid, "pm1", 0)
FIELD(pm25_atmospheric_ug_m3, Int32, pmsa003, "ug/m3", pms_valid, "pm25", 0)
FIELD(pm10_atmospheric_ug_m3, Int32, pmsa003, "ug/m3", pms_valid, "pm10", 0)
FIELD(particles_gt03_per_01l, Int32, pmsa003, "count/0.1L", pms_valid, "n03", 0)
FIELD(particles_gt05_per_01l, Int32, pmsa003, "count/0.1L", pms_valid, "n05", 0)
FIELD(particles_gt10_per_01l, Int32, pmsa003, "count/0.1L", pms_valid, "n1", 0)
FIELD(particles_gt25_per_01l, Int32, pmsa003, "count/0.1L", pms_valid, "n25", 0)
FIELD(particles_gt50_per_01l, Int32, pmsa003, "count/0.1L", pms_valid, "n5", 0)
FIELD(particles_gt100_per_01l, Int32, pmsa003, "count/0.1L", pms_valid, "n10", 0)
FIELD(pms_firmware, Int32, pmsa003, "code", pms_present, "", 0)
FIELD(pms_error, Int32, pmsa003, "code", pms_present, "", 0)
FIELD(pms_frames, Int64, pmsa003, "count", always, "", 0)
FIELD(pms_checksum_errors, Int64, pmsa003, "count", always, "", 0)
FIELD(pms_length_errors, Int64, pmsa003, "count", always, "", 0)
FIELD(imu_fresh_mask, Int32, m5_imu, "bitmask", always, "", 0)
FIELD(accel_x_g, Float, m5_imu, "g", accel_fresh_finite, "", 3)
FIELD(accel_y_g, Float, m5_imu, "g", accel_fresh_finite, "", 3)
FIELD(accel_z_g, Float, m5_imu, "g", accel_fresh_finite, "", 3)
FIELD(gyro_x_dps, Float, m5_imu, "deg/s", gyro_fresh_finite, "", 2)
FIELD(gyro_y_dps, Float, m5_imu, "deg/s", gyro_fresh_finite, "", 2)
FIELD(gyro_z_dps, Float, m5_imu, "deg/s", gyro_fresh_finite, "", 2)
FIELD(mag_x_raw, Int32, m5_imu_aux, "raw_count", mag_fresh, "", 0)
FIELD(mag_y_raw, Int32, m5_imu_aux, "raw_count", mag_fresh, "", 0)
FIELD(mag_z_raw, Int32, m5_imu_aux, "raw_count", mag_fresh, "", 0)
FIELD(imu_temperature_c, Float, m5_imu, "degC", imu_temperature_read_finite, "imu_t", 1)
FIELD(light_ch0_raw, Int32, ltr553, "raw_count", light_valid, "als", 0)
FIELD(light_ch1_raw, Int32, ltr553, "raw_count", light_valid, "", 0)
FIELD(proximity_raw, Int32, ltr553, "raw_count", proximity_valid, "", 0)
FIELD(light_status, Int32, ltr553, "code", always, "lst", 0)
FIELD(proximity_status, Int32, ltr553, "code", always, "", 0)
FIELD(vbus_mv, Int32, m5_power, "mV", api_report_unqualified, "vbus", 0)
FIELD(battery_mv, Int32, m5_power, "mV", api_report_unqualified, "bat", 0)
FIELD(battery_percent, Int32, m5_power, "%", battery_range_0_100, "pct", 0)
FIELD(charging_status, Int32, m5_power, "code", api_report_unqualified, "chg", 0)
FIELD(battery_current_ma, Int32, m5_power, "mA", unavailable, "", 0)
FIELD(external_5v_enabled, Int32, m5_power, "boolean", api_report_unqualified, "", 0)
FIELD(usb_output_enabled, Int32, m5_power, "boolean", api_report_unqualified, "", 0)
FIELD(rtc_date_yyyymmdd, Int32, m5_rtc, "YYYYMMDD", rtc_read_ok, "", 0)
FIELD(rtc_time_hhmmss, Int32, m5_rtc, "hhmmss", rtc_read_ok, "", 0)
FIELD(rtc_read_ok, Int32, m5_rtc, "boolean", always, "", 0)
FIELD(touch_points, Int32, m5_touch, "count", always, "", 0)
FIELD(touch_x_px, Int32, m5_touch, "pixel", touch_present, "", 0)
FIELD(touch_y_px, Int32, m5_touch, "pixel", touch_present, "", 0)
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
FIELD(ambient_temperature_c, Float, sht20_unresolved, "degC", unavailable, "t", 2)
FIELD(relative_humidity_percent, Float, sht20_unresolved, "%", unavailable, "rh", 1)
FIELD(clock_epoch, Int32, clock, "count", always, "", 0)
FIELD(collection_completed_mono_us, Int64, clock, "us", always, "", 0)
FIELD(pms_received_mono_us, Int64, pmsa003, "us", pms_present, "", 0)
FIELD(clock_anchor_mono_us, Int64, clock, "us", clock_anchored, "", 0)
FIELD(clock_anchor_utc_us, Int64, clock, "us_since_unix_epoch", clock_anchored, "", 0)

// Optional owner-provisioned H3 cell. location_source says where it came from
// and geometry is a native Parquet GEOMETRY 2D WKB Point at the cell centre,
// never the raw fix. All four are null when there is no location.
FIELD(h3_cell_id, Int64, h3_grid, "h3_index", location_present, "", 0)
FIELD(h3_resolution, Int32, h3_grid, "resolution_0_15", location_present, "", 0)
FIELD(location_source, Int32, h3_grid, "uint8_code", location_present, "", 0)
FIELD(geometry, ByteArray, h3_grid_center, "wkb_point_crs84", location_present, "", 0)
