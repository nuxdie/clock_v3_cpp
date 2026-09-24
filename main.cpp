#define SDL_MAIN_USE_CALLBACKS 1

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cpr/cpr.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "assets_fonts.h"
#include "assets_icons.h"
#include "calendar.h"
#include "horizon.h"
#include "scene.h"

using json = nlohmann::json;

using WindowPtr = SdlPtr<SDL_Window, SDL_DestroyWindow>;
using RendererPtr = SdlPtr<SDL_Renderer, SDL_DestroyRenderer>;
using FontPtr = SdlPtr<TTF_Font, TTF_CloseFont>;

namespace Config {
constexpr int screen_width = Screen::width;
constexpr int screen_height = Screen::height;
constexpr const char *AppName = "Digital Clock v3";
constexpr const char *AppVersion = "0.4.0";

// Where the clock hangs: the forecast is for here, and the sun and moon are placed for here.
constexpr double latitude = 52.3738;
constexpr double longitude = 4.8910;

// GROQ_API_KEY is defined via CMake target_compile_definitions
#ifndef GROQ_API_KEY
constexpr const char *GroqApiKey = "";
#else
constexpr const char *GroqApiKey = GROQ_API_KEY;
#endif

// iCalendar feeds for the event horizon (e.g. Google Calendar's "secret address in iCal format"), separated by
// spaces. Baked in from CALENDAR_URL at build time like the Groq key; a CALENDAR_URL set at run time wins.
#ifndef CALENDAR_URL
constexpr const char *CalendarUrl = "";
#else
constexpr const char *CalendarUrl = CALENDAR_URL;
#endif
} // namespace Config

// ------------------------------------------------------------- weather ----

enum class Icon { ClearDay, ClearNight, PartlyDay, PartlyNight, Cloudy, Fog, Rain, Snow, Thunder, Wind, COUNT };

Icon iconFor(int code, bool day) {
  switch (code) {
  case 0:
    return day ? Icon::ClearDay : Icon::ClearNight;
  case 1:
  case 2:
    return day ? Icon::PartlyDay : Icon::PartlyNight;
  case 3:
    return Icon::Cloudy;
  case 45:
  case 48:
    return Icon::Fog;
  case 71:
  case 73:
  case 75:
  case 77:
  case 85:
  case 86:
    return Icon::Snow;
  case 95:
  case 96:
  case 99:
    return Icon::Thunder;
  default:
    return Icon::Rain; // drizzle 51-57, rain 61-67, showers 80-82
  }
}

std::string conditionText(int code) {
  switch (code) {
  case 0:
    return "clear sky";
  case 1:
    return "mainly clear";
  case 2:
    return "partly cloudy";
  case 3:
    return "overcast";
  case 45:
    return "fog";
  case 48:
    return "rime fog";
  case 51:
    return "light drizzle";
  case 53:
    return "drizzle";
  case 55:
    return "dense drizzle";
  case 61:
    return "light rain";
  case 63:
    return "rain";
  case 65:
    return "heavy rain";
  case 71:
    return "light snow";
  case 73:
    return "snow";
  case 75:
    return "heavy snow";
  case 80:
    return "rain showers";
  case 81:
    return "rain showers";
  case 82:
    return "violent rain showers";
  case 95:
    return "thunderstorm";
  case 96:
  case 99:
    return "thunderstorm with hail";
  default:
    return "unknown";
  }
}

std::string basicAdvice(double t) {
  if (t < -10) return "Heavy winter coat, hat, scarf and warm boots.";
  if (t < 0) return "A winter coat and warm layers.";
  if (t < 10) return "A warm coat and a hat.";
  if (t < 20) return "A light jacket or a sweater.";
  return "Light clothing is fine.";
}

constexpr std::array<std::string_view, 7> kWeekdays = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                       "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 12> kMonths = {"January",   "February", "March",    "April",
                                                      "May",       "June",     "July",     "August",
                                                      "September", "October",  "November", "December"};

std::string dateString(const std::tm &tm) {
  return std::format("{}, {} {} {}", kWeekdays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900);
}

// std::localtime returns shared static storage, which races between the render and weather threads;
// localtime_r fills a caller-owned struct.
std::tm localTime(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  return tm;
}

#ifdef APP_DEBUG
// Debug: APP_FAKE_TIME=HH:MM pins the clock to that time today, and APP_FAKE_TIME="YYYY-MM-DD HH:MM" to that
// moment on another day (the sun's path and the moon's phase follow the date), to check the sky at any hour.
std::time_t fakeTimeOffset() {
  static const std::time_t offset = [] {
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    const char *env = SDL_getenv("APP_FAKE_TIME");
    if (!env) return std::time_t{0};
    const std::time_t now = std::time(nullptr);
    std::tm tm = localTime(now);
    if (std::sscanf(env, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) == 5) {
      tm.tm_year = Y - 1900, tm.tm_mon = M - 1, tm.tm_mday = D;
    } else if (std::sscanf(env, "%d:%d", &h, &m) != 2) {
      return std::time_t{0};
    }
    tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = 0, tm.tm_isdst = -1;
    return std::mktime(&tm) - now;
  }();
  return offset;
}
std::time_t now() { return std::time(nullptr) + fakeTimeOffset(); }

// Debug: APP_FAKE_WEATHER=<WMO code> shows that weather, e.g. 0 clear, 2 partly cloudy, 3 overcast, 45 fog,
// 63 rain, 65 heavy rain, 75 snow, 95 thunderstorm; APP_FAKE_WIND=<m/s>[,<degrees from>] the wind.
int fakeWeatherCode() {
  static const int code = [] {
    const char *env = SDL_getenv("APP_FAKE_WEATHER");
    return env ? SDL_atoi(env) : -1;
  }();
  return code;
}
#else
std::time_t now() { return std::time(nullptr); }
#endif

std::tm localNow() { return localTime(now()); }

// Interruptible sleep: returns early when the owning std::jthread is asked to stop.
void sleepFor(const std::stop_token &stopToken, std::chrono::milliseconds duration) {
  std::mutex m;
  std::unique_lock lock(m);
  std::condition_variable_any().wait_for(lock, stopToken, duration, [] { return false; });
}

// Hard timeouts on every request, and an abort as soon as a stop is requested, so a hung server can neither
// freeze the worker thread nor hold up shutdown.
cpr::ProgressCallback abortOnStop(std::stop_token stopToken) {
  return cpr::ProgressCallback{[stopToken](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                           intptr_t) { return !stopToken.stop_requested(); }};
}
const auto kConnectTimeout = cpr::ConnectTimeout{std::chrono::seconds(15)};
const auto kTimeout = cpr::Timeout{std::chrono::seconds(60)};

std::time_t parseTs(const std::string &s) {
  std::tm tm{};
  int Y, M, D, h, mi;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &mi) != 5) return 0;
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_isdst = -1;
  return std::mktime(&tm);
}

struct WeatherState {
  bool valid = false;
  double temperature = 0;
  double windspeed = 0;
  double winddirection = 270; // degrees the wind comes from
  int weathercode = 0;
  std::string advice;
  Horizon::Precip precip; // the next few hours of rain and snow, in 15-minute steps
};

#ifdef APP_DEBUG
// A plausible forecast for APP_FAKE_WEATHER, so the whole screen can be checked without a network.
WeatherState fakeWeather(int code) {
  WeatherState w;
  w.valid = true;
  w.temperature = 14;
  w.windspeed = 5;
  if (const char *env = SDL_getenv("APP_FAKE_WIND")) {
    float speed = 5, dir = 270;
    if (std::sscanf(env, "%f,%f", &speed, &dir) >= 1) w.windspeed = speed, w.winddirection = dir;
  }
  w.weathercode = code;
  w.advice = basicAdvice(w.temperature);
  SceneState s;
  applyWeather(s, {true, code, 5, 270, 0});
  const std::time_t t = now();
  w.precip.from = t - t % Horizon::step;
  for (float mm : {0.3f, 0.7f, 1.1f, 1.3f, 0.9f, 0.5f, 0.2f, 0.0f}) {
    w.precip.mm.push_back(mm * (s.rainIntensity + s.snowIntensity));
    w.precip.snow.push_back(s.snowIntensity > 0.0f);
  }
  return w;
}

