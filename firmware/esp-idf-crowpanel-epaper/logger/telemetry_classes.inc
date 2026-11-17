// CLASS(column, value, name, nodata).
// Written once from firmware/esp-idf-crowpanel-epaper/logger/board.json by `pixi run plugin-new-board`.
// Hand kept from then on. The plugin gate compares every row with the
// manifests, and nothing rewrites this file.
// One row per value of each code and boolean column, grouped by column
// in value order. value is the manifest value. The board code may name
// it by its enum later, the gate compares names and nodata only.
CLASS(clock_status, 0, "none", false)
CLASS(clock_status, 1, "host", false)
CLASS(clock_status, 3, "network", false)
CLASS(sen6x_status, 0, "missing", true)
CLASS(sen6x_status, 1, "warming", false)
CLASS(sen6x_status, 2, "stale", false)
CLASS(sen6x_status, 3, "sensor-error", false)
CLASS(sen6x_status, 4, "valid", false)
CLASS(location_source, 0, "none", true)
CLASS(location_source, 1, "configured", false)
CLASS(location_source, 2, "gnss", false)
CLASS(location_source, 3, "phone", false)
