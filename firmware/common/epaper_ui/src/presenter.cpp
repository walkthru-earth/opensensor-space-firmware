#include "epaper_ui/presenter.h"

#include <algorithm>

namespace opensensor::ui {
namespace {

constexpr std::uint8_t reverse_bits(std::uint8_t b) {
  b = static_cast<std::uint8_t>(((b & 0xF0U) >> 4) | ((b & 0x0FU) << 4));
  b = static_cast<std::uint8_t>(((b & 0xCCU) >> 2) | ((b & 0x33U) << 2));
  return static_cast<std::uint8_t>(((b & 0xAAU) >> 1) | ((b & 0x55U) << 1));
}
static_assert(reverse_bits(0x80) == 0x01 && reverse_bits(0x35) == 0xAC);

bool bit_at(std::span<const std::uint8_t> frame, std::size_t stride, int x,
            int y) {
  const std::size_t i =
      static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x >> 3);
  return (frame[i] & (0x80U >> (x & 7))) != 0;
}

} // namespace

epd::Refresh RefreshPolicy::next(bool page_changed, bool forced) {
  if (!shown_ || forced ||
      (caps_.partial && partials_ >= caps_.partials_per_full)) {
    shown_ = true;
    partials_ = 0;
    return epd::Refresh::Full;
  }
  if (page_changed) {
    partials_ = 0;
    return caps_.fast ? epd::Refresh::Fast : epd::Refresh::Full;
  }
  if (caps_.partial) {
    ++partials_;
    return epd::Refresh::Partial;
  }
  return caps_.fast ? epd::Refresh::Fast : epd::Refresh::Full;
}

void rotate_frame(std::span<const std::uint8_t> logical,
                  const DisplayProfile &profile, Rotation rotation,
                  std::span<std::uint8_t> panel) {
  const int w = profile.width;
  const int h = profile.height;
  const auto stride = static_cast<std::size_t>(profile.stride());
  const std::size_t bytes = profile.frame_bytes();
  if (panel.size() < bytes ||
      logical.size() < Canvas::bytes_for(profile, rotation))
    return;
  switch (rotation) {
  case Rotation::R0:
    std::copy_n(logical.begin(), bytes, panel.begin());
    return;
  case Rotation::R180:
    // Rows bottom up, bytes right to left, bits reversed. The width is a
    // multiple of 8, so no bit crosses a byte.
    for (std::size_t i = 0; i < bytes; ++i)
      panel[i] = reverse_bits(logical[bytes - 1 - i]);
    return;
  case Rotation::R90:
  case Rotation::R270: {
    // The logical frame is h wide and w tall.
    const auto ls = static_cast<std::size_t>((h + 7) / 8);
    for (int y = 0; y < h; ++y) {
      for (std::size_t bx = 0; bx < stride; ++bx) {
        std::uint8_t out = 0;
        for (int bit = 0; bit < 8; ++bit) {
          const int x = static_cast<int>(bx) * 8 + bit;
          // R90 puts logical (lx, ly) at panel (w - 1 - ly, lx).
          // R270 puts it at panel (ly, h - 1 - lx).
          const bool on = rotation == Rotation::R90
                              ? bit_at(logical, ls, y, w - 1 - x)
                              : bit_at(logical, ls, h - 1 - y, x);
          if (on)
            out = static_cast<std::uint8_t>(out | (0x80U >> bit));
        }
        panel[static_cast<std::size_t>(y) * stride + bx] = out;
      }
    }
    return;
  }
  }
}

Rect changed_window(std::span<const std::uint8_t> a,
                    std::span<const std::uint8_t> b,
                    const DisplayProfile &profile) {
  const auto stride = static_cast<std::size_t>(profile.stride());
  const auto rows = static_cast<std::size_t>(profile.height);
  if (stride == 0 || a.size() < stride * rows || b.size() < stride * rows)
    return {};
  std::size_t x0 = stride;
  std::size_t x1 = 0;
  std::size_t y0 = rows;
  std::size_t y1 = 0;
  for (std::size_t y = 0; y < rows; ++y) {
    for (std::size_t x = 0; x < stride; ++x) {
      if (a[y * stride + x] == b[y * stride + x])
        continue;
      x0 = std::min(x0, x);
      x1 = std::max(x1, x);
      y0 = std::min(y0, y);
      y1 = y;
    }
  }
  if (y0 == rows)
    return {};
  return {static_cast<int>(x0 * 8), static_cast<int>(y0),
          static_cast<int>((x1 - x0 + 1) * 8), static_cast<int>(y1 - y0 + 1)};
}

const char *to_string(PresenterError e) {
  switch (e) {
  case PresenterError::BadProfile:
    return "bad-profile";
  case PresenterError::BufferSize:
    return "buffer-size";
  }
  return "unknown";
}

std::expected<Presenter, PresenterError> Presenter::create(
    const DisplayProfile &profile, std::span<std::uint8_t> logical,
    std::span<std::uint8_t> panel, std::span<std::uint8_t> shown) {
  if (!validate(profile))
    return std::unexpected(PresenterError::BadProfile);
  const std::size_t frame = profile.frame_bytes();
  if (logical.size() < profile.logical_frame_bytes() || panel.size() < frame ||
      shown.size() < frame)
    return std::unexpected(PresenterError::BufferSize);
  return Presenter{profile, logical, panel.first(frame), shown.first(frame)};
}

Presenter::Presenter(const DisplayProfile &profile,
                     std::span<std::uint8_t> logical,
                     std::span<std::uint8_t> panel,
                     std::span<std::uint8_t> shown)
    : profile_(&profile), logical_(logical), panel_(panel), shown_(shown),
      policy_(profile.refresh) {}

std::expected<Canvas, CanvasError> Presenter::canvas(Rotation rotation) {
  drawn_ = rotation;
  return Canvas::create(logical_, *profile_, rotation);
}

bool Presenter::compose() {
  rotate_frame(logical_, *profile_, drawn_, panel_);
  return !has_shown_ || drawn_ != on_glass_ ||
         !std::equal(panel_.begin(), panel_.end(), shown_.begin());
}

Plan Presenter::plan(bool page_changed, bool forced) {
  const Rect whole{0, 0, profile_->width, profile_->height};
  const bool turned = has_shown_ && drawn_ != on_glass_;
  const epd::Refresh mode = policy_.next(page_changed, forced || turned);
  if (mode != epd::Refresh::Partial || !profile_->refresh.partial_window ||
      !has_shown_)
    return {.mode = mode, .window = whole};
  const Rect window = changed_window(panel_, shown_, *profile_);
  return {.mode = mode, .window = window.w > 0 ? window : whole};
}

void Presenter::shown_done() {
  std::copy(panel_.begin(), panel_.end(), shown_.begin());
  on_glass_ = drawn_;
  has_shown_ = true;
}

} // namespace opensensor::ui