// Debug: APP_FAKE_EVENTS="20:00 Dinner;22:30 Call" puts those events at their next such time (today, or tomorrow
// once it has passed), so the event horizon can be checked without a calendar.
std::vector<Calendar::Event> fakeEvents() {
  std::vector<Calendar::Event> events;
  const char *env = SDL_getenv("APP_FAKE_EVENTS");
  if (!env) return events;
  const std::time_t t = now();
  for (std::string_view item : Calendar::detail::split(env, ';')) {
    int h = 0, m = 0, used = 0;
    const std::string s(item);
    if (std::sscanf(s.c_str(), " %d:%d %n", &h, &m, &used) < 2) continue;
    std::tm tm = localTime(t);
    tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = 0, tm.tm_isdst = -1;
    std::time_t at = std::mktime(&tm);
    if (at < t) {
      tm.tm_mday += 1, tm.tm_isdst = -1;
      at = std::mktime(&tm);
    }
    events.push_back({at, at + 3600, s.substr(used), false});
  }
  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
  return events;
}
#endif

// ------------------------------------------------------------- helpers ----

std::size_t utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

// The first `max` characters of a UTF-8 string, with an ellipsis when it had more.
std::string ellipsize(const std::string &s, std::size_t max) {
  std::size_t i = 0, n = 0;
  while (i < s.size() && n < max)
    i += utf8Len((unsigned char)s[i]), ++n;
  return i >= s.size() ? s : s.substr(0, i) + "\xE2\x80\xA6";
}

