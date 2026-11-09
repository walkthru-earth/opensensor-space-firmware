#pragma once

// Host generator of the stac-opensensor v0.1.0 board template (#85). It joins
// a board's FIELD table with its SEM, CLASS and derived rows into one minified
// OpenSensorTemplate JSON. Output is a pure function of the tables, so the
// same tables give the same bytes, with no time or host in them.
// tools/test_telemetry_contract.py and tools/test_waveshare_contract.py write
// the bytes into each board's telemetry_template.h and their SHA-256 into
// telemetry_descriptor_digest.h, and fail when either is stale. Key order
// follows stac-opensensor templates/cores3-telemetry-v5.json (a017edf).

#include <column_semantics.h>
#include <parquet_writer.h>
#include <telemetry_logical.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boardtemplate {

struct Board {
  // Firmware schema name, the template id and firmware.schema.
  std::string_view id;
  std::string_view title;
  std::span<const std::string_view> keywords;
  // describedby link to the dictionary, with file:checksum "1220" + dict.
  std::string_view dictionary_href;
  std::string_view dict;
  // oslogger::Config default, the board does not override it.
  std::uint32_t sample_interval_ms = 0;
  // ble::kProtocolVersion, the lowest sync protocol the board speaks.
  unsigned proto_min = 0;
};

[[noreturn]] inline void fail(std::string_view what, std::string_view where) {
  std::fprintf(stderr, "board template: %.*s: %.*s\n", int(what.size()),
               what.data(), int(where.size()), where.data());
  std::abort();
}

class Writer {
public:
  std::string text;

  void raw(std::string_view bytes) { text.append(bytes); }

  // Printable ASCII only, so the bytes in .rodata are the bytes hashed and no
  // reader has to agree on a Unicode escape form.
  void string(std::string_view value) {
    text.push_back('"');
    for (const char c : value) {
      if (c < 0x20 || c > 0x7e)
        fail("non printable byte in string", value);
      if (c == '"' || c == '\\')
        text.push_back('\\');
      text.push_back(c);
    }
    text.push_back('"');
  }

  void key(std::string_view name) {
    comma();
    string(name);
    text.push_back(':');
    fresh = true;
  }

  void open(char bracket) {
    comma();
    text.push_back(bracket);
    fresh = true;
  }
  void close(char bracket) {
    text.push_back(bracket);
    fresh = false;
  }

  void value(std::string_view text_value) {
    comma();
    string(text_value);
    fresh = false;
  }
  void integer(std::int64_t number) {
    comma();
    text.append(std::to_string(number));
    fresh = false;
  }
  void boolean(bool flag) {
    comma();
    text.append(flag ? "true" : "false");
    fresh = false;
  }
  // Decimal text from the .inc, written as a JSON number unchanged so the
  // template carries exactly the sourced bound.
  void decimal(std::string_view number) {
    if (!opensensor::semantics::is_decimal(number))
      fail("not a decimal", number);
    const auto digits = number.substr(number.starts_with('-') ? 1 : 0);
    if (digits.size() > 1 && digits[0] == '0' && digits[1] != '.')
      fail("leading zero is not JSON", number);
    comma();
    text.append(number);
    fresh = false;
  }

private:
  bool fresh = true;
  void comma() {
    if (!fresh)
      text.push_back(',');
    fresh = false;
  }
};

// The STAC table type. A logical type names the column as readers see it, so
// an INT32 code is uint8 and a BYTE_ARRAY point is geometry.
inline std::string_view type_name(telemetry::PhysicalType type,
                                  const char *unit) {
  using telemetry::PhysicalType;
  switch (telemetry::logical_for(type, unit)) {
  case telemetry::LogicalType::Uint8:
    return "uint8";
  case telemetry::LogicalType::Geometry:
    return "geometry";
  case telemetry::LogicalType::None:
  case telemetry::LogicalType::TimestampMicrosUtc:
    break;
  }
  switch (type) {
  case PhysicalType::Int32:
    return "int32";
  case PhysicalType::Int64:
    return "int64";
  case PhysicalType::Float:
    return "float";
  case PhysicalType::ByteArray:
    break;
  }
  fail("unknown physical type", "");
}

