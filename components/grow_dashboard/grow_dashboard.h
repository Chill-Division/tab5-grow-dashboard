// Helpers for the dashboard packages: 6-hour trend buffers, Home Assistant history
// backfill, the chart renderer and the tile readouts. Header-only; the layouts register
// their sensors and charts at boot and the shared intervals in parts/base.yaml do the rest.
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
#include "esphome/components/sensor/sensor.h"
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
  uint32_t version{1};  // bumped on every change; a chart redraws when this moves on

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
    this->version++;
  }

  // Fold a live reading into the running mean of the bucket in progress.
  void add_sample(int64_t now, float value) {
    this->advance(now);
    if (std::isnan(value))
      return;
    this->sum += value;
    this->count++;
    this->values[POINTS - 1] = this->sum / this->count;
    this->version++;
  }

  // A completed 5-minute mean from HA. Buckets the device already holds are left alone.
  void backfill(int64_t bucket, float mean) {
    const int64_t slot = (POINTS - 1) - (this->head - bucket);
    if (slot < 0 || slot >= POINTS - 1 || std::isnan(mean) || !std::isnan(this->values[slot]))
      return;
    this->values[slot] = mean;
    this->version++;
  }

  bool has_gaps() const {
    return std::any_of(this->values, this->values + POINTS - 1, [](float v) { return std::isnan(v); });
  }
};

// The trend behind each charted entity, keyed by HA entity ID. Created on first use.
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

// Every charted entity as a Python-style list, for recorder.get_statistics' statistic_ids.
inline std::string statistic_ids() {
  std::string out = "[";
  for (const auto &entry : trends()) {
    if (out.size() > 1)
      out += ", ";
    out += "'" + entry.first + "'";
  }
  return out + "]";
}

// `stats` is the "stats" part of the reply shaped by the response_template in
// parts/base.yaml: {"sensor.x": [[offset, mean], ...], ...}, where `base` + offset is the
// 5-minute bucket number.
inline void backfill(JsonObjectConst stats, int32_t base, int64_t now) {
  for (JsonPairConst entity : stats) {
    Trend &t = trend(entity.key().c_str());
    t.advance(now);
    for (JsonArrayConst row : entity.value().as<JsonArrayConst>())
      t.backfill(base + row[0].as<int32_t>(), row[1].as<float>());
  }
}

// A Home Assistant sensor feeding the trend of its entity.
struct Track {
  sensor::Sensor *sensor;
  Trend *trend;
};
inline std::vector<Track> &tracks() {
  static std::vector<Track> all;
  return all;
}
inline void track(sensor::Sensor *sensor, const std::string &entity_id) {
  tracks().push_back({sensor, &trend(entity_id)});
}

