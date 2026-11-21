#include "os_text.h"

#include <algorithm>
#include <array>
#include <optional>

namespace opensensor::text {
namespace {

// ---- UTF-8 ----------------------------------------------------------------

// Decodes the code point at `i` and moves `i` past it. On malformed input it
// moves `i` past the maximal invalid subpart, at least one byte, and returns
// nullopt.
[[nodiscard]] std::optional<char32_t> next_codepoint(std::string_view s,
                                                     std::size_t &i) {
  const auto b0 = static_cast<std::uint8_t>(s[i++]);
  if (b0 < 0x80U)
    return b0;
  int need = 0;
  std::uint8_t lo = 0x80U;
  std::uint8_t hi = 0xBFU;
  char32_t cp = 0;
  if (b0 >= 0xC2U && b0 <= 0xDFU) {
    need = 1;
    cp = b0 & 0x1FU;
  } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
    need = 2;
    cp = b0 & 0x0FU;
    if (b0 == 0xE0U)
      lo = 0xA0U; // Overlong below U+0800.
    else if (b0 == 0xEDU)
      hi = 0x9FU; // UTF-16 surrogates.
  } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
    need = 3;
    cp = b0 & 0x07U;
    if (b0 == 0xF0U)
      lo = 0x90U; // Overlong below U+10000.
    else if (b0 == 0xF4U)
      hi = 0x8FU; // Past U+10FFFF.
  } else {
    return std::nullopt; // Continuation byte, C0, C1 or F5 to FF.
  }
  for (int k = 0; k < need; ++k) {
    if (i >= s.size())
      return std::nullopt;
    const auto b = static_cast<std::uint8_t>(s[i]);
    if (b < lo || b > hi)
      return std::nullopt; // The byte starts the next sequence.
    cp = (cp << 6U) | (b & 0x3FU);
    ++i;
    lo = 0x80U;
    hi = 0xBFU;
  }
  return cp;
}

// ---- Arabic joining -------------------------------------------------------

enum class Join : std::uint8_t { None, Right, Dual };

struct Letter {
  Join join = Join::None;
  char32_t forms = 0; // Isolated form, then final, initial and medial.
};

inline constexpr char32_t kFirstLetter = 0x0621; // Hamza.
inline constexpr char32_t kLastLetter = 0x064A;  // Yeh.
inline constexpr char32_t kTatweel = 0x0640;
inline constexpr char32_t kLam = 0x0644;

[[nodiscard]] consteval Join letter_join(char32_t c) {
  switch (c) {
  case 0x0621: // Hamza never joins.
    return Join::None;
  case 0x0622:
  case 0x0623:
  case 0x0624:
  case 0x0625:
  case 0x0627:
  case 0x0629:
  case 0x062F:
  case 0x0630:
  case 0x0631:
  case 0x0632:
  case 0x0648:
  case 0x0649: // Alef maksura has only isolated and final forms in FE70.
    return Join::Right;
  default:
    return Join::Dual;
  }
}

// U+FE80 to U+FEF4 list the letters' forms in Unicode order, one for hamza,
// two for a right joining letter and four for a dual joining one. U+063B to
// U+0640 have none.
[[nodiscard]] consteval std::array<Letter, kLastLetter - kFirstLetter + 1>
make_letters() {
  std::array<Letter, kLastLetter - kFirstLetter + 1> out{};
  char32_t at = 0xFE80;
  for (char32_t c = kFirstLetter; c <= kLastLetter; ++c) {
    if (c >= 0x063B && c <= 0x0640)
      continue;
    const Join j = letter_join(c);
    out[c - kFirstLetter] = {.join = j, .forms = at};
    at += c == 0x0621 ? 1 : (j == Join::Right ? 2 : 4);
  }
  return out;
}

static constexpr auto kLetters = make_letters();
static_assert(kLetters[0x0627 - kFirstLetter].forms == 0xFE8D); // Alef.
static_assert(kLetters[0x0644 - kFirstLetter].forms == 0xFEDD); // Lam.
static_assert(kLetters[0x064A - kFirstLetter].forms == 0xFEF1); // Yeh.

[[nodiscard]] const Letter *letter(char32_t c) {
  if (c < kFirstLetter || c > kLastLetter)
    return nullptr;
  const Letter &l = kLetters[c - kFirstLetter];
  return l.forms != 0 ? &l : nullptr;
}

[[nodiscard]] Join joining(char32_t c) {
  if (c == kTatweel)
    return Join::Dual;
  const Letter *l = letter(c);
  return l != nullptr ? l->join : Join::None;
}