inline std::string_view role_name(opensensor::semantics::Role role) {
  using opensensor::semantics::Role;
  switch (role) {
  case Role::physicalMeasurement:
    return "physicalMeasurement";
  case Role::auxiliaryInformation:
    return "auxiliaryInformation";
  case Role::qualityInformation:
    return "qualityInformation";
  case Role::referenceInformation:
    return "referenceInformation";
  case Role::coordinate:
    return "coordinate";
  case Role::modelResult:
    return "modelResult";
  }
  fail("unknown role", "");
}

inline std::string_view
aggregate_name(opensensor::semantics::Aggregate aggregate) {
  using opensensor::semantics::Aggregate;
  switch (aggregate) {
  case Aggregate::mean:
    return "mean";
  case Aggregate::minimum:
    return "minimum";
  case Aggregate::maximum:
    return "maximum";
  case Aggregate::sum:
    return "sum";
  case Aggregate::point:
    return "point";
  case Aggregate::mode:
    return "mode";
  }
  fail("unknown aggregate", "");
}

// Milliseconds as exact decimal seconds, 10000 as 10 and 2500 as 2.5.
inline std::string seconds(std::uint32_t ms) {
  if (ms == 0)
    fail("sample interval must be positive", "");
  std::string text = std::to_string(ms / 1000);
  if (const auto rest = ms % 1000) {
    std::string fraction = std::to_string(1000 + rest).substr(1);
    while (fraction.back() == '0')
      fraction.pop_back();
    text += "." + fraction;
  }
  return text;
}

// "<column> == <n>" as {"column","equals":n}, "finite" as the column's own
// finite gate, "" as no key.
inline void gate(Writer &out, const opensensor::semantics::Column &row) {
  const auto when = row.valid_when;
  if (when.empty())
    return;
  out.key("opensensor:valid_when");
  out.open('{');
  if (when == "finite") {
    out.key("column");
    out.value(row.name);
    out.key("finite");
    out.boolean(true);
  } else {
    constexpr std::string_view kEquals = " == ";
    const auto at = when.find(kEquals);
    if (at == std::string_view::npos)
      fail("malformed valid_when", when);
    out.key("column");
    out.value(when.substr(0, at));
    out.key("equals");
    out.decimal(when.substr(at + kEquals.size()));
  }
  out.close('}');
}

template <typename Field>
bool instrument(const Field &field, const opensensor::semantics::Column &row) {
  // A procedure that names a fitted sensor. Identity, clock and logger
  // bookkeeping have no sensor model, and a sensor the board cannot read
  // (validity "unavailable") is not an instrument of this build.
  return !row.sensor_model.empty() &&
         row.role != opensensor::semantics::Role::referenceInformation &&
         std::string_view{field.validity} != "unavailable";
}