// Small line icons for the event horizon, drawn as distance fields (so they are smooth at any size) and baked into
// white-on-transparent textures once at startup, like the weather icons.
namespace Sdf {
inline float segment(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float h = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
  return std::hypot(px - ax - dx * h, py - ay - dy * h);
}
inline float roundBox(float px, float py, float cx, float cy, float hw, float hh, float r) {
  const float qx = std::abs(px - cx) - hw + r, qy = std::abs(py - cy) - hh + r;
  return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
// Distance to an arc of radius `rad` around (cx, cy) from angle a0 to a1 (degrees, y down).
inline float arc(float px, float py, float cx, float cy, float rad, float a0, float a1) {
  float a = std::atan2(py - cy, px - cx) * 180.0f / (float)M_PI;
  if (a < a0) a += 360.0f;
  if (a <= a1) return std::abs(std::hypot(px - cx, py - cy) - rad);
  auto end = [&](float deg) {
    const float t = deg * (float)M_PI / 180.0f;
    return std::hypot(px - cx - rad * std::cos(t), py - cy - rad * std::sin(t));
  };
  return std::min(end(a0), end(a1));
}
} // namespace Sdf

// A white texture whose alpha is 1 where `dist` (in pixels of a size×size canvas) is below 0, with a pixel of
// antialiasing.
template <typename F> TexturePtr bakeIcon(SDL_Renderer *r, int size, F dist) {
  SurfacePtr s(SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32));
  if (!s) return {};
  for (int y = 0; y < size; ++y) {
    auto *row = (Uint8 *)s->pixels + (std::size_t)y * s->pitch;
    for (int x = 0; x < size; ++x) {
      row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
      row[x * 4 + 3] = (Uint8)(std::clamp(0.5f - dist(x + 0.5f, y + 0.5f), 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  TexturePtr t(SDL_CreateTextureFromSurface(r, s.get()));
  if (t) SDL_SetTextureScaleMode(t.get(), SDL_SCALEMODE_LINEAR);
  return t;
}

// A soft drop shadow for a run of text: its alpha, padded by `radius` and box-blurred (three passes each way
// approximate a gaussian). Made once per text change, so it costs nothing per frame.
TexturePtr makeShadow(SDL_Renderer *r, SDL_Surface *text, int radius) {
  SurfacePtr src(SDL_ConvertSurface(text, SDL_PIXELFORMAT_RGBA32));
  if (!src) return {};
  const int pad = radius * 2, w = src->w + 2 * pad, h = src->h + 2 * pad;
  std::vector<float> a((std::size_t)w * h, 0.0f), tmp(a.size());
  for (int y = 0; y < src->h; ++y) {
    const auto *row = (const Uint8 *)src->pixels + (std::size_t)y * src->pitch;
    for (int x = 0; x < src->w; ++x)
      a[(std::size_t)(y + pad) * w + x + pad] = row[x * 4 + 3] / 255.0f;
  }
  auto pass = [&](std::vector<float> &from, std::vector<float> &to, bool horizontal) {
    const int n = horizontal ? w : h, lines = horizontal ? h : w;
    for (int l = 0; l < lines; ++l) {
      auto at = [&](std::vector<float> &v, int i) -> float & {
        return horizontal ? v[(std::size_t)l * w + i] : v[(std::size_t)i * w + l];
      };
      float sum = 0.0f;
      for (int i = -radius; i < n + radius; ++i) {
        if (i + radius < n) sum += at(from, i + radius);
        if (i - radius - 1 >= 0) sum -= at(from, i - radius - 1);
        if (i >= 0 && i < n) at(to, i) = sum / (2 * radius + 1);
      }
    }
  };
  for (int i = 0; i < 3; ++i) {
    pass(a, tmp, true);
    pass(tmp, a, false);
  }
  SurfacePtr out(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32));
  if (!out) return {};
  for (int y = 0; y < h; ++y) {
    auto *row = (Uint8 *)out->pixels + (std::size_t)y * out->pitch;
    for (int x = 0; x < w; ++x) {
      row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
      row[x * 4 + 3] = (Uint8)std::clamp(a[(std::size_t)y * w + x] * 255.0f, 0.0f, 255.0f);
    }
  }
  TexturePtr t(SDL_CreateTextureFromSurface(r, out.get()));
  if (t) SDL_SetTextureScaleMode(t.get(), SDL_SCALEMODE_LINEAR);
  return t;
}

// A cached single-run text texture, always rendered white and tinted at draw time
// so theme colour changes and blinking are free (no re-rasterisation). With a `shadowRadius` it also keeps a soft
// shadow to lay under the text over a busy background.
struct Label {
  explicit Label(int shadowRadius = 0) : shadowRadius(shadowRadius) {}

  int shadowRadius;
  TexturePtr tex, shadow;
  float w = 0, h = 0;
  int ascent = 0;
  std::string cache;
  int wrapCache = -1;

  void set(SDL_Renderer *r, TTF_Font *f, const std::string &s, int wrap = 0) {
    if (s == cache && tex && wrap == wrapCache) return;
    cache = s;
    wrapCache = wrap;
    ascent = TTF_GetFontAscent(f);
    shadow.reset();
    if (s.empty()) {
      tex.reset();
      w = h = 0;
      return;
    }
    SDL_Color white{255, 255, 255, 255};
    SurfacePtr surf(wrap > 0 ? TTF_RenderText_Blended_Wrapped(f, s.c_str(), 0, white, wrap)
                             : TTF_RenderText_Blended(f, s.c_str(), 0, white));
    if (surf) {
      tex.reset(SDL_CreateTextureFromSurface(r, surf.get()));
      w = (float)surf->w;
      h = (float)surf->h;
      if (shadowRadius > 0) shadow = makeShadow(r, surf.get(), shadowRadius);
    }
  }

  void drawTop(SDL_Renderer *r, float x, float y, Col c, float alpha = 1.0f) const {
    if (!tex) return;
    if (shadow && alpha * c.a > 0.0f) {
      const float pad = shadowRadius * 2.0f, drop = shadowRadius * 0.25f;
      SDL_SetTextureColorMod(shadow.get(), (Uint8)shadowCol.r, (Uint8)shadowCol.g, (Uint8)shadowCol.b);
      SDL_SetTextureAlphaMod(shadow.get(), (Uint8)std::clamp(shadowAlpha * alpha * 255.0f, 0.0f, 255.0f));
      SDL_FRect dst{x - pad, y - pad + drop, w + 2 * pad, h + 2 * pad};
      SDL_RenderTexture(r, shadow.get(), nullptr, &dst);
    }
    SDL_SetTextureColorMod(tex.get(), (Uint8)c.r, (Uint8)c.g, (Uint8)c.b);
    SDL_SetTextureAlphaMod(tex.get(), (Uint8)std::clamp(c.a * alpha, 0.0f, 255.0f));
    SDL_FRect dst{x, y, w, h};
    SDL_RenderTexture(r, tex.get(), nullptr, &dst);
  }
  // place so the text baseline sits at baselineY
  void drawBase(SDL_Renderer *r, float x, float baselineY, Col c, float alpha = 1.0f) const {
    drawTop(r, x, baselineY - ascent, c, alpha);
  }

  // Set before drawing: the shadow's colour (dark under light ink, light under dark ink) and strength.
  Col shadowCol{0, 2, 12};
  float shadowAlpha = 0.0f;
};

// Per-glyph rendered run so we can apply letter tracking (SDL_ttf has none), with an optional soft shadow per glyph.
struct TrackedLabel {
  explicit TrackedLabel(int shadowRadius = 0) : shadowRadius(shadowRadius) {}

  int shadowRadius;
  std::string cache;
  float totalW = 0;
  int ascent = 0;
  struct G {
    TexturePtr t, shadow;
    float x = 0, w = 0, h = 0;
  };
  std::vector<G> gs;
  Col shadowCol{0, 2, 12};
  float shadowAlpha = 0.0f;

  void set(SDL_Renderer *r, TTF_Font *f, const std::string &s, float tracking) {
    if (s == cache && !gs.empty()) return;
    cache = s;
    gs.clear();
    ascent = TTF_GetFontAscent(f);
    float x = 0;
    for (std::size_t i = 0; i < s.size();) {
      std::size_t n = utf8Len((unsigned char)s[i]);
      std::string ch = s.substr(i, n);
      i += n;
      G g;
      SDL_Color white{255, 255, 255, 255};
      SurfacePtr surf(TTF_RenderText_Blended(f, ch.c_str(), 0, white));
      if (surf && surf->w > 0) {
        g.t.reset(SDL_CreateTextureFromSurface(r, surf.get()));
        g.w = (float)surf->w;
        g.h = (float)surf->h;
        if (shadowRadius > 0) g.shadow = makeShadow(r, surf.get(), shadowRadius);
      } else {
        int mw = 0, mh = 0;
        TTF_GetStringSize(f, ch.c_str(), 0, &mw, &mh); // spaces have no pixels
        g.w = (float)mw;
      }
      g.x = x;
      x += g.w + tracking;
      gs.push_back(std::move(g));
    }
    totalW = x > 0 ? x - tracking : 0;
  }

  void draw(SDL_Renderer *r, float ox, float baselineY, Col c) const {
    const float pad = shadowRadius * 2.0f, drop = shadowRadius * 0.25f;
    for (const auto &g : gs) {
      if (!g.shadow || shadowAlpha <= 0.0f) continue;
      SDL_SetTextureColorMod(g.shadow.get(), (Uint8)shadowCol.r, (Uint8)shadowCol.g, (Uint8)shadowCol.b);
      SDL_SetTextureAlphaMod(g.shadow.get(), (Uint8)std::clamp(shadowAlpha * 255.0f, 0.0f, 255.0f));
      SDL_FRect dst{ox + g.x - pad, baselineY - ascent - pad + drop, g.w + 2 * pad, g.h + 2 * pad};
      SDL_RenderTexture(r, g.shadow.get(), nullptr, &dst);
    }
    for (const auto &g : gs) {
      if (!g.t) continue;
      SDL_SetTextureColorMod(g.t.get(), (Uint8)c.r, (Uint8)c.g, (Uint8)c.b);
      SDL_SetTextureAlphaMod(g.t.get(), (Uint8)c.a);
      SDL_FRect dst{ox + g.x, baselineY - ascent, g.w, g.h};
      SDL_RenderTexture(r, g.t.get(), nullptr, &dst);
    }
  }
};

#ifdef APP_DEBUG
// Walks a whole day, every few minutes, in every kind of weather, and checks that the text theme keeps every piece
// of text readable over the scene (with its backing). Logs how much backing the palettes needed at most, which is
// worth keeping low: the less backing, the more of the scene shows.
void verifyReadability(std::time_t day) {
  int failures = 0;
  for (int code : {0, 2, 3, 45, 53, 63, 65, 75, 95}) {
    float haloMax = 0, scrimMax = 0;
    bool light = true;
    for (std::time_t t = day; t < day + 86400; t += 120) {
      const std::tm tm = localTime(t);
      SceneState s;
      applySky(s, Astro::skyAt(t, Config::latitude, Config::longitude), tm.tm_hour + tm.tm_min / 60.0f);
      applyWeather(s, {true, code, 5, 270, 0});
      s.snowCover *= s.snowIntensity;
      deriveScene(s);
      const Look l = lookFor(s);
      const TextTheme th = textThemeFor(l, s, light);
      light = th.lightInk;
      haloMax = std::max({haloMax, th.haloTime, th.haloTop});
      scrimMax = std::max({scrimMax, th.scrimStrip, th.scrimHorizon});
      const Backdrops b = backdropsFor(l, s);
      std::string problems;
      auto need = [&](const char *what, Col ink, Col bg, float min) {
        if (contrast(ink, bg) < min - 0.05f) problems += std::format(" {} {:.1f}<{:.1f}", what, contrast(ink, bg), min);
      };
      for (const Col &bg : b.time) {
        need("time", th.ink, mix(bg, th.halo, th.haloTime), Readability::time);
        need("colon", th.accent, mix(bg, th.halo, th.haloTime), Readability::accent);
      }
      for (const Col &bg : b.top)
        need("date", th.inkDim, mix(bg, th.halo, th.haloTop), Readability::text);
      for (const Col &bg : b.strip)
        need("strip", th.landInk, mix(bg, th.scrim, th.scrimStrip), Readability::landInk);
      for (const Col &bg : b.horizon)
        need("horizon", th.landMute, mix(bg, th.scrim, th.scrimHorizon), Readability::text);
      if (!problems.empty() && failures++ < 20)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Low contrast, weather %d at %02d:%02d:%s", code, tm.tm_hour,
                    tm.tm_min, problems.c_str());
    }
    SDL_Log("Weather %2d: most backing behind the sky text %.2f, behind the land text %.2f", code, haloMax, scrimMax);
  }
  if (failures == 0) SDL_Log("Text readable in every kind of weather at every minute of the day");
}
#endif

// ------------------------------------------------------------- the app ----

class Clock {
public:
  Clock() = default;
  Clock(const Clock &) = delete;
  Clock &operator=(const Clock &) = delete;

  // The worker threads write to members of this object: stop and join them before anything else is destroyed.
  ~Clock() {
    for (std::jthread *t : {&weatherLoaderThread, &calendarLoaderThread})
      t->request_stop();
    for (std::jthread *t : {&weatherLoaderThread, &calendarLoaderThread})
      if (t->joinable()) t->join();
  }

  bool Init() {
    SDL_SetAppMetadata(Config::AppName, Config::AppVersion, nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s", SDL_GetError());
      return false;
    }
    SDL_Window *w;
    SDL_Renderer *r;
    if (!SDL_CreateWindowAndRenderer(Config::AppName, Config::screen_width, Config::screen_height, SDL_WINDOW_RESIZABLE,
                                     &w, &r)) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create window/renderer: %s", SDL_GetError());
      return false;
    }
    window.reset(w);
    renderer.reset(r);
    SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_BLEND);
    if (!SDL_SetRenderVSync(renderer.get(), 1)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't enable vsync: %s", SDL_GetError());
    }
    // The scene is always gently moving (clouds, rain, leaves); 30 frames a second is smooth for all of it.
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "30");

