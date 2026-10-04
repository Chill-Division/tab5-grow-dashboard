// Helpers for dashboard.yaml: 6-hour trend buffers, Home Assistant history backfill and
// the chart renderer. Header-only; the dashboard's lambdas call these directly.
//
// A Trend holds one value per 5-minute bucket for the last 6 hours. The buckets line up
// with Home Assistant's short-term statistics (also 5-minute, aligned to the clock), so
// history pulled from recorder.get_statistics and samples taken on the device land in
// the same slots and mean the same thing.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "esphome/components/json/json_util.h"
#include "esphome/core/color.h"
#include "esphome/core/time.h"
#include "lvgl.h"

namespace esphome::grow_dashboard {

static constexpr int64_t BUCKET_SECONDS = 300;  // HA short-term statistics period
static constexpr int BUCKETS = 72;              // 6 h of completed buckets
static constexpr int POINTS = BUCKETS + 1;      // plus the bucket in progress
static constexpr int MAX_BRIDGE = 3;            // draw the line across gaps of up to 15 min

struct Trend {
  // Oldest first, NAN where there is no data. The last slot is the bucket in progress.
  float values[POINTS];
  int64_t head{0};  // bucket number (unix time / BUCKET_SECONDS) of the last slot
  double sum{0};    // running mean of the bucket in progress
  uint32_t count{0};
  bool dirty{true};

  Trend() { std::fill(std::begin(this->values), std::end(this->values), NAN); }

  // Slide the window forward so that the last slot is the bucket containing `now`.
  void advance(int64_t now) {
    const int64_t bucket = now / BUCKET_SECONDS;
    if (bucket <= this->head)
      return;  // same bucket, or the clock stepped backwards
    const int64_t shift = this->head == 0 ? POINTS : bucket - this->head;
    if (shift >= POINTS) {
      std::fill(std::begin(this->values), std::end(this->values), NAN);
    } else {
      std::move(this->values + shift, this->values + POINTS, this->values);
      std::fill(this->values + POINTS - shift, this->values + POINTS, NAN);
    }
    this->head = bucket;
    this->sum = 0;
    this->count = 0;
    this->dirty = true;
  }

  // Fold a live reading into the running mean of the bucket in progress.
  void add_sample(int64_t now, float value) {
    this->advance(now);
    if (std::isnan(value))
      return;
    this->sum += value;
    this->count++;
    this->values[POINTS - 1] = this->sum / this->count;
    this->dirty = true;
  }

  // A completed 5-minute mean from HA. Buckets the device already holds are left alone.
  void backfill(int64_t bucket, float mean) {
    const int64_t slot = (POINTS - 1) - (this->head - bucket);
    if (slot < 0 || slot >= POINTS - 1 || std::isnan(mean) || !std::isnan(this->values[slot]))
      return;
    this->values[slot] = mean;
    this->dirty = true;
  }

