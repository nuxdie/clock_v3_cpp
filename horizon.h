#pragma once

// The event horizon: how the next few hours unfold, on one time scale along the bottom of the screen.
//
//   NOW ──[ rain until 17:00 ]──────── sunset 19:32 ──── dinner 20:00═════21:30 ───── call 22:30══ +4H
//
// Weather comes in as bands (rain or snow periods, from the 15-minute forecast); the sun as marks; the calendar as
// marks at their start with a span along the line to their end. The next few timed events are shown, the one going
// on now among them; when none falls inside the window, the next one within a day and a half waits at its far end
// ("Tomorrow 08:30–09:00"). The timeline only shows when there is something on it besides the sun.
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
constexpr std::time_t countdown = 15 * 60; // an event this close says how many minutes are left
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
  std::time_t at = 0, end = 0; // an event's end; the same as `at` for the sun and for events without a length
  std::string title, when;
  bool beyond = false; // later than the window: waits at its far end
  int lane = 0;        // events that overlap in time stack below the line: 0 on it, 1 just under, ...
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

// When an event is: "20:00–21:30", "In 12 min · until 21:30" in its last quarter hour, "Now · until 21:30" once it
// has begun, "Tomorrow 08:30–09:00" for the next day, "20:00" when it has no length. An end on a later day than its
// start, half a day or more after it, is shown with that day's name.
inline std::string whenFor(const Calendar::Event &e, std::time_t now) {
  auto day = [](std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm.tm_year * 400 + tm.tm_yday;
  };
  auto endText = [&] {
    if (day(e.end) == day(e.start) || e.end - e.start < 12 * 3600) return hhmm(e.end); // past midnight: still clear
    std::tm tm{};
    localtime_r(&e.end, &tm);
    static constexpr const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return std::format("{} {}", kDays[tm.tm_wday], hhmm(e.end));
  };
  if (e.start <= now && e.end > now) return "Now \xC2\xB7 until " + endText();
  if (e.start > now && e.start - now <= countdown) {
    const long mins = (long)(e.start - now + 59) / 60; // rounded up: "In 1 min" until it starts
    const std::string in = std::format("In {} min", mins);
    return e.end > e.start ? in + " \xC2\xB7 until " + endText() : in;
  }
  const std::string start = day(e.start) == day(now) ? hhmm(e.start) : "Tomorrow " + hhmm(e.start);
  return e.end > e.start ? start + "\xE2\x80\x93" + endText() : start;
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
    marks.push_back(
        {is ? Mark::Kind::Sunrise : Mark::Kind::Sunset, hi, hi, is ? "Sunrise" : "Sunset", hhmm(hi), false});
    was = is;
  }
  return marks;
}

inline Model modelFor(std::time_t now, const Precip &precip, const std::vector<Calendar::Event> &events, double lat,
                      double lon) {
  Model m;
  m.bands = bandsFor(precip, now);
  auto mark = [&](const Calendar::Event &e, bool beyond) {
    m.marks.push_back({looksLikeCall(e.title) ? Mark::Kind::Call : Mark::Kind::Event, e.start,
                       std::max(e.start, e.end), e.title, whenFor(e, now), beyond});
  };
  // Going on now (begun, not yet over), or starting inside the window.
  for (const Calendar::Event &e : events) {
    if (e.allDay || e.start > now + span || (e.start < now && e.end <= now)) continue;
    mark(e, false);
    if ((int)m.marks.size() == maxEvents) break;
  }
  // Lanes: each event takes the first one free at its start, so overlapping events sit apart rather than on top.
  std::vector<std::time_t> laneFree;
  for (Mark &mk : m.marks) {
    std::size_t lane = 0;
    while (lane < laneFree.size() && laneFree[lane] > mk.at)
      ++lane;
    if (lane == laneFree.size()) laneFree.push_back(0);
    laneFree[lane] = std::max(mk.end, mk.at + 60); // an event without a length still holds its dot's place
    mk.lane = (int)lane;
  }
  if (m.marks.empty()) {
    for (const Calendar::Event &e : events) {
      if (e.allDay || e.start <= now + span || e.start > now + lookAhead) continue;
      mark(e, true);
      break;
    }
  }
  m.relevant = !m.marks.empty() || !m.bands.empty();
  for (Mark &s : sunMarks(now, lat, lon))
    m.marks.push_back(std::move(s));
  return m;
}

// Today's all-day events (birthdays, holidays, trips), for the line under the date: "Mum's birthday · Trip to Rome,
// until Sun". Empty when there are none.
inline std::string allDayLine(const std::vector<Calendar::Event> &events, std::time_t now) {
  static constexpr const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  std::string line;
  for (const Calendar::Event &e : events) {
    if (!e.allDay || e.start > now || e.end <= now) continue;
    if (!line.empty()) line += "  \xC2\xB7  ";
    line += e.title;
    // The end is the day after the last one; a last day later than today is named.
    auto noon = [](std::time_t t) {
      std::tm tm{};
      localtime_r(&t, &tm);
      tm.tm_hour = 12, tm.tm_min = tm.tm_sec = 0, tm.tm_isdst = -1;
      return std::mktime(&tm);
    };
    const std::time_t last = noon(e.end - 12 * 3600);
    const long days = std::lround(std::difftime(last, noon(now)) / 86400.0);
    if (days <= 0) continue;
    std::tm tm{};
    localtime_r(&last, &tm);
    static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    line += days == 1  ? std::string(", until tomorrow")
            : days < 7 ? std::format(", until {}", kDays[tm.tm_wday])
                       : std::format(", until {} {}", tm.tm_mday, kMonths[tm.tm_mon]);
  }
  return line;
}

} // namespace Horizon