template <typename Fields>
std::string build(const Board &board, const Fields &fields,
                  std::span<const opensensor::semantics::Column> rows,
                  std::span<const opensensor::semantics::Class> classes,
                  std::span<const opensensor::semantics::Derived> derived) {
  if (!opensensor::semantics::aligned(fields, rows))
    fail("FIELD and SEM rows differ", board.id);
  if (board.dict.size() != 64)
    fail("dict is not a SHA-256", board.dict);
  Writer out;
  out.open('{');
  out.key("type");
  out.value("OpenSensorTemplate");
  out.key("template_version");
  out.value("0.1.0");
  out.key("id");
  out.value(board.id);
  out.key("title");
  out.value(board.title);
  out.key("keywords");
  out.open('[');
  for (const auto keyword : board.keywords)
    out.value(keyword);
  out.close(']');

  out.key("instruments");
  out.open('[');
  std::vector<std::string_view> seen;
  std::size_t i = 0;
  for (const auto &field : fields) {
    const std::string_view procedure{field.procedure};
    if (instrument(field, rows[i++]) &&
        std::find(seen.begin(), seen.end(), procedure) == seen.end()) {
      seen.push_back(procedure);
      out.value(procedure);
    }
  }
  if (seen.empty())
    fail("no instruments", board.id);
  out.close(']');

  out.key("opensensor:sample_interval_s");
  out.decimal(seconds(board.sample_interval_ms));

  out.key("item_assets");
  out.open('{');
  out.key("data");
  out.open('{');
  out.key("type");
  out.value("application/vnd.apache.parquet");
  out.key("roles");
  out.open('[');
  out.value("data");
  out.close(']');
  out.key("table:primary_datetime");
  out.value("event_time_utc_us");
  // stac-opensensor 83787768 PROFILE, set when the files have the column.
  if (std::any_of(std::begin(fields), std::end(fields), [](const auto &field) {
        return std::string_view{field.name} == "geometry";
      })) {
    out.key("table:primary_geometry");
    out.value("geometry");
  }
  out.key("table:columns");
  out.open('[');
  i = 0;
  for (const auto &field : fields) {
    const auto &row = rows[i];
    const auto field_id = std::int64_t(++i);
    out.open('{');
    out.key("name");
    out.value(row.name);
    out.key("type");
    out.value(type_name(field.type, field.unit));
    // The GEOMETRY column has no unit (stac-opensensor 83787768 PROFILE).
    if (!row.ucum.empty()) {
      out.key("unit");
      out.value(row.ucum);
    }
    out.key("roles");
    out.open('[');
    out.value(role_name(row.role));
    out.close(']');
    if (!row.cf_standard_name.empty()) {
      out.key("cf:standard_name");
      out.value(row.cf_standard_name);
    }
    bool listed = false;
    for (const auto &entry : classes) {
      if (entry.column != row.name)
        continue;
      if (!listed) {
        out.key("classification:classes");
        out.open('[');
        listed = true;
      }
      out.open('{');
      out.key("value");
      out.integer(entry.value);
      out.key("name");
      out.value(entry.name);
      if (entry.nodata) {
        out.key("nodata");
        out.boolean(true);
      }
      out.close('}');
    }
    if (listed)
      out.close(']');
    out.key("opensensor:property");
    out.value(row.property);
    out.key("opensensor:procedure");
    out.value(field.procedure);
    out.key("opensensor:field_id");
    out.integer(field_id);
    gate(out, row);
    if (row.settle_s > 0) {
      out.key("opensensor:settle_s");
      out.integer(row.settle_s);
    }
    for (const auto &entry : derived) {
      if (entry.column != row.name)
        continue;
      out.key("opensensor:derived");
      out.open('{');
      out.key("from");
      out.open('[');
      for (const auto source : entry.from)
        out.value(source);
      out.close(']');
      out.key("method");
      out.value(entry.method);
      out.close('}');
    }
    out.key("opensensor:aggregate");
    out.value(aggregate_name(row.aggregate));
    if (!row.plausible_min.empty()) {
      out.key("opensensor:plausible_range");
      out.open('[');
      out.decimal(row.plausible_min);
      out.decimal(row.plausible_max);
      out.close(']');
    }
    out.close('}');
  }
  out.close(']');
  out.close('}');
  out.close('}');

  out.key("links");
  out.open('[');
  out.open('{');
  out.key("rel");
  out.value("describedby");
  out.key("href");
  out.value(board.dictionary_href);
  out.key("type");
  out.value("text/plain");
  out.key("title");
  out.value("Firmware column dictionary");
  out.key("file:checksum");
  out.value(std::string{"1220"} + std::string{board.dict});
  out.close('}');
  out.close(']');

  out.key("firmware");
  out.open('{');
  out.key("schema");
  out.value(board.id);
  out.key("proto_min");
  out.integer(board.proto_min);
  out.key("dict");
  out.value(board.dict);
  out.key("live_keys");
  out.open('{');
  for (const auto &field : fields)
    if (field.live_key != nullptr && *field.live_key != '\0') {
      out.key(field.name);
      out.value(field.live_key);
    }
  out.close('}');
  out.key("unit_labels");
  out.open('{');
  i = 0;
  for (const auto &field : fields)
    if (std::string_view{field.unit} != rows[i++].ucum) {
      out.key(field.name);
      out.value(field.unit);
    }
  out.close('}');
  out.close('}');
  out.close('}');
  return out.text;
}

} // namespace boardtemplate
