// CLASS(column, value, name, nodata).
// The classification:classes of each code and boolean column in the board
// template (#85), one row per value, grouped by column in value order. value
// is the enum or bool the code writes, so a renumbered code moves the table
// with it. nodata marks the value that means no reading.
// This file is NOT part of the dict digest, which hashes telemetry_fields.inc
// only. The static_asserts in telemetry_contract.h fail the build when a code
// or boolean column has no classes, when a boolean lacks its 0 or 1 row, when
// a row names an unknown column, and when a status legend or the board's
// clock sources list other pairs.
//
// Sources.
//   clock_status rows are exactly kClockSources in telemetry_contract.h, the
//   sources this board writes, named by opensensor::utc::source_name.
//   pms_status, light_status and proximity_status names are the status
//   legends in telemetry_contract.h, which collect() writes through the
//   enums named here.
//   charging_status is ChargingStatus, which charging_status_of in
//   telemetry_logger.cpp maps from M5Unified charge_state_t and checks by
//   static_assert, and its rows must equal kChargingStatusLegend.
//   external_5v_enabled, usb_output_enabled and rtc_read_ok are the bools
//   collect() writes.
//   location_source names are telemetry::LocationSource, the stac-opensensor
//   profile codes the shared logger writes in apply_cell.
// No classes.
//   pms_firmware and pms_error are raw PMSA003 frame bytes with no published
//   code list. imu_fresh_mask is an M5Unified sensor bitmask, not a code.
//   kClasslessColumns in telemetry_contract.h lists them.
CLASS(clock_status, kClockNone, "none", false)
CLASS(clock_status, kClockHost, "host", false)
CLASS(clock_status, kClockRtc, "rtc", false)
CLASS(clock_status, kClockNetwork, "network", false)
CLASS(pms_status, kPmsMissing, "missing", true)
CLASS(pms_status, kPmsWarming, "warming", false)
CLASS(pms_status, kPmsStale, "stale", false)
CLASS(pms_status, kPmsSensorError, "sensor-error", false)
CLASS(pms_status, kPmsValid, "valid", false)
CLASS(light_status, kLightUnavailable, "unavailable", true)
CLASS(light_status, kLightNotFresh, "not-fresh", false)
CLASS(light_status, kLightInvalid, "invalid", false)
CLASS(light_status, kLightValid, "valid", false)
CLASS(light_status, kLightIoError, "io-error", false)
CLASS(proximity_status, kProximityUnavailable, "unavailable", true)
CLASS(proximity_status, kProximityNotFresh, "not-fresh", false)
CLASS(proximity_status, kProximitySaturated, "saturated", false)
CLASS(proximity_status, kProximityValid, "valid", false)
CLASS(proximity_status, kProximityIoError, "io-error", false)
CLASS(charging_status, kChargingDischarging, "discharging", false)
CLASS(charging_status, kChargingCharging, "charging", false)
CLASS(charging_status, kChargingUnknown, "unknown", true)
CLASS(external_5v_enabled, false, "disabled", false)
CLASS(external_5v_enabled, true, "enabled", false)
CLASS(usb_output_enabled, false, "disabled", false)
CLASS(usb_output_enabled, true, "enabled", false)
CLASS(rtc_read_ok, false, "failed", false)
CLASS(rtc_read_ok, true, "ok", false)
CLASS(location_source, telemetry::LocationSource::None, "none", true)
CLASS(location_source, telemetry::LocationSource::Configured, "configured", false)
CLASS(location_source, telemetry::LocationSource::Gnss, "gnss", false)
CLASS(location_source, telemetry::LocationSource::Phone, "phone", false)
