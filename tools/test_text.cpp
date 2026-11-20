// Host tests for firmware/common/text, run by tools/test_text.py.

#include "../firmware/common/text/src/os_strings.h"
#include "../firmware/common/text/src/os_text.h"
#include "../firmware/common/text/src/os_text_font.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace opensensor::text;

namespace {

using Cps = std::vector<char32_t>;

Cps shaped(std::u32string_view in) {
  Cps t(in.begin(), in.end());
  t.resize(shape_arabic(t));
  return t;
}

Cps visual(std::string_view utf8, Direction base = Direction::Auto) {
  std::array<char32_t, kMaxLine> out{};
  const auto r = to_visual(utf8, out, base);
  assert(r.has_value());
  return {r->begin(), r->end()};
}

Cps decoded(std::string_view utf8, OnMalformed policy) {
  std::array<char32_t, 16> out{};
  const auto r = decode_utf8(utf8, out, policy);
  assert(r.has_value());
  return {out.begin(), out.begin() + static_cast<std::ptrdiff_t>(*r)};
}

Cps cps(std::initializer_list<char32_t> l) { return l; }

void test_joining_forms() {
  // Beh is dual joining, FE8F isolated, FE90 final, FE91 initial, FE92
  // medial.
  assert(shaped(U"\u0628") == cps({0xFE8F}));
  assert(shaped(U"\u0628\u0628") == cps({0xFE91, 0xFE90}));
  assert(shaped(U"\u0628\u0628\u0628") == cps({0xFE91, 0xFE92, 0xFE90}));
  // Alef joins only to the letter before it.
  assert(shaped(U"\u0628\u0627") == cps({0xFE91, 0xFE8E}));
  assert(shaped(U"\u0627\u0628") == cps({0xFE8D, 0xFE8F}));
  assert(shaped(U"\u0628\u0627\u0628") == cps({0xFE91, 0xFE8E, 0xFE8F}));
  // Hamza never joins, so the beh before it stays isolated.
  assert(shaped(U"\u0621") == cps({0xFE80}));
  assert(shaped(U"\u0628\u0621") == cps({0xFE8F, 0xFE80}));
  // Teh marbuta and yeh, the last letter of the range.
  assert(shaped(U"\u0628\u0629") == cps({0xFE91, 0xFE94}));
  assert(shaped(U"\u064A\u064A\u064A") == cps({0xFEF3, 0xFEF4, 0xFEF2}));
  // A space or a Latin letter breaks the join.
  assert(shaped(U"\u0628 \u0628") == cps({0xFE8F, U' ', 0xFE8F}));
  assert(shaped(U"\u0628a\u0628") == cps({0xFE8F, U'a', 0xFE8F}));
  // Tatweel joins on both sides and stays as it is.
  assert(shaped(U"\u0644\u0640") == cps({0xFEDF, 0x0640}));
  // Mafish, meem initial, feh medial, yeh medial, sheen final.
  assert(shaped(U"\u0645\u0641\u064A\u0634") ==
         cps({0xFEE3, 0xFED4, 0xFEF4, 0xFEB6}));
}

void test_lam_alef() {
  assert(shaped(U"\u0644\u0627") == cps({0xFEFB}));
  assert(shaped(U"\u0644\u0622") == cps({0xFEF5}));
  assert(shaped(U"\u0644\u0623") == cps({0xFEF7}));
  assert(shaped(U"\u0644\u0625") == cps({0xFEF9}));
  // After a joining letter the ligature takes its final form.
  assert(shaped(U"\u0628\u0644\u0627") == cps({0xFE91, 0xFEFC}));
  // The ligature does not join to the next letter.
  assert(shaped(U"\u0644\u0627\u0628") == cps({0xFEFB, 0xFE8F}));
  // A mark on the lam follows the ligature.
  assert(shaped(U"\u0644\u0651\u0627") == cps({0xFEFB, 0x0651}));
  // Lam before a letter that is not alef shapes normally.
  assert(shaped(U"\u0644\u0628") == cps({0xFEDF, 0xFE90}));
}

void test_marks() {
  // A fatha between two behs is transparent to joining.
  assert(shaped(U"\u0628\u064E\u0628") == cps({0xFE91, 0x064E, 0xFE90}));
  assert(shaped(U"\u0628\u0651\u064E\u0628") ==
         cps({0xFE91, 0x0651, 0x064E, 0xFE90}));
  assert(is_mark(0x064E) && is_mark(0x0670) && !is_mark(0x0628));
  // Reordered right to left, a mark stays after the letter it sits on.
  assert(visual("\u0628\u064E", Direction::Rtl) == cps({0xFE8F, 0x064E}));
  assert(visual("\u0628\u064E\u0628", Direction::Rtl) ==
         cps({0xFE90, 0xFE91, 0x064E}));
}

void test_mixed() {
  // mafish SD, the Latin run keeps its order at the visual left.
  assert(visual("\u0645\u0641\u064A\u0634 SD") ==
         cps({U'S', U'D', U' ', 0xFEB6, 0xFEF4, 0xFED4, 0xFEE3}));
  // A decimal number after Arabic stays in reading order.
  assert(visual("\u0623\u0639\u0644\u0649 12.5") ==
         cps({U'1', U'2', U'.', U'5', U' ', 0xFEF0, 0xFEE0, 0xFECB, 0xFE83}));
  // PM2.5 stays one left to right run.
  assert(visual("\u0639\u0637\u0644 PM2.5") ==
         cps({U'P', U'M', U'2', U'.', U'5', U' ', 0xFEDE, 0xFEC4, 0xFECB}));
  // A unit run with a slash and a superscript stays whole.
  assert(
      visual("12 \u00B5g/m\u00B3 \u0643\u0648\u064A\u0633", Direction::Rtl) ==
      cps({0xFEB2, 0xFEF3, 0xFEEE, 0xFEDB, U' ', 0xB5, U'g', U'/', U'm', 0xB3,
           U' ', U'1', U'2'}));
  // An Arabic word inside an English line is reversed in place.
  assert(visual("Device \u0645\u0641\u064A\u0634") ==
         cps({U'D', U'e', U'v', U'i', U'c', U'e', U' ', 0xFEB6, 0xFEF4, 0xFED4,
              0xFEE3}));
  // Al harara 24.3, the value is drawn left of the label.
  assert(visual("\u0627\u0644\u062D\u0631\u0627\u0631\u0629 24.3") ==
         cps({U'2', U'4', U'.', U'3', U' ', 0xFE93, 0xFEAD, 0xFE8D, 0xFEAE,
              0xFEA4, 0xFEDF, 0xFE8D}));
  // Brackets are mirrored inside a right to left line.
  const Cps p = visual("\u0645 (\u0628)", Direction::Rtl);
  assert(p == cps({U'(', 0xFE8F, U')', U' ', 0xFEE1}));
  // The Arabic comma separates without breaking the number order.
  assert(visual("6\u060C 7", Direction::Rtl) ==
         cps({U'7', U' ', 0x060C, U'6'}));
  assert(resolve_direction(cps({U'1', 0x0628}), Direction::Auto) ==
         Direction::Rtl);
  assert(resolve_direction(cps({U'1', U'2'}), Direction::Auto) ==
         Direction::Ltr);
}

void test_latin_passthrough() {
  constexpr std::string_view s =
      "PM2.5 12.4 \u00B5g/m\u00B3 CO\u2082 (ok) -2 h";
  std::array<char32_t, kMaxLine> raw{};
  const auto n = decode_utf8(s, raw);
  assert(n.has_value());
  const Cps want{raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(*n)};
  assert(visual(s) == want);
  assert(visual(s, Direction::Ltr) == want);
  assert(visual("") == Cps{});
  assert(!has_rtl(s));
}

void test_malformed() {
  constexpr OnMalformed kRep = OnMalformed::Replace;
  constexpr char32_t R = kReplacement;
  assert(decoded("\xF0\x9F\x98\x80", kRep) == cps({0x1F600}));
  assert(decoded("\xC2\xB5", kRep) == cps({0xB5}));
  assert(decoded("a\xC3", kRep) == cps({U'a', R}));
  assert(decoded("\xC0\xAF", kRep) == cps({R, R}));        // Overlong.
  assert(decoded("\xE0\x80\x80", kRep) == cps({R, R, R})); // Overlong.
  assert(decoded("\xED\xA0\x80", kRep) == cps({R, R, R})); // Surrogate.
  assert(decoded("\xF4\x90\x80\x80", kRep) == cps({R, R, R, R}));
  assert(decoded("\xF0\x9F\x98z", kRep) == cps({R, U'z'})); // Truncated.
  assert(decoded("\xFF\x80", kRep) == cps({R, R}));
  std::array<char32_t, 8> out{};
  for (const std::string_view bad :
       {"\xC3", "a\xFF", "\xE0\x80\x80", "\xED\xBF\xBF", "\x80"}) {
    const auto r = decode_utf8(bad, out);
    assert(!r && r.error() == Error::Malformed);
    const auto v = to_visual(bad, out);
    assert(!v && v.error() == Error::Malformed);
  }
  assert(!has_rtl("\xFF\xD8"));
  assert(has_rtl("\xFF\xD8\xA8"));
}

void test_overflow() {
  std::array<char32_t, 2> two{};
  const auto r = decode_utf8("abc", two);
  assert(!r && r.error() == Error::Overflow);
  const auto ok = decode_utf8("ab", two);
  assert(ok && *ok == 2);
  const auto v = to_visual("\u0645\u0641\u064A\u0634", two);
  assert(!v && v.error() == Error::Overflow);
  const auto m = decode_utf8("\xFF", two);
  assert(!m && m.error() == Error::Malformed);
  // The bidi pass stops past kMaxLine, even with a larger output span.
  std::array<char32_t, kMaxLine + 8> big{};
  const std::string longer(kMaxLine + 1, 'a');
  const auto b = to_visual(longer, big);
  assert(!b && b.error() == Error::Overflow);
  const auto e = to_visual(std::string(kMaxLine, 'a'), big);
  assert(e && e->size() == kMaxLine);
  // Lam-alef shrinks the line, so kMaxLine is counted after shaping.
  std::array<char32_t, 2 * kMaxLine> wide{};
  std::string ligatures;
  for (std::size_t i = 0; i < 100; ++i)
    ligatures += "\u0644\u0627";
  const auto l = to_visual(ligatures, wide);
  assert(l && l->size() == 100);
}

void test_has_rtl() {
  assert(!has_rtl("OpenSensor"));
  assert(has_rtl("\u0645\u0641\u064A\u0634 SD"));
  assert(has_rtl("\u05E9\u05DC\u05D5\u05DD"));
  assert(has_rtl("\uFEFB"));
  assert(!has_rtl("\u0661\u0662")); // Arabic-Indic digits are not letters.
  assert(is_rtl(0x0628) && is_rtl(0xFE8F) && !is_rtl(U'a'));
}

// The printf conversions of a format, "%.*s" style, in order.
std::vector<std::string> conversions(std::string_view s) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '%')
      continue;
    std::size_t j = i + 1;
    if (j < s.size() && s[j] == '%') {
      i = j;
      continue;
    }
    while (j < s.size() && std::string_view{"-+ #0123456789.*hlLjzt"}.find(
                               s[j]) != std::string_view::npos)
      ++j;
    // A % before a letter that is no conversion, like %RH, is literal text.
    if (j < s.size() && std::string_view{"diouxXfFeEgGaAcsp"}.find(s[j]) !=
                            std::string_view::npos) {
      out.emplace_back(s.substr(i, j + 1 - i));
      i = j;
    }
  }
  return out;
}

