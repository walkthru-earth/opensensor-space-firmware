#pragma once

// Where each page puts things, one table per size class. The pages read
// every coordinate, gap and font from here, so one renderer serves every
// panel. Values are pixels. A "dy" is from the top of its box, a "y" is a
// text baseline unless the comment says otherwise.
//
// A new size class is one more table in its own source file, so an image
// links only the table its profile names. Tune a table against the host
// preview, `pixi run epaper-ui-preview` writes every page at each size and
// rotation.

#include "epaper_ui/fonts.h"
#include "epaper_ui/geometry.h"
#include "epaper_ui/profile.h"

namespace opensensor::ui {

// The top bar with the brand, the UTC clock and the link tags.
struct HeaderLayout {
  int height = 0;   // The rule under the bar is drawn at this y.
  int baseline = 0; // Brand and clock.
  int clock_x = 0;  // Centre of the clock, off centre where tags crowd it.
  int margin = 0;   // Left and right edge of the bar and the pages.
  int tag_y = 0;    // Top of a tag box.
  int tag_h = 0;
  int tag_pad = 0; // Text inset on each side of a tag.
  int tag_baseline = 0;
  int tag_gap = 0; // Between two tags.
  // The battery icon at the right edge, drawn only when the board reports a
  // battery. Its body is centred on the tag boxes, the terminal nub sits on
  // its right and the percent follows it in the tag font.
  int battery_w = 0; // Body, the nub not included.
  int battery_h = 0;
  int battery_nub = 0;  // Nub width, its height is half the body's.
  int battery_line = 0; // Outline thickness, also the charging mark's.
  int battery_gap = 0;  // Between the icon and the percent.
};

// The PM2.5 hero on the dashboard, its band pill and the 2 h plot.
struct HeroLayout {
  Point label;      // Metric label baseline.
  int unit_gap = 0; // Between the label and its unit.
  Point value;      // Hero digits baseline.
  Point pill;       // Top left of the band pill.
  int pill_h = 0;
  int pill_pad = 0;      // Text inset on each side.
  int pill_baseline = 0; // From the pill top.
  Rect plot;
  int plot_caption_y = 0; // "Last 2 hours" and the max label.
  int plot_foot_dy = 0;   // Foot labels, below the plot bottom.
  int plot_rule_gap = 0;  // Dotted rule above, solid rule below.
  int plot_empty_dy = 0;  // "collecting", below the plot middle.
};

// The grid of metric tiles under the hero. One pixel rules part the tiles.
struct TileLayout {
  int top = 0;
  int w = 0;
  int h = 0;
  int cols = 0;
  int rows = 0;
  int pad = 0; // Left and right inset of the tile content.
  int label_dy = 0;
  int value_dy = 0;
  int spark_dy = 0; // Top of the sparkline.
  int spark_h = 0;
};

// Axes and ticks of a full line chart.
struct ChartStyle {
  int x_tick = 0;      // Length of an x tick under the frame.
  int x_label_gap = 0; // From the frame bottom to the label top.
  int y_tick_dx = 0;   // A y tick starts this far left of the plot.
  int y_tick_len = 0;
  int y_label_dx = 0;   // The right edge of a y label, left of the plot.
  int y_label_dy = 0;   // Its baseline below the tick.
  int tick_spacing = 0; // The plot height over this is the tick budget.
  int empty_dy = 0;     // The empty text, below the plot middle.
  int trace = 0;        // Trace thickness in pixels.
};

// Shared by the chart, status and pairing pages.
struct PageLayout {
  Point title;
  int page_number_y = 0; // Right aligned at the header margin.
  int footer_rule_y = 0;
  int footer_y = 0;
};

struct ChartPageLayout {
  int sub_gap = 0;    // Between the title and its unit line.
  int stats_y = 0;    // now, min and max.
  int stats_step = 0; // From one stat to the next.
  int stat_gap = 0;   // Between a stat name and its value.
  Rect chart;
};

struct StatusPageLayout {
  int top = 0;  // First row baseline.
  int step = 0; // Row pitch.
  int value_x = 0;
};

struct PairingLayout {
  int title_y = 0;
  int prompt_y = 0;
  int code_y = 0;
  int device_y = 0;
};

struct AsleepLayout {
  Rect card;
  int brand_dy = 0;
  int title_dy = 0;
  int hint_dy = 0;
  int rule_inset = 0;
  int rule_dy = 0;
  int lines_dy = 0; // First "name value" line.
  int line_step = 0;
  int name_gap = 0; // Between a name and its value.
};

// The settings popup, centred on the frame.
struct PopupLayout {
  int w = 0;
  int head = 0;   // Title band height.
  int row = 0;    // Row pitch.
  int foot = 0;   // Hint band height.
  int pad = 0;    // Text inset left and right.
  int margin = 0; // White margin around the frame.
  int title_dy = 0;
  int rule_inset = 0;
  int rule_dy = 0;   // Head rule above the first row, foot rule below the last.
  int bar_inset = 0; // The selection bar's inset from the frame.
  int row_baseline = 0;
  int pill_dy = 0;       // Edit pill top below the row top.
  int pill_shrink = 0;   // Edit pill height is the row less this.
  int pill_gap = 0;      // Between the arrows, the value and the pill edge.
  int pill_extra = 0;    // Pill width beyond the value and both arrows.
  int pill_overhang = 0; // Past the right text edge.
  int hint_dy = 0;       // Hint baseline above the bottom.

  [[nodiscard]] constexpr int height(int rows) const {
    return head + rows * row + foot;
  }
};

struct Layout {
  int width = 0;
  int height = 0;
  FontScale scale = FontScale::Compact;
  const FontSet &fonts;
  HeaderLayout header;
  HeroLayout hero;
  TileLayout tiles;
  PageLayout page;
  ChartPageLayout chart_page;
  ChartStyle chart;
  StatusPageLayout status;
  PairingLayout pairing;
  AsleepLayout asleep;
  PopupLayout popup;
};

// 400 x 300, compact fonts, and its portrait 300 x 400.
extern const Layout kLayout400x300;
extern const Layout kLayout300x400;
// 800 x 480, large fonts, and its portrait 480 x 800.
extern const Layout kLayout800x480;
extern const Layout kLayout480x800;

} // namespace opensensor::ui