// Feeds every tracked sensor into its trend. While HA is disconnected the readings are
// stale, so the windows only move on and the gap is filled from HA's statistics later.
inline void sample_all(int64_t now, bool live) {
  for (const Track &t : tracks()) {
    if (live) {
      t.trend->add_sample(now, t.sensor->state);
    } else {
      t.trend->advance(now);
    }
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

// The mean of the readings that are available, NAN if neither is.
inline float mean_of(float a, float b) {
  if (std::isnan(a))
    return b;
  if (std::isnan(b))
    return a;
  return (a + b) / 2;
}

inline lv_color_t to_lv(Color c) { return lv_color_make(c.r, c.g, c.b); }

// Swaps a tile's target hint for an amber High/Low chip while `value` is out of range.
inline void show_status(lv_obj_t *target_label, lv_obj_t *status_chip, float value, float target_low,
                        float target_high) {
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

// The big number on a single-sensor tile, plus its High/Low status.
inline void show_value(lv_obj_t *value_label, lv_obj_t *target_label, lv_obj_t *status_chip, float value,
                       int decimals, float target_low, float target_high) {
  char text[24];
  format_value(text, sizeof(text), value, decimals);
  lv_label_set_text(value_label, text);
  show_status(target_label, status_chip, value, target_low, target_high);
}

// Both probes of a zone tile. The High/Low status follows the zone average.
inline void show_pair(lv_obj_t *value1, lv_obj_t *value2, lv_obj_t *target_label, lv_obj_t *status_chip, float v1,
                      float v2, int decimals, float target_low, float target_high) {
  char text[24];
  format_value(text, sizeof(text), v1, decimals);
  lv_label_set_text(value1, text);
  format_value(text, sizeof(text), v2, decimals);
  lv_label_set_text(value2, text);
  show_status(target_label, status_chip, mean_of(v1, v2), target_low, target_high);
}

// "21.3 °C" from two probes, or "--" while neither has a reading.
inline void show_average(lv_obj_t *label, float v1, float v2, int decimals, const char *unit) {
  const float mean = mean_of(v1, v2);
  char text[32];
  if (std::isnan(mean)) {
    snprintf(text, sizeof(text), "--");
  } else {
    snprintf(text, sizeof(text), "%.*f %s", decimals, mean, unit);
  }
  lv_label_set_text(label, text);
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

struct Series {
  Trend *trend;
  Color color;
};

struct Chart {
  lv_obj_t *canvas;
  lv_obj_t *range_label;     // nullptr for no range text
  const char *range_format;  // takes the 6-hour low and high as two %s
  std::vector<Series> series;
  float target_low;
  float target_high;
  float min_span;  // smallest y span, so sensor noise isn't stretched to fill the chart
  int decimals;
  std::vector<uint32_t> drawn;  // each trend's version at the last draw
};

inline std::vector<Chart> &charts() {
  static std::vector<Chart> all;
  return all;
}

// One trend with an area wash under its line, and its 6-hour low and high in `range_label`.
inline void add_chart(lv_obj_t *canvas, lv_obj_t *range_label, const char *range_format,
                      const std::string &entity_id, Color color, float target_low, float target_high,
                      float min_span, int decimals) {
  charts().push_back(Chart{canvas, range_label, range_format, {{&trend(entity_id), color}}, target_low, target_high,
                           min_span, decimals, {0}});
}

// Two trends on one scale, such as the two probes in a zone: lines only, no wash.
inline void add_chart(lv_obj_t *canvas, const std::string &entity1, Color color1, const std::string &entity2,
                      Color color2, float target_low, float target_high, float min_span, int decimals) {
  charts().push_back(Chart{canvas,
                           nullptr,
                           nullptr,
                           {{&trend(entity1), color1}, {&trend(entity2), color2}},
                           target_low,
                           target_high,
                           min_span,
                           decimals,
                           {0, 0}});
}

// Renders a chart: target band, hourly gridlines, an area wash under a single line, the
// lines and a dot on each latest value. The y scale covers the data and the target band,
// so you can see where the readings sit relative to target.
inline void render(Chart &chart) {
  lv_draw_buf_t *buf = lv_canvas_get_draw_buf(chart.canvas);
  if (buf == nullptr)
    return;
  const int width = buf->header.w;
  const int height = buf->header.h;
  const size_t count = chart.series.size();

  // One time axis for every series, placed by bucket against the newest window
  int64_t head = 0;
  for (const Series &s : chart.series)
    head = std::max(head, s.trend->head);
  std::vector<float> v(count * POINTS, NAN);  // v[k * POINTS + i]: series k at slot i
  float lo = INFINITY, hi = -INFINITY;
  for (size_t k = 0; k < count; k++) {
    const Trend &t = *chart.series[k].trend;
    if (t.head == 0)
      continue;
    for (int i = 0; i < POINTS; i++) {
      const int64_t j = i - (head - t.head);
      if (j < 0 || j >= POINTS || std::isnan(t.values[j]))
        continue;
      v[k * POINTS + i] = t.values[j];
      lo = std::min(lo, t.values[j]);
      hi = std::max(hi, t.values[j]);
    }
  }
  const bool have_data = lo <= hi;

  if (chart.range_label != nullptr) {
    if (have_data) {
      char low_text[24], high_text[24];
      format_value(low_text, sizeof(low_text), lo, chart.decimals);
      format_value(high_text, sizeof(high_text), hi, chart.decimals);
      lv_label_set_text_fmt(chart.range_label, chart.range_format, low_text, high_text);
    } else {
      lv_label_set_text(chart.range_label, "Waiting for data");
    }
  }

  // Y scale: the data plus the target band, at least min_span tall, with some headroom
  const bool band = chart.target_high > chart.target_low;
  float y_min = have_data ? lo : chart.target_low, y_max = have_data ? hi : chart.target_high;
  if (band) {
    y_min = std::min(y_min, chart.target_low);
    y_max = std::max(y_max, chart.target_high);
  }
  if (!have_data && !band) {
    y_min = 0;
    y_max = 1;
  }
  if (y_max - y_min < chart.min_span) {
    const float mid = (y_min + y_max) / 2;
    y_min = mid - chart.min_span / 2;
    y_max = mid + chart.min_span / 2;
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
  auto joined = [](int a, int b) { return a >= 0 && b > a && b - a <= MAX_BRIDGE + 1; };

  // Nearest slot with data at or before / at or after each slot, per series
  std::vector<int> prev(count * POINTS), next(count * POINTS);
  for (size_t k = 0; k < count; k++) {
    const float *vk = &v[k * POINTS];
    for (int i = 0, last = -1; i < POINTS; i++) {
      if (!std::isnan(vk[i]))
        last = i;
      prev[k * POINTS + i] = last;
    }
    for (int i = POINTS - 1, last = -1; i >= 0; i--) {
      if (!std::isnan(vk[i]))
        last = i;
      next[k * POINTS + i] = last;
    }
  }

  // Height of a lone series' line in every pixel column (NAN where there is none), for the wash
  std::vector<float> curve(width, NAN);
  if (count == 1) {
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
  }

  // Gridlines on each local hour boundary inside the window
  std::vector<uint8_t> grid(width, 0);
  if (head > 0) {
    const int64_t t_left = (head - (POINTS - 1)) * BUCKET_SECONDS;
    const int64_t t_right = head * BUCKET_SECONDS;
    const int64_t tz = ESPTime::timezone_offset();
    int64_t hour = (t_left + tz) / 3600 * 3600 - tz;
    if (hour < t_left)
      hour += 3600;
    for (; hour <= t_right; hour += 3600) {
      const int gx = (int) lroundf(left + (float) (hour - t_left) / (t_right - t_left) * (right - left));
      for (int g = gx - GRID_WIDTH / 2; g < gx - GRID_WIDTH / 2 + GRID_WIDTH; g++) {
        if (g >= 0 && g < width)
          grid[g] = 1;
      }
    }
  }

  // Background, band, gridlines and the area wash, pixel by pixel
  const detail::Rgb surface = detail::rgb(theme.surface), grid_rgb = detail::rgb(theme.grid),
                    band_rgb = detail::rgb(theme.band), edge_rgb = detail::rgb(theme.band_edge),
                    ink = detail::rgb(chart.series[0].color);
  const int band_top = band ? (int) lroundf(y_at(chart.target_high)) : -10;
  const int band_bottom = band ? (int) lroundf(y_at(chart.target_low)) : -10;
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

  // The lines and end dots, anti-aliased by LVGL
  lv_layer_t layer;
  lv_canvas_init_layer(chart.canvas, &layer);
  for (size_t k = 0; k < count; k++) {
    const float *vk = &v[k * POINTS];
    const int *pk = &prev[k * POINTS], *nk = &next[k * POINTS];
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = to_lv(chart.series[k].color);
    line.width = LINE_WIDTH;
    line.round_start = 1;
    line.round_end = 1;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = to_lv(chart.series[k].color);
    dot.bg_opa = LV_OPA_COVER;

    for (int i = 0; i < POINTS; i++) {
      if (std::isnan(vk[i]))
        continue;
      const int j = i + 1 < POINTS ? nk[i + 1] : -1;
      if (joined(i, j)) {
        line.p1.x = (lv_value_precise_t) lroundf(x_at(i));
        line.p1.y = (lv_value_precise_t) lroundf(y_at(vk[i]));
        line.p2.x = (lv_value_precise_t) lroundf(x_at(j));
        line.p2.y = (lv_value_precise_t) lroundf(y_at(vk[j]));
        lv_draw_line(&layer, &line);
      } else if (!joined(i > 0 ? pk[i - 1] : -1, i)) {
        // An isolated reading: no neighbour close enough to join, so show it as a dot
        const int32_t cx = lroundf(x_at(i)), cy = lroundf(y_at(vk[i])), r = LINE_WIDTH / 2 + 1;
        const lv_area_t area = {cx - r, cy - r, cx + r, cy + r};
        lv_draw_rect(&layer, &dot, &area);
      }
    }

    const int last = pk[POINTS - 1];
    if (last >= 0) {
      const int32_t cx = lroundf(x_at(last)), cy = lroundf(y_at(vk[last]));
      const lv_area_t area = {cx - DOT_RADIUS, cy - DOT_RADIUS, cx + DOT_RADIUS, cy + DOT_RADIUS};
      lv_draw_rect_dsc_t end = dot;
      end.border_color = to_lv(theme.surface);
      end.border_width = 3;
      end.border_opa = LV_OPA_COVER;
      lv_draw_rect(&layer, &end, &area);
    }
  }
  lv_canvas_finish_layer(chart.canvas, &layer);
  lv_obj_invalidate(chart.canvas);
}

// Redraws charts whose data changed: the first one found, or all of them when `all` is
// set. One per call keeps each main-loop pass short, so touch and animations stay smooth.
// Returns whether anything was drawn.
inline bool draw_charts(bool all) {
  bool drew = false;
  for (Chart &chart : charts()) {
    bool changed = false;
    for (size_t k = 0; k < chart.series.size(); k++)
      changed |= chart.drawn[k] != chart.series[k].trend->version;
    if (!changed)
      continue;
    for (size_t k = 0; k < chart.series.size(); k++)
      chart.drawn[k] = chart.series[k].trend->version;
    render(chart);
    drew = true;
    if (!all)
      break;
  }
  return drew;
}

}  // namespace esphome::grow_dashboard