// The isolated lam-alef ligature for an alef, 0 for any other letter. The
// final form is one more.
[[nodiscard]] char32_t lam_alef(char32_t alef) {
  switch (alef) {
  case 0x0622:
    return 0xFEF5;
  case 0x0623:
    return 0xFEF7;
  case 0x0625:
    return 0xFEF9;
  case 0x0627:
    return 0xFEFB;
  default:
    return 0;
  }
}

// ---- Bidi classes ---------------------------------------------------------

// UAX #9 bidi classes this pass resolves. B and S read as WS, BN as ON.
enum class Bc : std::uint8_t { L, R, AL, EN, ES, ET, AN, CS, NSM, WS, ON };

[[nodiscard]] bool in(char32_t c, char32_t lo, char32_t hi) {
  return c >= lo && c <= hi;
}

[[nodiscard]] Bc classify(char32_t c) {
  if (is_mark(c))
    return Bc::NSM;
  if (in(c, '0', '9') || c == 0xB2 || c == 0xB3 || c == 0xB9 ||
      in(c, 0x2074, 0x2079) || c == 0x2070 || in(c, 0x2080, 0x2089) ||
      in(c, 0x06F0, 0x06F9))
    return Bc::EN;
  if (in(c, 'A', 'Z') || in(c, 'a', 'z'))
    return Bc::L;
  if (c < 0x80) {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
      return Bc::WS;
    case '+':
    case '-':
      return Bc::ES;
    case '#':
    case '$':
    case '%':
      return Bc::ET;
    case ',':
    case '.':
    case '/':
    case ':':
      return Bc::CS;
    default:
      return Bc::ON;
    }
  }
  if (c == 0xA0)
    return Bc::CS;
  if (in(c, 0xA2, 0xA5) || c == 0xB0 || c == 0xB1 || in(c, 0x2030, 0x2034) ||
      in(c, 0x20A0, 0x20CF) || c == 0x066A)
    return Bc::ET;
  if (c == 0xAA || c == 0xB5 || c == 0xBA ||
      (in(c, 0xC0, 0x2B8) && c != 0xD7 && c != 0xF7) || in(c, 0x370, 0x52F))
    return Bc::L;
  if (c < 0x100)
    return Bc::ON;
  if (in(c, 0x0590, 0x05FF) || in(c, 0xFB1D, 0xFB4F))
    return Bc::R;
  if (in(c, 0x0600, 0x0605) || in(c, 0x0660, 0x0669) || c == 0x066B ||
      c == 0x066C || c == 0x06DD)
    return Bc::AN;
  if (c == 0x060C)
    return Bc::CS;
  if (in(c, 0x0600, 0x06FF) || in(c, 0x0750, 0x077F) || in(c, 0x08A0, 0x08FF) ||
      in(c, 0xFB50, 0xFDFF) || in(c, 0xFE70, 0xFEFE))
    return Bc::AL;
  if (in(c, 0x2000, 0x200A) || c == 0x3000)
    return Bc::WS;
  if (in(c, 0x2010, 0x2BFF) || in(c, 0x3001, 0x303F) || c == kReplacement)
    return Bc::ON;
  return Bc::L;
}

[[nodiscard]] bool is_strong(Bc b) {
  return b == Bc::L || b == Bc::R || b == Bc::AL;
}

[[nodiscard]] char32_t mirror(char32_t c) {
  switch (c) {
  case '(':
    return ')';
  case ')':
    return '(';
  case '[':
    return ']';
  case ']':
    return '[';
  case '{':
    return '}';
  case '}':
    return '{';
  case '<':
    return '>';
  case '>':
    return '<';
  case 0xAB:
    return 0xBB;
  case 0xBB:
    return 0xAB;
  case 0x2039:
    return 0x203A;
  case 0x203A:
    return 0x2039;
  default:
    return c;
  }
}

// Reverses text[s, t) together with its levels.
void reverse_range(std::span<char32_t> text, std::span<std::uint8_t> level,
                   std::size_t s, std::size_t t) {
  std::reverse(text.begin() + static_cast<std::ptrdiff_t>(s),
               text.begin() + static_cast<std::ptrdiff_t>(t));
  std::reverse(level.begin() + static_cast<std::ptrdiff_t>(s),
               level.begin() + static_cast<std::ptrdiff_t>(t));
}

} // namespace