    if (!TTF_Init()) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL_ttf: %s", SDL_GetError());
      return false;
    }
    auto open = [](const unsigned char *data, unsigned int len, float size) {
      return FontPtr(TTF_OpenFontIO(SDL_IOFromConstMem(data, len), true, size));
    };
    fTime = open(SpaceGrotesk_Medium_ttf, SpaceGrotesk_Medium_ttf_len, Layout::timeSize);
    fTempNum = open(SpaceGrotesk_Medium_ttf, SpaceGrotesk_Medium_ttf_len, 40.0f);
    fWindNum = open(SpaceGrotesk_Medium_ttf, SpaceGrotesk_Medium_ttf_len, 30.0f);
    fUnitLg = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 20.0f);
    fUnitSm = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 16.0f);
    fDate = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 20.0f);
    fCondition = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 13.0f);
    fAxis = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 12.0f);
    fAdvice = open(Inter_Regular_ttf, Inter_Regular_ttf_len, 23.0f);
    fMarkTitle = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 15.0f);
    fMarkWhen = open(Inter_Regular_ttf, Inter_Regular_ttf_len, 15.0f);
    if (!fTime || !fTempNum || !fWindNum || !fUnitLg || !fUnitSm || !fDate || !fCondition || !fAxis || !fAdvice ||
        !fMarkTitle || !fMarkWhen) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't load embedded fonts: %s", SDL_GetError());
      return false;
    }

    TTF_SetFontWrapAlignment(fAdvice.get(), TTF_HORIZONTAL_ALIGN_CENTER); // the advice is centred under the clock

    // Prefer proper typographic glyphs where the font carries them.
    windUnit = TTF_FontHasGlyph(fUnitSm.get(), 0x2044) ? "m\xE2\x81\x84s" : "m/s"; // m⁄s

    LoadIcons();
    BakeHorizonIcons();
    if (!scene.Init(renderer.get())) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create scene textures: %s", SDL_GetError());
      return false;
    }
#ifdef APP_DEBUG
    {
      std::tm midnight = localNow();
      midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0, midnight.tm_isdst = -1;
      verifyReadability(std::mktime(&midnight));
    }
#endif

    if (!SDL_SetRenderLogicalPresentation(renderer.get(), Config::screen_width, Config::screen_height,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't set logical presentation: %s", SDL_GetError());
    }
#ifndef APP_DEBUG
    SDL_HideCursor();
#endif

    // Debug: APP_SHOT=path.png saves one frame (after APP_SHOT_FRAME frames) and exits.
    shotPath = SDL_getenv("APP_SHOT");
    if (const char *sf = SDL_getenv("APP_SHOT_FRAME")) shotFrame = SDL_atoi(sf);

    // SDL fills its CPU-feature cache lazily and without a lock; do it here, before the worker thread exists.
    SDL_GetSIMDAlignment();
    weatherLoaderThread = std::jthread(&Clock::FetchWeather, this);
    const char *urls = SDL_getenv("CALENDAR_URL");
    calendarUrls = urls && *urls ? urls : Config::CalendarUrl;
    if (!calendarUrls.empty()) calendarLoaderThread = std::jthread(&Clock::FetchCalendar, this);
    return true;
  }

  SDL_AppResult Iterate() {
    Render();
    if (shotPath && ++frameCount >= shotFrame) return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
  }

