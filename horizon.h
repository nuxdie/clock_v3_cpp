#pragma once

// The event horizon: how the next few hours unfold, on one time scale along the bottom of the screen.
//
//   NOW ──[ rain until 17:00 ]──────── sunset 19:32 ──── dinner 20:00 ─────────── call 22:30 ── +4H
//
// Weather comes in as bands (rain or snow periods, from the 15-minute forecast); the sun and the calendar as marks.
// The next few timed events are shown; when none falls inside the window, the next one within a day and a half
// waits at its far end ("Tomorrow 08:30"). The timeline only shows when there is something on it besides the sun.
// This file has no SDL in it, so it can be checked on its own.

#include "astro.h"
#include "calendar.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <string>
#include <vector>

namespace Horizon {

constexpr std::time_t span = 4 * 3600; // NOW .. +4H
constexpr std::time_t step = 15 * 60;  // the precipitation forecast's resolution
constexpr int maxEvents = 3;
constexpr std::time_t lookAhead = 36 * 3600;
constexpr double sunriseElevation = -0.83; // the sun's centre, refraction included, at sunrise and sunset

struct Band {
  std::time_t from = 0, to = 0;
  bool snow = false;
  float peak = 0; // mm in the wettest 15 minutes
  std::string caption;
};

struct Mark {
  enum class Kind { Event, Call, Sunrise, Sunset } kind = Kind::Event;
  std::time_t at = 0;
  std::string title, when;
  bool beyond = false; // later than the window: waits at its far end
};

struct Model {
  std::vector<Band> bands;
  std::vector<Mark> marks; // events first (soonest first), then the sun
  bool relevant = false;   // anything besides the sun
};

// The forecast as the timeline needs it: precipitation per 15-minute step starting at `from`.
struct Precip {
  std::time_t from = 0;
  std::vector<float> mm;
  std::vector<bool> snow;
};

inline std::string hhmm(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  return std::format("{:02}:{:02}", tm.tm_hour, tm.tm_min);
}

// Calls get a handset rather than a calendar page.
inline bool looksLikeCall(const std::string &title) {
  std::string s;
  for (char c : title)
    s += (char)std::tolower((unsigned char)c);
  for (const char *w : {"call", "phone", "zoom", "meet", "teams", "skype", "facetime", "звонок", "созвон"})
    if (s.find(w) != std::string::npos) return true;
  return false;
}

// Rain and snow periods: wet steps, with dry gaps of a single step bridged so a shower does not read as two.
inline std::vector<Band> bandsFor(const Precip &p, std::time_t now) {
  std::vector<Band> bands;
  const std::time_t end = now + span;
  constexpr float wet = 0.05f;
  for (std::size_t i = 0; i < p.mm.size(); ++i) {
    const std::time_t a = p.from + (std::time_t)i * step, b = a + step;
    if (p.mm[i] < wet || b <= now || a >= end) continue;
    const bool snow = i < p.snow.size() && p.snow[i];
    if (!bands.empty() && a - bands.back().to <= step && bands.back().snow == snow) {
      bands.back().to = b;
      bands.back().peak = std::max(bands.back().peak, p.mm[i]);
    } else {
      bands.push_back({a, b, snow, p.mm[i], {}});
    }
  }
  for (Band &b : bands) {
    const char *what = b.snow ? "snow" : "rain";
    const std::string word = b.peak < 0.3f    ? std::format("Light {}", what)
                             : b.peak >= 1.0f ? std::format("Heavy {}", what)
                                              : std::string(b.snow ? "Snow" : "Rain");
    const bool fromNow = b.from <= now + step / 2, pastEnd = b.to >= end;
    b.caption = fromNow && pastEnd ? std::format("{} for hours", word)
                : fromNow          ? std::format("{} until {}", word, hhmm(b.to))
                : pastEnd          ? std::format("{} from {}", word, hhmm(b.from))
                                   : std::format("{} {}\xE2\x80\x93{}", word, hhmm(b.from), hhmm(b.to));
    b.from = std::max(b.from, now);
    b.to = std::min(b.to, end);
  }
  return bands;
}

// Sunrise and sunset inside the window, to the minute.
inline std::vector<Mark> sunMarks(std::time_t now, double lat, double lon) {
  std::vector<Mark> marks;
  auto above = [&](std::time_t t) { return Astro::skyAt(t, lat, lon).sun.elevation > sunriseElevation; };
  constexpr std::time_t probe = 5 * 60;
  bool was = above(now);
  for (std::time_t t = now + probe; t <= now + span; t += probe) {
    const bool is = above(t);
    if (is == was) continue;
    std::time_t lo = t - probe, hi = t;
    while (hi - lo > 30)
      (above((lo + hi) / 2) == was ? lo : hi) = (lo + hi) / 2;
    marks.push_back({is ? Mark::Kind::Sunrise : Mark::Kind::Sunset, hi, is ? "Sunrise" : "Sunset", hhmm(hi), false});
    was = is;
  }
  return marks;
}

inline Model modelFor(std::time_t now, const Precip &precip, const std::vector<Calendar::Event> &events, double lat,
                      double lon) {
  Model m;
  m.bands = bandsFor(precip, now);
  for (const Calendar::Event &e : events) {
    if (e.allDay || e.start < now || e.start > now + span) continue;
    m.marks.push_back(
        {looksLikeCall(e.title) ? Mark::Kind::Call : Mark::Kind::Event, e.start, e.title, hhmm(e.start), false});
    if ((int)m.marks.size() == maxEvents) break;
  }
  if (m.marks.empty()) {
    for (const Calendar::Event &e : events) {
      if (e.allDay || e.start <= now + span || e.start > now + lookAhead) continue;
      std::tm a{}, b{};
      localtime_r(&now, &a);
      localtime_r(&e.start, &b);
      const bool sameDay = a.tm_yday == b.tm_yday && a.tm_year == b.tm_year;
      m.marks.push_back({looksLikeCall(e.title) ? Mark::Kind::Call : Mark::Kind::Event, e.start, e.title,
                         sameDay ? hhmm(e.start) : "Tomorrow " + hhmm(e.start), true});
      break;
    }
  }
  m.relevant = !m.marks.empty() || !m.bands.empty();
  for (Mark &s : sunMarks(now, lat, lon))
    m.marks.push_back(std::move(s));
  return m;
}

} // namespace Horizon
