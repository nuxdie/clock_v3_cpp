#pragma once

// Upcoming events from iCalendar feeds (RFC 5545), such as a Google Calendar "secret address in iCal format".
//
// Only what a wall clock needs is read: each event's start, end and title. Recurring events are expanded over the
// window asked for (FREQ DAILY, WEEKLY, MONTHLY, YEARLY with INTERVAL, COUNT, UNTIL, BYDAY and BYMONTHDAY), without
// the dates in EXDATE and with moved or cancelled instances (RECURRENCE-ID) taken from their own entries. Times with
// a TZID are read as the clock's local time: the clock hangs where its calendar lives. This file has no SDL in it,
// so it can be checked on its own.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Calendar {

struct Event {
  std::time_t start = 0, end = 0;
  std::string title;
  bool allDay = false;
};

namespace detail {

struct Stamp {
  std::time_t t = 0;
  bool allDay = false;
  bool ok = false;
};

inline std::time_t fromLocal(int Y, int M, int D, int h, int m, int s) {
  std::tm tm{};
  tm.tm_year = Y - 1900, tm.tm_mon = M - 1, tm.tm_mday = D;
  tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = s, tm.tm_isdst = -1;
  return std::mktime(&tm);
}

// "20260924T200000Z" is UTC; "20260924T200000" (floating, or with a TZID) is local time; "20260924" is a whole day.
inline Stamp parseStamp(std::string_view v) {
  Stamp st;
  int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
  const std::string str(v);
  if (std::sscanf(str.c_str(), "%4d%2d%2dT%2d%2d%2d", &Y, &M, &D, &h, &m, &s) == 6) {
    if (str.size() > 15 && str[15] == 'Z') {
      std::tm tm{};
      tm.tm_year = Y - 1900, tm.tm_mon = M - 1, tm.tm_mday = D, tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = s;
      st.t = timegm(&tm);
    } else {
      st.t = fromLocal(Y, M, D, h, m, s);
    }
    st.ok = true;
  } else if (std::sscanf(str.c_str(), "%4d%2d%2d", &Y, &M, &D) == 3) {
    st.t = fromLocal(Y, M, D, 0, 0, 0);
    st.allDay = st.ok = true;
  }
  return st;
}

// "PT1H30M", "P1D", "P2W", "-PT15M" in seconds.
inline long parseDuration(std::string_view v) {
  long sign = 1, total = 0, n = 0;
  for (char c : v) {
    if (c == '-')
      sign = -1;
    else if (std::isdigit((unsigned char)c))
      n = n * 10 + (c - '0');
    else {
      switch (c) {
      case 'W':
        total += n * 7 * 86400;
        break;
      case 'D':
        total += n * 86400;
        break;
      case 'H':
        total += n * 3600;
        break;
      case 'M':
        total += n * 60;
        break;
      case 'S':
        total += n;
        break;
      default:
        break;
      }
      n = 0;
    }
  }
  return sign * total;
}

inline std::string unescape(std::string_view v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      const char c = v[++i];
      out += (c == 'n' || c == 'N') ? ' ' : c;
    } else {
      out += v[i];
    }
  }
  return out;
}

inline std::vector<std::string_view> split(std::string_view v, char sep) {
  std::vector<std::string_view> parts;
  std::size_t from = 0;
  while (from <= v.size()) {
    const std::size_t to = std::min(v.find(sep, from), v.size());
    parts.push_back(v.substr(from, to - from));
    from = to + 1;
  }
  return parts;
}

struct Rule {
  enum class Freq { None, Daily, Weekly, Monthly, Yearly } freq = Freq::None;
  int interval = 1;
  int count = 0;                          // 0: no limit
  std::time_t until = 0;                  // 0: no limit
  std::vector<std::pair<int, int>> byDay; // (ordinal or 0, weekday 0 = Sunday)
  std::vector<int> byMonthDay;
};

