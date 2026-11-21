#pragma once

// Board-neutral text preparation for small displays. UTF-8 is decoded to code
// points, Arabic letters are shaped into their joined presentation forms and
// a line is put into visual order, left to right, so a 1 bit canvas can draw
// it glyph by glyph. No IO, no heap, no clocks and no locale. Every output
// goes into a caller-provided span of code points.
//
// The bidi pass is a reduced Unicode Bidirectional Algorithm (UAX #9) for one
// line of UI text. It resolves weak and neutral types and reorders levels 0
// to 2. Explicit embeddings, isolates and bracket pairs are not handled, so a
// label must not rely on them.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace opensensor::text {

// The base direction of a line. Auto takes the first strong letter and falls
// back to Ltr when there is none.
enum class Direction : std::uint8_t { Ltr, Rtl, Auto };

enum class [[nodiscard]] Error : std::uint8_t {
  Malformed, // Invalid UTF-8 and the policy was Reject.
  Overflow,  // The output span, or kMaxLine for the bidi pass, is too small.
};

// What decoding does with an invalid byte sequence. Replace writes one
// U+FFFD per maximal invalid subpart, the Unicode recommended practice.
enum class OnMalformed : std::uint8_t { Reject, Replace };

inline constexpr char32_t kReplacement = 0xFFFD;

// The longest line the bidi pass reorders, in code points after shaping. Its
// scratch lives on the stack, two bytes per code point.
inline constexpr std::size_t kMaxLine = 160;

// Decodes `utf8` into `out`. Overlong forms, surrogates, values past U+10FFFF
// and truncated sequences are malformed. Returns the code point count.
[[nodiscard]] std::expected<std::size_t, Error>
decode_utf8(std::string_view utf8, std::span<char32_t> out,
            OnMalformed policy = OnMalformed::Reject);

// Shapes Arabic in place, in logical order. Each letter of U+0621 to U+064A
// becomes its isolated, initial, medial or final form from Presentation
// Forms-B, lam followed by alef becomes one ligature, and combining marks are
// transparent to joining. Returns the new length, never more than the input.
[[nodiscard]] std::size_t shape_arabic(std::span<char32_t> text);

// The base direction `d` resolves to for `text`, Ltr or Rtl.
[[nodiscard]] Direction resolve_direction(std::span<const char32_t> text,
                                          Direction d);

// Reorders one shaped line in place from logical to visual order for a base
// direction. Right to left runs are reversed with each combining mark kept
// after its letter, numbers and Latin keep their own order, and paired
// punctuation in a right to left run is mirrored. Fails with Overflow past
// kMaxLine.
[[nodiscard]] std::expected<void, Error> reorder(std::span<char32_t> text,
                                                 Direction base);

// Decode, shape and reorder in one call. Returns the visual line inside `out`.
[[nodiscard]] std::expected<std::span<const char32_t>, Error>
to_visual(std::string_view utf8, std::span<char32_t> out,
          Direction base = Direction::Auto,
          OnMalformed policy = OnMalformed::Reject);

// True when `utf8` holds a right to left letter. Malformed bytes count as
// left to right, so this never fails.
[[nodiscard]] bool has_rtl(std::string_view utf8);

// A combining mark, drawn over or under the letter before it with no advance.
[[nodiscard]] bool is_mark(char32_t cp);

// A right to left letter, Arabic or Hebrew, including presentation forms.
[[nodiscard]] bool is_rtl(char32_t cp);

} // namespace opensensor::text