private:
  WindowPtr window;
  RendererPtr renderer;

  FontPtr fTime, fTempNum, fWindNum, fUnitLg, fUnitSm, fDate, fCondition, fAxis, fAdvice, fMarkTitle, fMarkWhen;
  std::array<TexturePtr, (std::size_t)Icon::COUNT> icons;
  TexturePtr icCalendar, icCall, icSun, icDot, icGlow;
  std::string windUnit = "m/s";
  const std::string deg = "\xC2\xB0";    // °
  const std::string mdot = " \xC2\xB7 "; // · with spaces

  std::mutex weatherMutex;
  WeatherState weather;
  WeatherInput lastInput; // kept across failed fetches, so a network blip does not clear the sky

  std::string calendarUrls;
  std::mutex calendarMutex;
  std::vector<Calendar::Event> calendarEvents; // the next few days

  float horizonShown = 0; // the timeline fades in and out as it becomes relevant

  Scene scene;
  SceneState sceneNow; // eased towards the forecast
  bool haveScene = false;
  double lastFrameSecs = 0;

  // The text colours follow the scene; when the sky text flips between light and dark ink (sunrise, sunset, a storm
  // rolling in) it fades over a moment instead of snapping.
  TextTheme theme;
  bool haveTheme = false;
  TextTheme flipFrom, shownTheme;
  Uint64 flipStartMs = 0;
  bool flipping = false;
  static constexpr float kFlipMs = 2000.0f;

  const char *shotPath = nullptr;
  int shotFrame = 180;
  int frameCount = 0;

  // Cached label runs
  TrackedLabel lDate{3}, lCondition{3};
  Label lHH{10}, lColon{10}, lMM{10};
  Label lTempNum{4}, lTempUnit{3}, lWindNum{4}, lWindUnit{3};
  Label lAdvice{4};
  std::array<Label, 5> lAxis{Label{2}, Label{2}, Label{2}, Label{2}, Label{2}};
  static constexpr int kMarkLabels = 8, kBandLabels = 3;
  std::array<Label, kMarkLabels> lMarkTitle{Label{3}, Label{3}, Label{3}, Label{3},
                                            Label{3}, Label{3}, Label{3}, Label{3}};
  std::array<Label, kMarkLabels> lMarkWhen{Label{3}, Label{3}, Label{3}, Label{3},
                                           Label{3}, Label{3}, Label{3}, Label{3}};
  std::array<Label, kBandLabels> lBand{Label{3}, Label{3}, Label{3}};

  // Declared last so they are destroyed first; ~Clock also joins them explicitly.
  std::jthread weatherLoaderThread, calendarLoaderThread;

  // -------------------------------------------------------------- assets --
  void LoadIcons() {
    struct E {
      Icon id;
      const unsigned char *data;
      unsigned int len;
    };
    const std::array<E, (std::size_t)Icon::COUNT> table{{
        {Icon::ClearDay, icon_clear_day_png, icon_clear_day_png_len},
        {Icon::ClearNight, icon_clear_night_png, icon_clear_night_png_len},
        {Icon::PartlyDay, icon_partly_day_png, icon_partly_day_png_len},
        {Icon::PartlyNight, icon_partly_night_png, icon_partly_night_png_len},
        {Icon::Cloudy, icon_cloudy_png, icon_cloudy_png_len},
        {Icon::Fog, icon_fog_png, icon_fog_png_len},
        {Icon::Rain, icon_rain_png, icon_rain_png_len},
        {Icon::Snow, icon_snow_png, icon_snow_png_len},
        {Icon::Thunder, icon_thunder_png, icon_thunder_png_len},
        {Icon::Wind, icon_wind_png, icon_wind_png_len},
    }};
    for (const auto &e : table) {
      SurfacePtr s(IMG_Load_IO(SDL_IOFromConstMem(e.data, e.len), true));
      if (!s) continue;
      TexturePtr t(SDL_CreateTextureFromSurface(renderer.get(), s.get()));
      if (t) SDL_SetTextureScaleMode(t.get(), SDL_SCALEMODE_LINEAR);
      icons[(std::size_t)e.id] = std::move(t);
    }
  }

  void drawIcon(Icon ic, float x, float y, float size, Col c, float alpha = 1.0f) {
    SDL_Texture *t = icons[(std::size_t)ic].get();
    if (!t) return;
    SDL_SetTextureColorMod(t, (Uint8)c.r, (Uint8)c.g, (Uint8)c.b);
    SDL_SetTextureAlphaMod(t, (Uint8)std::clamp(c.a * alpha, 0.0f, 255.0f));
    SDL_FRect dst{x, y, size, size};
    SDL_RenderTexture(renderer.get(), t, nullptr, &dst);
  }

  // The event horizon's icons, on a 40×40 canvas: a calendar page, a telephone handset, the sun on the horizon, and
  // a marker dot with its glow.
  void BakeHorizonIcons() {
    SDL_Renderer *r = renderer.get();
    constexpr int n = 40;
    constexpr float stroke = 1.7f;
    icCalendar = bakeIcon(r, n, [](float x, float y) {
      const float page = std::abs(Sdf::roundBox(x, y, 20, 23, 14, 13, 4)) - stroke;
      const float band = std::max(Sdf::roundBox(x, y, 20, 23, 14, 13, 4), y - 16.0f);
      const float rings = std::min(Sdf::segment(x, y, 13, 6, 13, 12), Sdf::segment(x, y, 27, 6, 27, 12)) - stroke;
      return std::min({page, band, rings});
    });
    icCall = bakeIcon(r, n, [](float x, float y) {
      constexpr float cx = 27, cy = 13, rad = 15;
      const float body = Sdf::arc(x, y, cx, cy, rad, 88, 182) - 2.6f;
      auto pad = [&](float deg) {
        const float t = deg * (float)M_PI / 180.0f, ex = cx + rad * std::cos(t), ey = cy + rad * std::sin(t);
        return Sdf::segment(x, y, ex, ey, ex + (cx - ex) * 0.38f, ey + (cy - ey) * 0.38f) - 3.6f;
      };
      return std::min({body, pad(90), pad(180)});
    });
    icSun = bakeIcon(r, n, [](float x, float y) {
      const float line = Sdf::segment(x, y, 4, 29, 36, 29) - stroke;
      const float disc = std::max(std::hypot(x - 20, y - 29) - 8.0f, y - 26.5f);
      float rays = 99.0f;
      for (float deg : {180.0f, 225.0f, 270.0f, 315.0f, 360.0f}) {
        const float c = std::cos(deg * (float)M_PI / 180.0f), sn = std::sin(deg * (float)M_PI / 180.0f);
        rays = std::min(rays, Sdf::segment(x, y, 20 + 12 * c, 29 + 12 * sn, 20 + 16 * c, 29 + 16 * sn) - stroke);
      }
      return std::min({line, disc, rays});
    });
    icDot = bakeIcon(r, n, [](float x, float y) { return (std::hypot(x - 20, y - 20) - 19.0f) * 0.5f; });
    icGlow = bakeIcon(r, n, [](float x, float y) {
      return 0.5f - std::pow(smooth01(1.0f - std::hypot(x - 20, y - 20) / 20.0f), 1.5f); // alpha falls off
    });
  }

  void drawTex(SDL_Texture *t, float cx, float cy, float size, Col c, float alpha) {
    if (!t) return;
    SDL_SetTextureColorMod(t, (Uint8)std::clamp(c.r, 0.0f, 255.0f), (Uint8)std::clamp(c.g, 0.0f, 255.0f),
                           (Uint8)std::clamp(c.b, 0.0f, 255.0f));
    SDL_SetTextureAlphaMod(t, (Uint8)std::clamp(c.a * alpha, 0.0f, 255.0f));
    SDL_FRect dst{cx - size / 2.0f, cy - size / 2.0f, size, size};
    SDL_RenderTexture(renderer.get(), t, nullptr, &dst);
  }

  void fillRect(float x, float y, float w, float h, Col c) {
    SDL_SetRenderDrawColor(renderer.get(), (Uint8)c.r, (Uint8)c.g, (Uint8)c.b, (Uint8)c.a);
    SDL_FRect r{x, y, w, h};
    SDL_RenderFillRect(renderer.get(), &r);
  }

  // --------------------------------------------------------------- data --
  // SDL keeps per-thread error strings for threads it did not create; the thread has to free them itself.
  struct SdlThreadCleanup {
    ~SdlThreadCleanup() { SDL_CleanupTLS(); }
  };

  void FetchWeather(std::stop_token stopToken) {
    SdlThreadCleanup cleanup;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    const auto url = cpr::Url{"https://api.open-meteo.com/v1/forecast"};
    const auto params = cpr::Parameters{{"latitude", std::format("{:.4f}", Config::latitude)},
                                        {"longitude", std::format("{:.4f}", Config::longitude)},
                                        {"current_weather", "true"},
                                        {"minutely_15", "precipitation,snowfall"},
                                        {"windspeed_unit", "ms"},
                                        {"forecast_days", "2"},
                                        {"timezone", "auto"}};

    while (!stopToken.stop_requested()) {
      bool ok = false;
      double temp = 0, wind = 0, windDir = 270;
      int code = 0;
      Horizon::Precip precip;
      try {
        cpr::Response resp = cpr::Get(url, params, kConnectTimeout, kTimeout, abortOnStop(stopToken));
        if (resp.status_code == 200) {
          auto j = json::parse(resp.text);
          const auto &cw = j.at("current_weather");
          temp = cw.at("temperature").get<double>();
          wind = cw.at("windspeed").get<double>();
          code = cw.at("weathercode").get<int>();
          if (cw.contains("winddirection") && cw.at("winddirection").is_number())
            windDir = cw.at("winddirection").get<double>();

          // Each value is the sum over the 15 minutes before its time; keep the steps from now to the horizon.
          if (j.contains("minutely_15")) {
            const auto &m = j.at("minutely_15");
            const auto &times = m.at("time");
            const auto &mm = m.at("precipitation");
            const bool haveSnow = m.contains("snowfall");
            std::time_t now = std::time(nullptr);
            std::size_t start = 0;
            while (start < times.size() && parseTs(times[start].get<std::string>()) <= now)
              start++;
            constexpr std::size_t steps = Horizon::span / Horizon::step + 1;
            if (start < times.size()) precip.from = parseTs(times[start].get<std::string>()) - Horizon::step;
            for (std::size_t i = start; i < start + steps && i < mm.size(); ++i) {
              const float fall = mm[i].is_null() ? 0.0f : (float)mm[i].get<double>();
              const float snow =
                  haveSnow && !m.at("snowfall")[i].is_null() ? (float)m.at("snowfall")[i].get<double>() : 0.0f;
              precip.mm.push_back(fall);
              precip.snow.push_back(snow * 10.0f / 7.0f > fall * 0.5f); // 7 cm of snow is ~10 mm of water
            }
          }
          ok = true;
        }
      } catch (const std::exception &e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Weather fetch failed: %s", e.what());
      }

      std::string advice;
      if (ok) advice = FetchAdvice(temp, code, stopToken);

      {
        std::scoped_lock lock(weatherMutex);
        if (ok) {
          weather.valid = true;
          weather.temperature = temp;
          weather.windspeed = wind;
          weather.winddirection = windDir;
          weather.weathercode = code;
          weather.precip = std::move(precip);
          weather.advice = std::move(advice);
        } else {
          weather = WeatherState{};
        }
      }

      // Every 5 minutes; sooner after a failure (e.g. Wi-Fi not up yet right after boot).
      sleepFor(stopToken, ok ? std::chrono::minutes(5) : std::chrono::minutes(1));
    }
  }

  // The calendar feeds, every ten minutes. A feed that fails keeps its last good events, so a network blip does not
  // empty the horizon. The feed addresses are secret: they are never logged.
  void FetchCalendar(std::stop_token stopToken) {
    SdlThreadCleanup cleanup;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    std::vector<std::string> urls;
    for (std::string_view u : Calendar::detail::split(calendarUrls, ' ')) {
      if (u.empty()) continue;
      std::string url(u);
      if (url.starts_with("webcal://")) url = "https://" + url.substr(9);
      urls.push_back(std::move(url));
    }
    std::vector<std::vector<Calendar::Event>> feeds(urls.size());

    while (!stopToken.stop_requested()) {
      bool ok = true;
      const std::time_t t = std::time(nullptr);
      for (std::size_t i = 0; i < urls.size(); ++i) {
        try {
          cpr::Response resp = cpr::Get(cpr::Url{urls[i]}, kConnectTimeout, kTimeout, abortOnStop(stopToken));
          if (resp.status_code == 200) {
            feeds[i] = Calendar::parse(resp.text, t - 86400, t + 3 * 86400);
          } else {
            ok = false;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Calendar %zu fetch failed: %ld", i + 1, resp.status_code);
          }
        } catch (const std::exception &e) {
          ok = false;
          SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Calendar %zu fetch failed: %s", i + 1, e.what());
        }
      }
      std::vector<Calendar::Event> all;
      for (const auto &f : feeds)
        all.insert(all.end(), f.begin(), f.end());
      std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
      {
        std::scoped_lock lock(calendarMutex);
        calendarEvents = std::move(all);
      }
      sleepFor(stopToken, ok ? std::chrono::minutes(10) : std::chrono::minutes(1));
    }
  }

  // One short sentence, or the offline advice when the model returns nothing usable.
  static std::string sanitizeAdvice(std::string out, double temp) {
    std::replace(out.begin(), out.end(), '\n', ' ');
    const auto first = out.find_first_not_of(" \t\r\"");
    const auto last = out.find_last_not_of(" \t\r\"");
    out = first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
    if (out.empty() || out.size() > 200) return basicAdvice(temp);
    return out;
  }

  std::string FetchAdvice(double temp, int code, const std::stop_token &stopToken) {
    std::string apiKey = Config::GroqApiKey;
    if (apiKey.empty() || apiKey == "MISSING_KEY") return basicAdvice(temp);
    try {
      std::tm tm = localNow();
      std::string prompt =
          std::format("I live in Amsterdam. Today is {}, the time is {:02}:{:02} and the weather is {} ({:.0f}C). "
                      "What should I wear? Answer as one short sentence, continuing the phrase \"You should wear\" "
                      "but WITHOUT the words \"you should wear\" — just the clothing. Do not mention the city, "
                      "time, date or the weather itself.",
                      dateString(tm), tm.tm_hour, tm.tm_min, conditionText(code), temp);
      json payload = {{"model", "openai/gpt-oss-120b"},
                      {"max_tokens", 120},
                      {"temperature", 0.7},
                      {"messages",
                       {{{"role", "system"}, {"content", "You give concise, practical clothing advice."}},
                        {{"role", "user"}, {"content", prompt}}}}};
      cpr::Response r = cpr::Post(
          cpr::Url{"https://api.groq.com/openai/v1/chat/completions"}, cpr::Body{payload.dump()},
          cpr::Header{{"Authorization", std::string("Bearer ") + apiKey}, {"Content-Type", "application/json"}},
          kConnectTimeout, kTimeout, abortOnStop(stopToken));
      if (r.status_code == 200) {
        auto j = json::parse(r.text);
        return sanitizeAdvice(j.at("choices").at(0).at("message").at("content").get<std::string>(), temp);
      } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LLM fetch failed %ld", r.status_code);
      }
    } catch (const std::exception &e) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LLM exception: %s", e.what());
    }
    return basicAdvice(temp);
  }

  // ------------------------------------------------------------- render --
  static std::string toUpper(std::string s) {
    for (char &c : s)
      c = (char)std::toupper((unsigned char)c);
    return s;
  }

  void Render() {
    const std::time_t t = now();
    const std::tm tm = localTime(t);
    const double secs = (double)SDL_GetTicksNS() / 1e9;

    WeatherState w;
    {
      std::scoped_lock lock(weatherMutex);
      w = weather;
    }
#ifdef APP_DEBUG
    if (const int fake = fakeWeatherCode(); fake >= 0) w = fakeWeather(fake);
#endif
    if (w.valid) {
      lastInput = {true, w.weathercode, (float)w.windspeed, (float)w.winddirection,
                   w.precip.mm.empty() ? 0.0f : w.precip.mm.front()};
    }

    // The world right now: the sky over this place at this moment, and the forecast rolling in over a few seconds.
    const Astro::Sky sky = Astro::skyAt(t, Config::latitude, Config::longitude);
    SceneState target;
    applySky(target, sky, (float)tm.tm_hour + tm.tm_min / 60.0f + tm.tm_sec / 3600.0f);
    applyWeather(target, lastInput);
    const float dt = haveScene ? (float)std::clamp(secs - lastFrameSecs, 0.0, 0.5) : 0.0f;
    if (!haveScene) {
      sceneNow = target;
      sceneNow.snowCover = target.snowCover * target.snowIntensity;
      haveScene = true;
    } else {
      easeScene(sceneNow, target, dt);
    }
    lastFrameSecs = secs;
    deriveScene(sceneNow);
    const Look look = lookFor(sceneNow);
    const TextTheme th = CurrentTheme(textThemeFor(look, sceneNow, haveTheme ? theme.lightInk : true));

    scene.Draw(renderer.get(), sceneNow, look, secs);
    DrawTop(tm, w, th);
    DrawTime(tm, th);
    DrawWeatherStrip(w, sky.sun.elevation > Horizon::sunriseElevation, look, th);
    DrawHorizon(t, w, th, dt);

    if (shotPath && frameCount + 1 >= shotFrame) {
      if (SDL_Surface *s = SDL_RenderReadPixels(renderer.get(), nullptr)) {
        IMG_SavePNG(s, shotPath);
        SDL_DestroySurface(s);
      }
    }
    SDL_RenderPresent(renderer.get());
  }

  static TextTheme mixTheme(const TextTheme &a, const TextTheme &b, float k) {
    TextTheme t = b;
    t.ink = mix(a.ink, b.ink, k);
    t.inkDim = mix(a.inkDim, b.inkDim, k);
    t.accent = mix(a.accent, b.accent, k);
    // The backing fades out under the old ink and in under the new, rather than passing through grey.
    t.halo = k < 0.5f ? a.halo : b.halo;
    t.haloTime = k < 0.5f ? a.haloTime * (1.0f - 2.0f * k) : b.haloTime * (2.0f * k - 1.0f);
    t.haloTop = k < 0.5f ? a.haloTop * (1.0f - 2.0f * k) : b.haloTop * (2.0f * k - 1.0f);
    return t;
  }

  TextTheme CurrentTheme(const TextTheme &target) {
    const Uint64 ms = SDL_GetTicks();
    if (haveTheme && target.lightInk != theme.lightInk) {
      flipFrom = shownTheme;
      flipStartMs = ms;
      flipping = true;
    }
    theme = target;
    haveTheme = true;
    shownTheme = target;
    if (flipping) {
      const float k = std::clamp((float)(ms - flipStartMs) / kFlipMs, 0.0f, 1.0f);
      if (k >= 1.0f)
        flipping = false;
      else
        shownTheme = mixTheme(flipFrom, target, smooth01(k));
    }
    return shownTheme;
  }

  // The backing for a run of text that needs `need` (see backingFor): light ink always casts a soft shadow, dark ink
  // only glows when it has to.
  static Backing skyBacking(const TextTheme &th, float need) {
    return backingFor(need, 0.45f * smooth01((relativeLuminance(th.ink) - 0.25f) / 0.5f));
  }

  // Date on the left and the current conditions on the right, in small tracked capitals.
  void DrawTop(const std::tm &tm, const WeatherState &w, const TextTheme &th) {
    SDL_Renderer *r = renderer.get();
    lDate.set(r, fDate.get(), toUpper(dateString(tm)), 3.0f);
    lCondition.set(r, fCondition.get(), w.valid ? toUpper(conditionText(w.weathercode)) : "", 2.4f);
    const float x = Layout::padX, B = Layout::dateBaseline;
    const float condX = Config::screen_width - Layout::padX - lCondition.totalW, condB = B - 2.0f;
    const Backing bk = skyBacking(th, th.haloTop);
    drawSoftRect(r, x, B - lDate.ascent * 0.75f, x + lDate.totalW, B + 3.0f, 24.0f, th.halo, bk.panel);
    if (lCondition.totalW > 0)
      drawSoftRect(r, condX, condB - lCondition.ascent * 0.75f, condX + lCondition.totalW, condB + 3.0f, 20.0f, th.halo,
                   bk.panel);
    for (TrackedLabel *l : {&lDate, &lCondition}) {
      l->shadowCol = th.halo;
      l->shadowAlpha = bk.glyph;
    }
    lDate.draw(r, x, B, th.inkDim);
    lCondition.draw(r, condX, condB, th.inkDim);
  }

  void DrawTime(const std::tm &tm, const TextTheme &th) {
    SDL_Renderer *r = renderer.get();
    lHH.set(r, fTime.get(), std::format("{:02}", tm.tm_hour));
    lMM.set(r, fTime.get(), std::format("{:02}", tm.tm_min));
    lColon.set(r, fTime.get(), ":");

    const float gap = Layout::timeSize * 0.02f;
    const float total = lHH.w + gap + lColon.w + gap + lMM.w;
    float x = (Config::screen_width - total) / 2.0f;
    const float base = Layout::timeBaseline;
    const Backing bk = skyBacking(th, th.haloTime);
    drawSoftRect(r, x + 10.0f, Layout::timeTop - 4.0f, x + total - 10.0f, base + 4.0f, 80.0f, th.halo, bk.panel);

    // gentle colon pulse
    const float ph = (float)(SDL_GetTicks() % 2000) / 2000.0f;
    const float pulse = 0.6f + 0.4f * (0.5f + 0.5f * std::cos(ph * 2.0f * (float)M_PI));

    for (Label *l : {&lHH, &lColon, &lMM}) {
      l->shadowCol = th.halo;
      l->shadowAlpha = bk.glyph;
    }
    lHH.drawBase(r, x, base, th.ink);
    x += lHH.w + gap;
    lColon.drawBase(r, x, base - Layout::timeSize * 0.02f, th.accent, pulse);
    x += lColon.w + gap;
    lMM.drawBase(r, x, base, th.ink);
  }

  // Centred under the clock: [condition] NN °C   [wind] N m/s, and the clothing advice below.
  void DrawWeatherStrip(const WeatherState &w, bool isDay, const Look &look, const TextTheme &th) {
    SDL_Renderer *r = renderer.get();
    const float B = Layout::stripBaseline;
    lTempNum.set(r, fTempNum.get(), w.valid ? std::format("{:.0f}", w.temperature) : "--");
    lTempUnit.set(r, fUnitLg.get(), deg + "C");
    lWindNum.set(r, fWindNum.get(), w.valid ? std::format("{:.0f}", w.windspeed) : "--");
    lWindUnit.set(r, fUnitSm.get(), windUnit);
    lAdvice.set(r, fAdvice.get(), w.valid ? w.advice : "", 700);

    constexpr float tIcon = 36.0f, wIcon = 26.0f;
    const float row =
        tIcon + 12.0f + lTempNum.w + 4.0f + lTempUnit.w + 44.0f + wIcon + 10.0f + lWindNum.w + 5.0f + lWindUnit.w;
    float x = (Config::screen_width - row) / 2.0f;

    // Backing, where the land is not dark enough on its own (fog, snow); a third line of advice reaches the water.
    const float left = std::min(x, (Config::screen_width - lAdvice.w) / 2.0f) - 6.0f;
    const float right = Config::screen_width - left;
    const float adviceTop = Layout::adviceBaseline - lAdvice.ascent;
    float scrim = th.scrimStrip;
    if (adviceTop + lAdvice.h > Layout::horizonY)
      scrim = std::max(scrim, backingNeeded(th.landInk, th.scrim, {waterColour(look)}, Readability::landInk, 0.85f));
    const Backing bk = backingFor(scrim, 0.5f);
    drawSoftRect(r, left, B - 34.0f, right, std::max(B + 8.0f, adviceTop + lAdvice.h), 48.0f, th.scrim, bk.panel);

    for (Label *l : {&lTempNum, &lTempUnit, &lWindNum, &lWindUnit, &lAdvice})
      l->shadowAlpha = bk.glyph;
    const float numCenter = B - lTempNum.ascent * 0.36f; // rough optical centre of the figures
    drawIcon(w.valid ? iconFor(w.weathercode, isDay) : Icon::Cloudy, x, numCenter - tIcon / 2.0f, tIcon,
             mix(th.landAccent, th.landInk, 0.35f));
    x += tIcon + 12.0f;
    lTempNum.drawBase(r, x, B, th.landInk);
    x += lTempNum.w + 4.0f;
    lTempUnit.drawBase(r, x, B, th.landDim);
    x += lTempUnit.w + 44.0f;
    const float wNumCenter = B - lWindNum.ascent * 0.36f;
    drawIcon(Icon::Wind, x, wNumCenter - wIcon / 2.0f, wIcon, th.landDim);
    x += wIcon + 10.0f;
    lWindNum.drawBase(r, x, B, th.landInk);
    x += lWindNum.w + 5.0f;
    lWindUnit.drawBase(r, x, B, th.landDim);

    lAdvice.drawTop(r, (Config::screen_width - lAdvice.w) / 2.0f, adviceTop, th.landInk);
  }

  // The event horizon (horizon.h): the next four hours on one time scale, with rain and snow as bands on the line,
  // and the sun and the calendar as marks with a label above. Labels that would collide give way, soonest events
  // first; their marks stay. Marks grow and brighten as their time comes closer.
  void DrawHorizon(std::time_t t, const WeatherState &w, const TextTheme &th, float dt) {
    std::vector<Calendar::Event> events;
    {
      std::scoped_lock lock(calendarMutex);
      events = calendarEvents;
    }
#ifdef APP_DEBUG
    if (SDL_getenv("APP_FAKE_EVENTS")) events = fakeEvents();
#endif
    const Horizon::Model m =
        Horizon::modelFor(t, w.valid ? w.precip : Horizon::Precip{}, events, Config::latitude, Config::longitude);
    const float target = m.relevant ? 1.0f : 0.0f;
    horizonShown =
        dt <= 0.0f ? target : horizonShown + std::clamp(target - horizonShown, -dt / 1.5f, dt / 1.5f); // 1.5 s
    const float A = smooth01(horizonShown);
    if (A <= 0.004f) return;

    SDL_Renderer *r = renderer.get();
    const float left = Layout::padX, right = Config::screen_width - Layout::padX, CW = right - left;
    const float lineY = Layout::horizonLineY;
    auto xAt = [&](std::time_t at) {
      return left + std::clamp((float)(at - t) / (float)Horizon::span, 0.0f, 1.0f) * CW;
    };
    auto withAlpha = [](Col c, float a) { return Col{c.r, c.g, c.b, c.a * a}; };

    const Backing bk = backingFor(th.scrimHorizon, 0.5f);
    drawSoftRect(r, left, Layout::horizonTitleBaseline - 16.0f, right, Layout::horizonAxisBaseline + 4.0f, 36.0f,
                 th.scrim, bk.panel * A);

    // The time scale: a hairline with an hour tick, and NOW .. +4H below it.
    fillRect(left, lineY - 0.75f, CW, 1.5f, withAlpha(th.landInk, 0.4f * A));
    for (int h = 0; h <= 4; ++h) {
      Label &l = lAxis[h];
      l.set(r, fAxis.get(), h == 0 ? "NOW" : std::format("+{}H", h));
      l.shadowAlpha = bk.glyph;
      const float x = left + CW * h / 4.0f;
      fillRect(x - 0.75f, lineY - 3.5f, 1.5f, 7.0f, withAlpha(th.landInk, 0.5f * A));
      const float lx = h == 0 ? x : h == 4 ? x - l.w : x - l.w / 2.0f;
      l.drawBase(r, lx, Layout::horizonAxisBaseline, th.landMute, A);
    }

    // Rain and snow: a band on the line, as thick as the precipitation in each 15 minutes.
    const Col snowCol{214, 230, 255}, rainCol = mix(th.landAccent, Col{96, 160, 255}, 0.5f);
    for (std::size_t i = 0; i < w.precip.mm.size() && w.valid; ++i) {
      const std::time_t a = w.precip.from + (std::time_t)i * Horizon::step, b = a + Horizon::step;
      const float mm = w.precip.mm[i];
      if (mm < 0.05f || b <= t || a >= t + Horizon::span) continue;
      const float x0 = xAt(a), x1 = xAt(b), hh = 3.0f + std::clamp(mm / 2.0f, 0.0f, 1.0f) * 6.0f;
      const bool snow = i < w.precip.snow.size() && w.precip.snow[i];
      fillRect(x0, lineY - 1.0f - hh, x1 - x0, hh, withAlpha(snow ? snowCol : rainCol, 0.9f * A));
    }

    // Labels, in order of importance; one that would overlap a label already placed is left out.
    struct Box {
      float x0, x1, y0, y1;
    };
    std::vector<Box> placed;
    auto place = [&](float x0, float w, float y0, float y1) -> std::optional<float> {
      x0 = std::clamp(x0, left, right - w);
      for (const Box &b : placed)
        if (x0 < b.x1 + 16.0f && b.x0 < x0 + w + 16.0f && y0 < b.y1 && b.y0 < y1) return std::nullopt;
      placed.push_back({x0, x0 + w, y0, y1});
      return x0;
    };
    constexpr float icon = 20.0f, gap = 6.0f;
    const float whenB = Layout::horizonWhenBaseline;

    // Events first, then the rain and snow, then the sun.
    int slot = 0;
    for (const Horizon::Mark &mk : m.marks) {
      if (slot >= kMarkLabels) break;
      const bool sun = mk.kind == Horizon::Mark::Kind::Sunrise || mk.kind == Horizon::Mark::Kind::Sunset;
      if (sun) continue;
      DrawMark(mk, slot++, t, xAt(mk.beyond ? t + Horizon::span : mk.at), th, bk, A, place);
    }
    for (std::size_t i = 0; i < m.bands.size() && (int)i < kBandLabels; ++i) {
      const Horizon::Band &b = m.bands[i];
      Label &l = lBand[i];
      l.set(r, fMarkWhen.get(), b.caption);
      l.shadowAlpha = bk.glyph;
      // At the start of the band, or failing that at its end, or just after a label in the way.
      const float bw = icon + gap + l.w;
      std::vector<float> tries{xAt(b.from), xAt(b.to) - bw};
      for (const Box &p : placed)
        tries.push_back(p.x1 + 16.0f);
      std::optional<float> x0;
      for (float x : tries)
        if (x <= xAt(b.to) && (x0 = place(x, bw, whenB - 14.0f, whenB + 4.0f))) break;
      if (!x0) continue;
      drawIcon(b.snow ? Icon::Snow : Icon::Rain, *x0, whenB - 15.0f, icon, b.snow ? snowCol : th.landAccent, A);
      l.drawBase(r, *x0 + icon + gap, whenB, th.landInk, A);
    }
    for (const Horizon::Mark &mk : m.marks) {
      if (slot >= kMarkLabels) break;
      const bool sun = mk.kind == Horizon::Mark::Kind::Sunrise || mk.kind == Horizon::Mark::Kind::Sunset;
      if (sun) DrawMark(mk, slot++, t, xAt(mk.at), th, bk, A, place);
    }
  }

  template <typename Place>
  void DrawMark(const Horizon::Mark &mk, int slot, std::time_t t, float x, const TextTheme &th, const Backing &bk,
                float A, Place &place) {
    using Kind = Horizon::Mark::Kind;
    SDL_Renderer *r = renderer.get();
    const Col colour = mk.kind == Kind::Call    ? Col{255, 128, 200}
                       : mk.kind == Kind::Event ? Col{184, 150, 255}
                                                : Col{255, 180, 86};
    // How close it is: 0 at the far end of the window (or beyond it), 1 now; and within the last half hour.
    const float near = mk.beyond ? 0.0f : 1.0f - std::clamp((float)(mk.at - t) / (float)Horizon::span, 0.0f, 1.0f);
    const float soon = mk.beyond ? 0.0f : smooth01(1.0f - (float)(mk.at - t) / 1800.0f);
    const bool event = mk.kind == Kind::Event || mk.kind == Kind::Call;
    const float radius = event ? 3.5f + 2.5f * near * near : 3.5f;
    const float lineY = Layout::horizonLineY;

    // The label: an icon, the title, and the time below it.
    Label &title = lMarkTitle[slot], &when = lMarkWhen[slot];
    title.set(r, fMarkTitle.get(), ellipsize(mk.title, 22));
    when.set(r, fMarkWhen.get(), mk.when);
    title.shadowAlpha = when.shadowAlpha = bk.glyph;
    constexpr float icon = 20.0f, gap = 6.0f;
    const float titleB = Layout::horizonTitleBaseline, whenB = Layout::horizonWhenBaseline;
    const float w = icon + gap + std::max(title.w, when.w);
    if (const auto x0 = place(x - icon / 2.0f, w, titleB - 13.0f, whenB + 4.0f)) {
      SDL_Texture *ic = mk.kind == Kind::Call ? icCall.get() : event ? icCalendar.get() : icSun.get();
      const float iconY = (titleB + whenB) / 2.0f - 5.0f;
      drawTex(ic, *x0 + icon / 2.0f, iconY, icon, mix(colour, th.landInk, 0.3f), A);
      const float emphasis = event ? 0.5f + 0.5f * near : 0.5f;
      title.drawBase(r, *x0 + icon + gap, titleB, mix(th.landDim, th.landInk, emphasis), A);
      when.drawBase(r, *x0 + icon + gap, whenB, mix(th.landMute, th.landDim, emphasis), A);
      // A hairline from the label down to its mark, when the label sits right above it.
      if (std::abs(*x0 + icon / 2.0f - x) < 1.0f)
        fillRect(x - 0.5f, whenB + 6.0f, 1.0f, lineY - radius - 2.0f - (whenB + 6.0f),
                 Col{th.landInk.r, th.landInk.g, th.landInk.b, 70.0f * A});
    }

    // The mark, with a soft glow in the last half hour before an event.
    if (event && soon > 0.0f) drawTex(icGlow.get(), x, lineY, radius * 5.0f, colour, 0.45f * soon * A);
    drawTex(icDot.get(), x, lineY, radius * 2.0f + 1.0f, colour, (mk.beyond ? 0.7f : 0.75f + 0.25f * near) * A);
  }
};

SDL_AppResult SDL_AppInit(void **appstate, int, char *[]) {
  // libcurl's global init is not thread-safe: do it once, before the worker thread starts.
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return SDL_APP_FAILURE;
  auto *app = new Clock();
  if (!app->Init()) {
    delete app;
    return SDL_APP_FAILURE;
  }
  *appstate = app;
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
  if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE) return SDL_APP_SUCCESS;
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) { return static_cast<Clock *>(appstate)->Iterate(); }

void SDL_AppQuit(void *appstate, SDL_AppResult) {
  delete static_cast<Clock *>(appstate); // joins the weather thread
  TTF_Quit();
  curl_global_cleanup();
}
