#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

// The archive roots the logger is given and the rule that keeps the flash
// fallback root (decision record 0004) apart from the others. Pure, so the
// host test checks every overlap. It only compares strings.
namespace opensensor::logger::roots {

// Longest fallback root, so <root>/<relative> fits the 416 byte walk paths.
inline constexpr std::size_t kMaxFallbackBytes = 31;

// True when `path` is `root` or lies below it. A missing path or root holds
// nothing.
[[nodiscard]] inline bool path_within(const char *path, const char *root) {
  if (path == nullptr || root == nullptr)
    return false;
  const std::size_t length = std::strlen(root);
  if (length == 1 && root[0] == '/')
    return path[0] == '/';
  return std::strncmp(path, root, length) == 0 &&
         (path[length] == '\0' || path[length] == '/');
}

struct Roots {
  const char *output = nullptr;    // the card archive
  const char *benchmark = nullptr; // codec-test copies
  const char *legacy = nullptr;    // optional legacy archive
  const char *fallback = nullptr;  // optional flash archive
};

// True when the fallback root is unset, or an absolute path of at most
// kMaxFallbackBytes that neither holds nor sits below the card archive, the
// codec-test root or the legacy root. A root inside another would make one
// walk list or quarantine the files of the other.
[[nodiscard]] inline bool fallback_valid(const Roots &roots) {
  const char *root = roots.fallback;
  if (root == nullptr)
    return true;
  if (root[0] != '/' || std::strlen(root) > kMaxFallbackBytes)
    return false;
  const std::array others{roots.output, roots.benchmark, roots.legacy};
  return std::none_of(others.begin(), others.end(), [root](const char *other) {
    return path_within(root, other) || path_within(other, root);
  });
}

} // namespace opensensor::logger::roots