inline Rule parseRule(std::string_view v) {
  static constexpr std::string_view kDays[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  Rule r;
  for (std::string_view part : split(v, ';')) {
    const auto eq = part.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view key = part.substr(0, eq), val = part.substr(eq + 1);
    const std::string sval(val);
    if (key == "FREQ") {
      r.freq = val == "DAILY"     ? Rule::Freq::Daily
               : val == "WEEKLY"  ? Rule::Freq::Weekly
               : val == "MONTHLY" ? Rule::Freq::Monthly
               : val == "YEARLY"  ? Rule::Freq::Yearly
                                  : Rule::Freq::None;
    } else if (key == "INTERVAL") {
      r.interval = std::max(1, std::atoi(sval.c_str()));
    } else if (key == "COUNT") {
      r.count = std::max(0, std::atoi(sval.c_str()));
    } else if (key == "UNTIL") {
      const Stamp st = parseStamp(val);
      if (st.ok) r.until = st.allDay ? st.t + 86399 : st.t;
    } else if (key == "BYDAY") {
      for (std::string_view d : split(val, ',')) {
        if (d.size() < 2) continue;
        const int ord = d.size() > 2 ? std::atoi(std::string(d.substr(0, d.size() - 2)).c_str()) : 0;
        const std::string_view name = d.substr(d.size() - 2);
        for (int i = 0; i < 7; ++i)
          if (kDays[i] == name) r.byDay.push_back({ord, i});
      }
    } else if (key == "BYMONTHDAY") {
      for (std::string_view d : split(val, ','))
        r.byMonthDay.push_back(std::atoi(std::string(d).c_str()));
    }
  }
  return r;
}

inline int daysInMonth(int year, int month0) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month0 == 1 && leap ? 29 : kDays[month0];
}

// The starts of a recurring event, in order, from its first start until `to` (or the rule's own end). Instances keep
// their wall-clock time across daylight-saving changes.
inline std::vector<std::time_t> occurrences(std::time_t first, const Rule &r, std::time_t to) {
  std::vector<std::time_t> out;
  std::tm base{};
  localtime_r(&first, &base);
  auto at = [&](int year, int mon, int day) {
    std::tm tm = base;
    tm.tm_year = year, tm.tm_mon = mon, tm.tm_mday = day, tm.tm_isdst = -1;
    return std::mktime(&tm);
  };
  int emitted = 0;
  bool done = false;
  auto emit = [&](std::time_t t) {
    if (done || t < first) return;
    if ((r.until && t > r.until) || t > to || (r.count && emitted >= r.count)) {
      done = true;
      return;
    }
    ++emitted;
    out.push_back(t);
  };

  constexpr int kMaxPeriods = 20000; // a daily event from fifty years ago
  for (int p = 0; p < kMaxPeriods && !done; ++p) {
    const int step = p * r.interval;
    std::vector<std::time_t> period;
    switch (r.freq) {
    case Rule::Freq::Daily:
      period.push_back(at(base.tm_year, base.tm_mon, base.tm_mday + step));
      break;
    case Rule::Freq::Weekly: {
      if (r.byDay.empty()) {
        period.push_back(at(base.tm_year, base.tm_mon, base.tm_mday + 7 * step));
      } else {
        const int monday = base.tm_mday - (base.tm_wday + 6) % 7 + 7 * step; // weeks start on Monday
        for (const auto &[ord, wd] : r.byDay)
          period.push_back(at(base.tm_year, base.tm_mon, monday + (wd + 6) % 7));
      }
      break;
    }
    case Rule::Freq::Monthly: {
      const int months = base.tm_mon + step;
      const int year = base.tm_year + months / 12, mon = months % 12;
      const int dim = daysInMonth(year + 1900, mon);
      if (!r.byDay.empty()) {
        std::tm firstOfMonth = base;
        firstOfMonth.tm_year = year, firstOfMonth.tm_mon = mon, firstOfMonth.tm_mday = 1, firstOfMonth.tm_isdst = -1;
        std::mktime(&firstOfMonth);
        for (const auto &[ord, wd] : r.byDay) {
          const int firstDay = 1 + (wd - firstOfMonth.tm_wday + 7) % 7;
          std::vector<int> days;
          for (int d = firstDay; d <= dim; d += 7)
            days.push_back(d);
          if (ord == 0) {
            for (int d : days)
              period.push_back(at(year, mon, d));
          } else if (ord > 0 && ord <= (int)days.size()) {
            period.push_back(at(year, mon, days[ord - 1]));
          } else if (ord < 0 && -ord <= (int)days.size()) {
            period.push_back(at(year, mon, days[days.size() + ord]));
          }
        }
      } else {
        std::vector<int> days = r.byMonthDay;
        if (days.empty()) days.push_back(base.tm_mday);
        for (int d : days) {
          if (d < 0) d = dim + 1 + d;
          if (d >= 1 && d <= dim) period.push_back(at(year, mon, d)); // no 31st in a 30-day month
        }
      }
      break;
    }
    case Rule::Freq::Yearly:
      if (base.tm_mon == 1 && base.tm_mday == 29 && daysInMonth(base.tm_year + step + 1900, 1) != 29) break;
      period.push_back(at(base.tm_year + step, base.tm_mon, base.tm_mday));
      break;
    case Rule::Freq::None:
      return {first};
    }
    std::sort(period.begin(), period.end());
    for (std::time_t t : period)
      emit(t);
  }
  return out;
}

} // namespace detail