const Font *const kFonts[] = {&fonts::ar_body_s, &fonts::ar_heading_s,
                              &fonts::ar_body_l, &fonts::ar_heading_l};

void test_fonts() {
  static_assert(sizeof(Glyph) == 16);
  for (const Font *f : kFonts) {
    assert(!f->glyphs.empty() && f->bits != nullptr);
    assert(f->ascent > 0 && f->descent > 0);
    for (std::size_t i = 1; i < f->glyphs.size(); ++i)
      assert(f->glyphs[i - 1].codepoint < f->glyphs[i].codepoint);
    for (const Glyph &g : f->glyphs) {
      assert((g.advance == 0) == is_mark(g.codepoint));
      assert(g.width > 0 && g.height > 0);
    }
    assert(find_glyph(*f, U'a') == nullptr);
    const Glyph *lam_alef = find_glyph(*f, 0xFEFB);
    assert(lam_alef != nullptr && lam_alef->codepoint == 0xFEFB);
    for (char32_t d = 0x0660; d <= 0x0669; ++d)
      assert(find_glyph(*f, d) != nullptr);
  }
}

void test_string_table() {
  static_assert(kStringCount > 100);
  static_assert(text(Lang::En, StringId::Brand) == "OpenSensor");
  static_assert(text(Lang::ArEg, StringId::MetricPm25) == "PM2.5");
  static_assert(direction(Lang::ArEg, StringId::MetricPm25) == Direction::Ltr);
  static_assert(direction(Lang::ArEg, StringId::StatusTitle) == Direction::Rtl);
  static_assert(direction(Lang::En, StringId::StatusTitle) == Direction::Ltr);
  std::size_t arabic_glyphs = 0;
  for (std::size_t i = 0; i < kStringCount; ++i) {
    const auto id = static_cast<StringId>(i);
    const std::string_view en = text(Lang::En, id);
    const std::string_view ar = text(Lang::ArEg, id);
    assert(!en.empty() && !ar.empty());
    assert(!has_rtl(en));
    if (kKeepLtr[i]) {
      assert(en == ar);
    } else {
      assert(has_rtl(ar));
      assert(direction(Lang::ArEg, id) == Direction::Rtl);
    }
    if (conversions(en) != conversions(ar)) {
      std::fprintf(stderr, "string %zu, printf conversions differ\n", i);
      assert(false);
    }
    // Every Arabic code point the shaped line needs is baked in every size.
    // The rest, Latin, digits and units, comes from the board's Latin fonts.
    const Cps v = visual(ar, direction(Lang::ArEg, id));
    for (const char32_t c : v) {
      const bool arabic = (c >= 0x0600 && c <= 0x06FF) ||
                          (c >= 0xFB50 && c <= 0xFDFF) ||
                          (c >= 0xFE70 && c <= 0xFEFF);
      if (!arabic)
        continue;
      ++arabic_glyphs;
      for (const Font *f : kFonts) {
        if (find_glyph(*f, c) == nullptr) {
          std::fprintf(stderr,
                       "string %zu needs U+%04X, run pixi run "
                       "text-fonts\n",
                       i, static_cast<unsigned>(c));
          assert(false);
        }
      }
    }
  }
  assert(arabic_glyphs > 0);
  std::printf("text: %zu strings, %zu fonts of %zu glyphs\n", kStringCount,
              std::size(kFonts), fonts::ar_body_s.glyphs.size());
}

} // namespace

int main() {
  test_joining_forms();
  test_lam_alef();
  test_marks();
  test_mixed();
  test_latin_passthrough();
  test_malformed();
  test_overflow();
  test_has_rtl();
  test_fonts();
  test_string_table();
  std::puts("text tests passed");
  return 0;
}
