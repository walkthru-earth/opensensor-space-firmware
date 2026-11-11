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
//   pms_status, pm25_batch_candidate_status, gauge_status and
//   battery_health_state names are the status legends in
//   telemetry_contract.h. The modules in sensor_modules.h write them through
//   generated status constants equal to the enums named here.
//   gauge_hibernating is the bool MAX17048 hibernating() returns.
//   location_source names are telemetry::LocationSource, the stac-opensensor
//   profile codes the shared logger writes in apply_cell.
// No classes.
//   pms_firmware and pms_error are raw PMS5003T frame bytes with no published
//   code list. gauge_status_flags is the MAX17048 STATUS bitmask, not a code.
//   kClasslessColumns in telemetry_contract.h lists them.
CLASS(clock_status, kClockNone, "none", false)
CLASS(clock_status, kClockHost, "host", false)
CLASS(clock_status, kClockNetwork, "network", false)
CLASS(pms_status, kPmsMissing, "missing", true)
CLASS(pms_status, kPmsWarming, "warming", false)
CLASS(pms_status, kPmsStale, "stale", false)
CLASS(pms_status, kPmsSensorError, "sensor-error", false)
CLASS(pms_status, kPmsValid, "valid", false)
CLASS(pms_status, kPmsModelMismatch, "model-mismatch", false)
CLASS(pms_status, kPmsSleeping, "sleeping", false)
CLASS(pm25_batch_candidate_status, kBatchDisabled, "disabled", false)
CLASS(pm25_batch_candidate_status, kBatchIdentityMismatch, "identity-mismatch",
      false)
CLASS(pm25_batch_candidate_status, kBatchSourceUnavailable, "PMS-unavailable",
      false)
CLASS(pm25_batch_candidate_status, kBatchCandidate, "candidate-only", false)
CLASS(gauge_status, kGaugeNotInstalled, "battery-not-installed-or-disabled",
      true)
CLASS(gauge_status, kGaugeReadError, "read-error-or-implausible", false)
CLASS(gauge_status, kGaugeValid, "valid", false)
CLASS(gauge_hibernating, false, "active", false)
CLASS(gauge_hibernating, true, "hibernating", false)
CLASS(battery_health_state, opensensor::battery::State::Unknown, "unknown", true)
CLASS(battery_health_state, opensensor::battery::State::ExternalPowerLikely,
      "external-power-likely", false)
CLASS(battery_health_state, opensensor::battery::State::Ok, "ok", false)
CLASS(battery_health_state, opensensor::battery::State::Low, "low", false)
CLASS(battery_health_state, opensensor::battery::State::Critical, "critical", false)
CLASS(battery_health_state, opensensor::battery::State::Settling, "settling", false)
CLASS(location_source, telemetry::LocationSource::None, "none", true)
CLASS(location_source, telemetry::LocationSource::Configured, "configured", false)
CLASS(location_source, telemetry::LocationSource::Gnss, "gnss", false)
CLASS(location_source, telemetry::LocationSource::Phone, "phone", false)