bool is_mark(char32_t c) {
  return in(c, 0x0300, 0x036F) || in(c, 0x0591, 0x05BD) ||
         in(c, 0x0610, 0x061A) || in(c, 0x064B, 0x065F) || c == 0x0670 ||
         in(c, 0x06D6, 0x06DC) || in(c, 0x06DF, 0x06E4) ||
         in(c, 0x06E7, 0x06E8) || in(c, 0x06EA, 0x06ED) ||
         in(c, 0xFE20, 0xFE2F);
}

bool is_rtl(char32_t c) {
  const Bc b = classify(c);
  return b == Bc::R || b == Bc::AL;
}

std::expected<std::size_t, Error> decode_utf8(std::string_view utf8,
                                              std::span<char32_t> out,
                                              OnMalformed policy) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const std::optional<char32_t> cp = next_codepoint(utf8, i);
    if (!cp && policy == OnMalformed::Reject)
      return std::unexpected(Error::Malformed);
    if (n >= out.size())
      return std::unexpected(Error::Overflow);
    out[n++] = cp ? *cp : kReplacement;
  }
  return n;
}

std::size_t shape_arabic(std::span<char32_t> t) {
  const std::size_t n = t.size();
  std::size_t m = 0;
  Join before = Join::None; // The last letter's joining, marks skipped.
  // Writing never passes reading, m <= i, so the shaping runs in place.
  for (std::size_t i = 0; i < n; ++i) {
    const char32_t c = t[i];
    if (is_mark(c)) {
      t[m++] = c;
      continue;
    }
    const Join jt = joining(c);
    std::size_t k = i + 1;
    while (k < n && is_mark(t[k]))
      ++k;
    const char32_t next = k < n ? t[k] : 0;
    const bool joins_before = jt != Join::None && before == Join::Dual;
    if (c == kLam && lam_alef(next) != 0) {
      t[m++] = lam_alef(next) + (joins_before ? 1 : 0);
      for (std::size_t j = i + 1; j < k; ++j)
        t[m++] = t[j]; // Marks on the lam follow the ligature.
      i = k;           // The alef is part of the ligature.
      before = Join::Right;
      continue;
    }
    before = jt;
    const Letter *l = letter(c);
    if (l == nullptr) {
      t[m++] = c; // Tatweel and every non letter stay as they are.
      continue;
    }
    const bool joins_after =
        jt == Join::Dual && next != 0 && joining(next) != Join::None;
    char32_t form = 0;
    if (joins_before)
      form = joins_after ? 3 : 1;
    else if (joins_after)
      form = 2;
    t[m++] = l->forms + form;
  }
  return m;
}

Direction resolve_direction(std::span<const char32_t> text, Direction d) {
  if (d != Direction::Auto)
    return d;
  for (const char32_t c : text) {
    const Bc b = classify(c);
    if (b == Bc::L)
      return Direction::Ltr;
    if (b == Bc::R || b == Bc::AL)
      return Direction::Rtl;
  }
  return Direction::Ltr;
}

