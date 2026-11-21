#pragma once

#include <cstddef>

// Path bounds shared by the archive walk, the LIST merge of the flash root and
// the flash copy back (decision record 0004). One definition, so a path that
// fits one walk always fits the others (#210).
namespace opensensor::logger::paths {

// A full path, <root>/<relative>, with its terminating NUL.
inline constexpr std::size_t kPathBytes = 416;
// A name relative to an archive root, with its terminating NUL.
inline constexpr std::size_t kRelativeBytes = 384;

static_assert(kRelativeBytes < kPathBytes,
              "a full path holds its root and the relative name");

} // namespace opensensor::logger::paths