  bool has_gaps() const {
    return std::any_of(this->values, this->values + POINTS - 1, [](float v) { return std::isnan(v); });
  }
};

// The trend behind each tile, keyed by HA entity ID. Created on first use.
inline std::map<std::string, Trend> &trends() {
  static std::map<std::string, Trend> all;
  return all;
}
inline Trend &trend(const std::string &entity_id) { return trends()[entity_id]; }

// Whether any trend is missing completed buckets that HA's statistics could fill.
inline bool any_gaps() {
  return trends().empty() ||
         std::any_of(trends().begin(), trends().end(), [](const auto &entry) { return entry.second.has_gaps(); });
}

// Keeps every trend's window moving while no live readings are coming in.
inline void advance_all(int64_t now) {
  for (auto &entry : trends())
    entry.second.advance(now);
}

// `stats` is the reply shaped by the YAML's response_template:
// {"sensor.x": [[bucket, mean], ...], ...}
inline void backfill(JsonObjectConst stats, int64_t now) {
  for (JsonPairConst entity : stats) {
    Trend &t = trend(entity.key().c_str());
    t.advance(now);
    for (JsonArrayConst row : entity.value().as<JsonArrayConst>())
      t.backfill(row[0].as<int32_t>(), row[1].as<float>());
  }
}

// "1,045", "1.12" or "--"
inline void format_value(char *out, size_t len, float value, int decimals) {
  if (std::isnan(value)) {
    snprintf(out, len, "--");
  } else if (decimals == 0 && std::fabs(value) >= 1000.0f && std::fabs(value) < 1e6f) {
    const long n = lroundf(value);
    snprintf(out, len, "%s%ld,%03ld", n < 0 ? "-" : "", std::labs(n) / 1000, std::labs(n) % 1000);
  } else {
    snprintf(out, len, "%.*f", decimals, value);
  }
}

inline lv_color_t to_lv(Color c) { return lv_color_make(c.r, c.g, c.b); }

// Sets the big number on a tile, and swaps the target hint for an amber High/Low chip
// while the value is outside the target range.
inline void show_value(lv_obj_t *value_label, lv_obj_t *target_label, lv_obj_t *status_chip, float value,
                       int decimals, float target_low, float target_high) {
  char text[24];
  format_value(text, sizeof(text), value, decimals);
  lv_label_set_text(value_label, text);

  const char *status = nullptr;
  if (!std::isnan(value) && target_high > target_low) {
    if (value > target_high) {
      status = "▲ High";
    } else if (value < target_low) {
      status = "▼ Low";
    }
  }
  if (status != nullptr) {
    lv_label_set_text(status_chip, status);
    lv_obj_remove_flag(status_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(target_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(status_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(target_label, LV_OBJ_FLAG_HIDDEN);
  }
}

// Colours the charts share. Set from the YAML's color: entries at boot, so the chart
// background always matches the card it sits on.
struct Theme {
  Color surface{0x16, 0x1B, 0x22};    // card background
  Color grid{0x26, 0x2D, 0x36};       // hourly gridlines
  Color band{0x15, 0x2B, 0x1F};       // target band fill
  Color band_edge{0x1F, 0x5C, 0x2E};  // target band edges
};
inline Theme theme;

namespace detail {

struct Rgb {
  float r, g, b;
};

inline Rgb rgb(Color c) { return {(float) c.r, (float) c.g, (float) c.b}; }

inline Rgb mix(const Rgb &a, const Rgb &b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// LV_COLOR_FORMAT_RGB565, which is what a non-transparent canvas uses at 16-bit colour depth
inline uint16_t rgb565(const Rgb &c) {
  const auto r = (uint16_t) (c.r + 0.5f), g = (uint16_t) (c.g + 0.5f), b = (uint16_t) (c.b + 0.5f);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

}  // namespace detail

// Renders `trend` into `canvas`: target band, hourly gridlines, an area wash under the
// line, the line itself and a dot on the latest value. The y scale covers both the data
// and the target band, so you can see where the reading sits relative to target.
// `min_span` stops sensor noise being stretched to fill the chart. `range_label` shows
// the 6-hour low and high through `range_format`, which takes them as two %s. Does
// nothing unless the trend changed since the last draw; returns whether it drew.
inline bool draw_trend(lv_obj_t *canvas, lv_obj_t *range_label, Trend &trend, Color line_color, float target_low,
                       float target_high, float min_span, int decimals, const char *range_format) {
  if (!trend.dirty)
    return false;
  lv_draw_buf_t *buf = lv_canvas_get_draw_buf(canvas);
  if (buf == nullptr)
    return false;
  trend.dirty = false;

  const int width = buf->header.w;
  const int height = buf->header.h;
  const float *v = trend.values;

  float lo = INFINITY, hi = -INFINITY;
  for (float x : trend.values) {
    if (!std::isnan(x)) {
      lo = std::min(lo, x);
      hi = std::max(hi, x);
    }
  }
  const bool have_data = lo <= hi;

  if (range_label != nullptr) {
    if (have_data) {
      char low_text[24], high_text[24];
      format_value(low_text, sizeof(low_text), lo, decimals);
      format_value(high_text, sizeof(high_text), hi, decimals);
      lv_label_set_text_fmt(range_label, range_format, low_text, high_text);
    } else {
      lv_label_set_text(range_label, "Waiting for data");
    }
  }

  // Y scale: the data plus the target band, at least min_span tall, with some headroom
  const bool band = target_high > target_low;
  float y_min = have_data ? lo : target_low, y_max = have_data ? hi : target_high;
  if (band) {
    y_min = std::min(y_min, target_low);
    y_max = std::max(y_max, target_high);
  }
  if (!have_data && !band) {
    y_min = 0;
    y_max = 1;
  }
  if (y_max - y_min < min_span) {
    const float mid = (y_min + y_max) / 2;
    y_min = mid - min_span / 2;
    y_max = mid + min_span / 2;
  }
  const float headroom = (y_max - y_min) * 0.12f;
  y_min -= headroom;
  y_max += headroom;

  // Plot geometry. The latest point sits at `right`, leaving room for the end dot.
  static constexpr int LINE_WIDTH = 5;
  static constexpr int DOT_RADIUS = 9;  // includes the 3 px ring in the card colour
  static constexpr int GRID_WIDTH = 2;
  static constexpr float WASH = 0.30f;  // area opacity just under the line, fading to 0
  const float left = LINE_WIDTH / 2.0f;
  const float right = width - 1 - DOT_RADIUS;
  const float top = DOT_RADIUS;
  const float bottom = height - 1 - LINE_WIDTH / 2.0f;
  const float step = (right - left) / (POINTS - 1);
  auto x_at = [&](int i) { return left + i * step; };
  auto y_at = [&](float value) { return top + (y_max - value) / (y_max - y_min) * (bottom - top); };

  // Nearest point with data at or before / at or after each slot
  int prev[POINTS], next[POINTS];
  for (int i = 0, last = -1; i < POINTS; i++) {
    if (!std::isnan(v[i]))
      last = i;
    prev[i] = last;
  }
  for (int i = POINTS - 1, last = -1; i >= 0; i--) {
    if (!std::isnan(v[i]))
      last = i;
    next[i] = last;
  }
  auto joined = [](int a, int b) { return a >= 0 && b > a && b - a <= MAX_BRIDGE + 1; };

  // Height of the line in every pixel column, NAN where there is no line
  std::vector<float> curve(width, NAN);
  for (int x = 0; x < width; x++) {
    const float f = (x - left) / step;
    if (f < 0 || f > POINTS - 1)
      continue;
    const int i = std::min((int) f, POINTS - 2);
    const int a = prev[i], b = next[i + 1];
    if (!joined(a, b))
      continue;
    curve[x] = y_at(v[a] + (v[b] - v[a]) * ((f - a) / (b - a)));
  }

  // Gridlines on each local hour boundary inside the window
  std::vector<uint8_t> grid(width, 0);
  if (trend.head > 0) {
    const int64_t t_left = (trend.head - (POINTS - 1)) * BUCKET_SECONDS;
    const int64_t t_right = trend.head * BUCKET_SECONDS;
    const int64_t tz = ESPTime::timezone_offset();
    int64_t hour = (t_left + tz) / 3600 * 3600 - tz;
    if (hour < t_left)
      hour += 3600;
    for (; hour <= t_right; hour += 3600) {
      const int gx = (int) lroundf(left + (float) (hour - t_left) / (t_right - t_left) * (right - left));
      for (int k = gx - GRID_WIDTH / 2; k < gx - GRID_WIDTH / 2 + GRID_WIDTH; k++) {
        if (k >= 0 && k < width)
          grid[k] = 1;
      }
    }
  }

  // Background, band, gridlines and the area wash, pixel by pixel
  const detail::Rgb surface = detail::rgb(theme.surface), grid_rgb = detail::rgb(theme.grid),
                    band_rgb = detail::rgb(theme.band), edge_rgb = detail::rgb(theme.band_edge),
                    ink = detail::rgb(line_color);
  const int band_top = band ? (int) lroundf(y_at(target_high)) : -10;
  const int band_bottom = band ? (int) lroundf(y_at(target_low)) : -10;
  for (int y = 0; y < height; y++) {
    auto *row = reinterpret_cast<uint16_t *>(buf->data + y * buf->header.stride);
    const bool edge = band && (std::abs(y - band_top) <= 1 || std::abs(y - band_bottom) <= 1);
    const bool in_band = band && y > band_top && y < band_bottom;
    for (int x = 0; x < width; x++) {
      detail::Rgb c = edge ? edge_rgb : grid[x] ? grid_rgb : in_band ? band_rgb : surface;
      const float yc = curve[x];
      if (!std::isnan(yc) && y >= yc)
        c = detail::mix(c, ink, WASH * (1.0f - (y - yc) / (height - yc)));
      row[x] = detail::rgb565(c);
    }
  }

  // The line and the end dot, anti-aliased by LVGL
  lv_layer_t layer;
  lv_canvas_init_layer(canvas, &layer);
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = to_lv(line_color);
  line.width = LINE_WIDTH;
  line.round_start = 1;
  line.round_end = 1;
  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.radius = LV_RADIUS_CIRCLE;
  dot.bg_color = to_lv(line_color);
  dot.bg_opa = LV_OPA_COVER;

  for (int i = 0; i < POINTS; i++) {
    if (std::isnan(v[i]))
      continue;
    const int j = i + 1 < POINTS ? next[i + 1] : -1;
    if (joined(i, j)) {
      line.p1.x = (lv_value_precise_t) lroundf(x_at(i));
      line.p1.y = (lv_value_precise_t) lroundf(y_at(v[i]));
      line.p2.x = (lv_value_precise_t) lroundf(x_at(j));
      line.p2.y = (lv_value_precise_t) lroundf(y_at(v[j]));
      lv_draw_line(&layer, &line);
    } else if (!joined(i > 0 ? prev[i - 1] : -1, i)) {
      // An isolated reading: no neighbour close enough to join, so show it as a dot
      const int32_t cx = lroundf(x_at(i)), cy = lroundf(y_at(v[i])), r = LINE_WIDTH / 2 + 1;
      const lv_area_t area = {cx - r, cy - r, cx + r, cy + r};
      lv_draw_rect(&layer, &dot, &area);
    }
  }

  const int last = prev[POINTS - 1];
  if (last >= 0) {
    const int32_t cx = lroundf(x_at(last)), cy = lroundf(y_at(v[last]));
    const lv_area_t area = {cx - DOT_RADIUS, cy - DOT_RADIUS, cx + DOT_RADIUS, cy + DOT_RADIUS};
    lv_draw_rect_dsc_t end = dot;
    end.border_color = to_lv(theme.surface);
    end.border_width = 3;
    end.border_opa = LV_OPA_COVER;
    lv_draw_rect(&layer, &end, &area);
  }
  lv_canvas_finish_layer(canvas, &layer);
  lv_obj_invalidate(canvas);
  return true;
}

}  // namespace esphome::grow_dashboard
