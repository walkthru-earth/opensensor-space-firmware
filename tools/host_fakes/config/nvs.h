#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 1;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 2;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 3;
constexpr unsigned NVS_READWRITE = 1;
namespace fake_platform {
struct Values {
  std::map<std::string, std::uint32_t> numbers;
  std::map<std::string, std::string> strings;
  std::map<std::string, std::vector<std::uint8_t>> blobs;
  std::map<std::string, unsigned> types;
};
struct Nvs : Values {
  bool fail_open = false;
  std::string fail_write_key;
  std::string fail_commit_key;
  // 1-based write number that fails, 0 for none. Lets one save of a key pass
  // and a later save of the same key fail.
  unsigned fail_write_at = 0;
  // Flips the last byte that nvs_get_blob copies out for this key, so a
  // readback compare sees a record that differs from the one just written.
  std::string corrupt_read_key;
  unsigned writes = 0;
  unsigned commits = 0;
  bool opened = false;
  Values pending;
  std::string pending_key;
};
inline Nvs nvs;
inline bool allow_write(const char *key) {
  ++nvs.writes;
  nvs.pending_key = key;
  return nvs.opened && nvs.fail_write_key != key &&
         nvs.writes != nvs.fail_write_at;
}
inline unsigned type(const char *key) {
  const auto pending = nvs.pending.types.find(key);
  if (pending != nvs.pending.types.end())
    return pending->second;
  const auto found = nvs.types.find(key);
  return found == nvs.types.end() ? 0 : found->second;
}
} // namespace fake_platform
inline esp_err_t nvs_open(const char *name, unsigned, nvs_handle_t *handle) {
  auto &nvs = fake_platform::nvs;
  nvs.opened = std::strcmp(name, "oscfg") == 0 && !nvs.fail_open;
  nvs.pending = {};
  *handle = 1;
  return nvs.opened ? ESP_OK : ESP_FAIL;
}
inline void nvs_close(nvs_handle_t) {
  fake_platform::nvs.opened = false;
  fake_platform::nvs.pending = {};
}
inline esp_err_t nvs_commit(nvs_handle_t) {
  auto &nvs = fake_platform::nvs;
  ++nvs.commits;
  if (!nvs.opened || nvs.fail_commit_key == nvs.pending_key)
    return ESP_FAIL;
  for (const auto &entry : nvs.pending.numbers)
    nvs.numbers[entry.first] = entry.second;
  for (const auto &entry : nvs.pending.strings)
    nvs.strings[entry.first] = entry.second;
  for (const auto &entry : nvs.pending.blobs)
    nvs.blobs[entry.first] = entry.second;
  for (const auto &entry : nvs.pending.types)
    nvs.types[entry.first] = entry.second;
  nvs.pending = {};
  return ESP_OK;
}
inline esp_err_t fake_set_number(const char *key, std::uint32_t value,
                                 unsigned type) {
  if (!fake_platform::allow_write(key))
    return ESP_FAIL;
  fake_platform::nvs.pending.numbers[key] = value;
  fake_platform::nvs.pending.types[key] = type;
  return ESP_OK;
}
inline esp_err_t nvs_set_u8(nvs_handle_t, const char *key, std::uint8_t value) {
  return fake_set_number(key, value, 1);
}
inline esp_err_t nvs_set_u32(nvs_handle_t, const char *key,
                             std::uint32_t value) {
  return fake_set_number(key, value, 4);
}
inline esp_err_t fake_get_number(const char *key, std::uint32_t *out,
                                 unsigned type) {
  const auto &nvs = fake_platform::nvs;
  if (!nvs.opened)
    return ESP_FAIL;
  const auto found_type = fake_platform::type(key);
  if (!found_type)
    return ESP_ERR_NVS_NOT_FOUND;
  if (found_type != type)
    return ESP_ERR_NVS_TYPE_MISMATCH;
  const auto pending = nvs.pending.numbers.find(key);
  *out = pending != nvs.pending.numbers.end() ? pending->second
                                              : nvs.numbers.at(key);
  return ESP_OK;
}
inline esp_err_t nvs_get_u8(nvs_handle_t, const char *key, std::uint8_t *out) {
  std::uint32_t value;
  const auto result = fake_get_number(key, &value, 1);
  if (result == ESP_OK)
    *out = static_cast<std::uint8_t>(value);
  return result;
}
inline esp_err_t nvs_get_u32(nvs_handle_t, const char *key,
                             std::uint32_t *out) {
  return fake_get_number(key, out, 4);
}
inline esp_err_t nvs_set_str(nvs_handle_t, const char *key, const char *value) {
  if (!fake_platform::allow_write(key))
    return ESP_FAIL;
  fake_platform::nvs.pending.strings[key] = value;
  fake_platform::nvs.pending.types[key] = 8;
  return ESP_OK;
}
inline esp_err_t nvs_get_str(nvs_handle_t, const char *key, char *out,
                             std::size_t *size) {
  const auto &nvs = fake_platform::nvs;
  if (!nvs.opened)
    return ESP_FAIL;
  if (!fake_platform::type(key))
    return ESP_ERR_NVS_NOT_FOUND;
  if (fake_platform::type(key) != 8)
    return ESP_ERR_NVS_TYPE_MISMATCH;
  const auto pending = nvs.pending.strings.find(key);
  const auto &value = pending != nvs.pending.strings.end()
                          ? pending->second
                          : nvs.strings.at(key);
  const auto required = value.size() + 1;
  if (out && *size < required) {
    *size = required;
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  *size = required;
  if (out)
    std::memcpy(out, value.c_str(), required);
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *data,
                              std::size_t size) {
  if (!fake_platform::allow_write(key))
    return ESP_FAIL;
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  fake_platform::nvs.pending.blobs[key] =
      std::vector<std::uint8_t>(bytes, bytes + size);
  fake_platform::nvs.pending.types[key] = 9;
  return ESP_OK;
}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *out,
                              std::size_t *size) {
  const auto &nvs = fake_platform::nvs;
  if (!nvs.opened)
    return ESP_FAIL;
  if (!fake_platform::type(key))
    return ESP_ERR_NVS_NOT_FOUND;
  if (fake_platform::type(key) != 9)
    return ESP_ERR_NVS_TYPE_MISMATCH;
  const auto pending = nvs.pending.blobs.find(key);
  const auto &value =
      pending != nvs.pending.blobs.end() ? pending->second : nvs.blobs.at(key);
  if (out && *size < value.size()) {
    *size = value.size();
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  *size = value.size();
  if (out) {
    std::memcpy(out, value.data(), value.size());
    if (nvs.corrupt_read_key == key && !value.empty())
      static_cast<std::uint8_t *>(out)[value.size() - 1] ^= 1;
  }
  return ESP_OK;
}