// The events in an iCalendar text that overlap [from, to], recurring ones expanded, sorted by start.
inline std::vector<Event> parse(std::string_view text, std::time_t from, std::time_t to) {
  using namespace detail;
  // Unfold: a line starting with a space or a tab continues the one before it.
  std::vector<std::string> lines;
  for (std::string_view raw : split(text, '\n')) {
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    if (!raw.empty() && (raw[0] == ' ' || raw[0] == '\t') && !lines.empty())
      lines.back().append(raw.substr(1));
    else
      lines.emplace_back(raw);
  }

  struct Entry {
    std::string uid, title;
    Stamp start, end, recurrenceId;
    long duration = -1;
    std::string rrule;
    std::vector<std::time_t> exdates;
    bool cancelled = false;
  };
  std::vector<Entry> entries;
  Entry cur;
  bool inEvent = false;
  int depth = 0; // nested components (VALARM) inside an event
  for (const std::string &line : lines) {
    if (line == "BEGIN:VEVENT") {
      cur = Entry{}, inEvent = true, depth = 0;
      continue;
    }
    if (!inEvent) continue;
    if (line == "END:VEVENT") {
      if (cur.start.ok) entries.push_back(std::move(cur));
      inEvent = false;
      continue;
    }
    if (line.starts_with("BEGIN:")) ++depth;
    if (line.starts_with("END:")) --depth;
    if (depth > 0 || line.starts_with("END:")) continue;

    // NAME;PARAM=...;PARAM="...":VALUE
    bool quoted = false;
    std::size_t colon = std::string::npos;
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') quoted = !quoted;
      if (line[i] == ':' && !quoted) {
        colon = i;
        break;
      }
    }
    if (colon == std::string::npos) continue;
    const std::string_view head(line.data(), colon), value(line.data() + colon + 1, line.size() - colon - 1);
    const std::string_view name = head.substr(0, head.find(';'));
    if (name == "SUMMARY")
      cur.title = unescape(value);
    else if (name == "UID")
      cur.uid = value;
    else if (name == "DTSTART")
      cur.start = parseStamp(value);
    else if (name == "DTEND")
      cur.end = parseStamp(value);
    else if (name == "DURATION")
      cur.duration = parseDuration(value);
    else if (name == "RRULE")
      cur.rrule = value;
    else if (name == "RECURRENCE-ID")
      cur.recurrenceId = parseStamp(value);
    else if (name == "STATUS")
      cur.cancelled = value == "CANCELLED";
    else if (name == "EXDATE") {
      for (std::string_view d : split(value, ','))
        if (const Stamp st = parseStamp(d); st.ok) cur.exdates.push_back(st.t);
    }
  }

  // Instances moved or cancelled on their own: the recurring event leaves those out.
  std::map<std::string, std::set<std::time_t>> replaced;
  for (const Entry &e : entries)
    if (e.recurrenceId.ok) replaced[e.uid].insert(e.recurrenceId.t);

  std::vector<Event> out;
  for (const Entry &e : entries) {
    if (e.cancelled) continue;
    const long length = e.end.ok          ? (long)(e.end.t - e.start.t)
                        : e.duration >= 0 ? e.duration
                        : e.start.allDay  ? 86400L
                                          : 0L;
    std::vector<std::time_t> starts{e.start.t};
    if (!e.rrule.empty() && !e.recurrenceId.ok) {
      const Rule rule = parseRule(e.rrule);
      if (rule.freq != Rule::Freq::None) starts = occurrences(e.start.t, rule, to);
    }
    const auto it = replaced.find(e.uid);
    for (std::time_t s : starts) {
      if (s > to || s + length < from) continue;
      if (!e.recurrenceId.ok && it != replaced.end() && it->second.contains(s)) continue;
      if (std::find(e.exdates.begin(), e.exdates.end(), s) != e.exdates.end()) continue;
      out.push_back({s, s + length, e.title.empty() ? "Event" : e.title, e.start.allDay});
    }
  }
  std::sort(out.begin(), out.end(), [](const Event &a, const Event &b) { return a.start < b.start; });
  return out;
}

} // namespace Calendar