std::expected<void, Error> reorder(std::span<char32_t> text, Direction base) {
  const std::size_t n = text.size();
  if (n > kMaxLine)
    return std::unexpected(Error::Overflow);
  const bool rtl = resolve_direction(text, base) == Direction::Rtl;
  const Bc sos = rtl ? Bc::R : Bc::L;

  std::array<Bc, kMaxLine> cls{};
  bool any_rtl = false;
  for (std::size_t i = 0; i < n; ++i) {
    cls[i] = classify(text[i]);
    any_rtl |= cls[i] == Bc::R || cls[i] == Bc::AL || cls[i] == Bc::AN;
  }
  if (!rtl && !any_rtl)
    return {}; // Left to right with nothing to move.

  // W1 to W3. A mark takes the class before it, a European number after an
  // Arabic letter is an Arabic number, and AL is R from here on.
  Bc strong = sos;
  for (std::size_t i = 0; i < n; ++i) {
    if (cls[i] == Bc::NSM)
      cls[i] = i == 0 ? sos : cls[i - 1];
    if (is_strong(cls[i]))
      strong = cls[i];
    else if (cls[i] == Bc::EN && strong == Bc::AL)
      cls[i] = Bc::AN;
  }
  for (std::size_t i = 0; i < n; ++i)
    if (cls[i] == Bc::AL)
      cls[i] = Bc::R;
  // W4. One separator between two numbers of one kind joins them.
  for (std::size_t i = 1; i + 1 < n; ++i) {
    const Bc prev = cls[i - 1];
    if (prev != cls[i + 1])
      continue;
    if ((cls[i] == Bc::ES && prev == Bc::EN) ||
        (cls[i] == Bc::CS && (prev == Bc::EN || prev == Bc::AN)))
      cls[i] = prev;
  }
  // W5. Terminators next to a European number, like % or a degree sign,
  // become part of it.
  for (std::size_t i = 0; i < n;) {
    if (cls[i] != Bc::ET) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && cls[j] == Bc::ET)
      ++j;
    if ((i > 0 && cls[i - 1] == Bc::EN) || (j < n && cls[j] == Bc::EN))
      std::fill(cls.begin() + static_cast<std::ptrdiff_t>(i),
                cls.begin() + static_cast<std::ptrdiff_t>(j), Bc::EN);
    i = j;
  }
  // W6 and W7. Leftover separators are neutral, and a European number after
  // a left to right letter is left to right.
  strong = sos;
  for (std::size_t i = 0; i < n; ++i) {
    if (cls[i] == Bc::ES || cls[i] == Bc::ET || cls[i] == Bc::CS)
      cls[i] = Bc::ON;
    if (cls[i] == Bc::L || cls[i] == Bc::R)
      strong = cls[i];
    else if (cls[i] == Bc::EN && strong == Bc::L)
      cls[i] = Bc::L;
  }
  // N1 and N2. Neutrals between two sides of one direction take it, numbers
  // counting as right to left, others take the base direction.
  const auto side = [](Bc b) { return b == Bc::L ? Bc::L : Bc::R; };
  for (std::size_t i = 0; i < n;) {
    if (cls[i] != Bc::WS && cls[i] != Bc::ON) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && (cls[j] == Bc::WS || cls[j] == Bc::ON))
      ++j;
    const Bc lead = i > 0 ? side(cls[i - 1]) : sos;
    const Bc trail = j < n ? side(cls[j]) : sos;
    const Bc fill = lead == trail ? lead : sos;
    std::fill(cls.begin() + static_cast<std::ptrdiff_t>(i),
              cls.begin() + static_cast<std::ptrdiff_t>(j), fill);
    i = j;
  }
  // I1 and I2, then L1 for trailing white space.
  std::array<std::uint8_t, kMaxLine> level{};
  const std::uint8_t e = rtl ? 1 : 0;
  std::uint8_t top = e;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint8_t lv = e;
    if (rtl)
      lv = cls[i] == Bc::R ? 1 : 2;
    else if (cls[i] == Bc::R)
      lv = 1;
    else if (cls[i] == Bc::EN || cls[i] == Bc::AN)
      lv = 2;
    level[i] = lv;
    top = std::max(top, lv);
  }
  for (std::size_t i = n; i > 0 && classify(text[i - 1]) == Bc::WS; --i)
    level[i - 1] = e;
  // L2. From the highest level down to the lowest odd one, reverse every run
  // at that level or above.
  const std::span<std::uint8_t> lv{level.data(), n};
  for (std::uint8_t at = top; at >= 1; --at) {
    for (std::size_t i = 0; i < n;) {
      if (lv[i] < at) {
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j < n && lv[j] >= at)
        ++j;
      reverse_range(text, lv, i, j);
      i = j;
    }
  }
  // L3. A reversed letter now follows its marks, put it back in front so a
  // mark draws over the glyph before it. Then L4 mirrors paired punctuation.
  for (std::size_t i = 0; i < n; ++i) {
    if ((lv[i] & 1U) == 0 || !is_mark(text[i]))
      continue;
    std::size_t j = i;
    while (j < n && (lv[j] & 1U) != 0 && is_mark(text[j]))
      ++j;
    if (j < n && (lv[j] & 1U) != 0) {
      reverse_range(text, lv, i, j + 1);
      i = j;
    }
  }
  for (std::size_t i = 0; i < n; ++i)
    if ((lv[i] & 1U) != 0)
      text[i] = mirror(text[i]);
  return {};
}

std::expected<std::span<const char32_t>, Error>
to_visual(std::string_view utf8, std::span<char32_t> out, Direction base,
          OnMalformed policy) {
  const std::expected<std::size_t, Error> n = decode_utf8(utf8, out, policy);
  if (!n)
    return std::unexpected(n.error());
  const std::size_t m = shape_arabic(out.first(*n));
  const std::span<char32_t> line = out.first(m);
  if (const std::expected<void, Error> r = reorder(line, base); !r)
    return std::unexpected(r.error());
  return line;
}

bool has_rtl(std::string_view utf8) {
  for (std::size_t i = 0; i < utf8.size();) {
    const std::optional<char32_t> cp = next_codepoint(utf8, i);
    if (cp && is_rtl(*cp))
      return true;
  }
  return false;
}

} // namespace opensensor::text
