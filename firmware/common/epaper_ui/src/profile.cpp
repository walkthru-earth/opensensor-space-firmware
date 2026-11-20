#include "epaper_ui/profile.h"

#include "epaper_ui/layout.h"

namespace opensensor::ui {

std::expected<void, ProfileError> validate(const DisplayProfile &p) {
  if (p.width <= 0 || p.height <= 0 || p.width % 8 != 0)
    return std::unexpected(ProfileError::BadSize);
  if (p.depth != ColorDepth::Mono1)
    return std::unexpected(ProfileError::UnsupportedDepth);
  if (p.layout == nullptr || p.rotated_layout == nullptr)
    return std::unexpected(ProfileError::NoLayout);
  if (p.layout->width != p.width || p.layout->height != p.height ||
      p.layout->scale != p.font_scale)
    return std::unexpected(ProfileError::LayoutMismatch);
  if (p.rotated_layout->width != p.height ||
      p.rotated_layout->height != p.width ||
      p.rotated_layout->scale != p.font_scale)
    return std::unexpected(ProfileError::LayoutMismatch);
  // A partial window needs partial refresh, and partial refresh needs a
  // bound on the ghosting it leaves.
  if ((p.refresh.partial_window && !p.refresh.partial) ||
      (p.refresh.partial && p.refresh.partials_per_full == 0))
    return std::unexpected(ProfileError::BadRefreshCaps);
  return {};
}

const char *to_string(ProfileError e) {
  switch (e) {
  case ProfileError::BadSize:
    return "bad-size";
  case ProfileError::UnsupportedDepth:
    return "unsupported-depth";
  case ProfileError::NoLayout:
    return "no-layout";
  case ProfileError::LayoutMismatch:
    return "layout-mismatch";
  case ProfileError::BadRefreshCaps:
    return "bad-refresh-caps";
  }
  return "unknown";
}

} // namespace opensensor::ui
