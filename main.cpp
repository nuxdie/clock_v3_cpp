#define SDL_MAIN_USE_CALLBACKS 1

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cpr/cpr.h>
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "assets_fonts.h"
#include "assets_icons.h"

using json = nlohmann::json;

template <typename T, auto Deleter>
using SdlPtr = std::unique_ptr<T, std::integral_constant<decltype(Deleter), Deleter>>;
using WindowPtr = SdlPtr<SDL_Window, SDL_DestroyWindow>;
using RendererPtr = SdlPtr<SDL_Renderer, SDL_DestroyRenderer>;
using SurfacePtr = SdlPtr<SDL_Surface, SDL_DestroySurface>;
using TexturePtr = SdlPtr<SDL_Texture, SDL_DestroyTexture>;
using FontPtr = SdlPtr<TTF_Font, TTF_CloseFont>;

namespace Config {
constexpr int screen_width = 1024;
constexpr int screen_height = 600;
constexpr const char *AppName = "Digital Clock v3";
constexpr const char *AppVersion = "0.3.0";

// GROQ_API_KEY is defined via CMake target_compile_definitions
#ifndef GROQ_API_KEY
constexpr const char *GroqApiKey = "";
#else
constexpr const char *GroqApiKey = GROQ_API_KEY;
#endif

// Layout (logical pixels)
constexpr float pad_x = 56.0f;
constexpr float date_baseline = 84.0f;
constexpr float time_baseline = 362.0f;
constexpr float time_size = 300.0f;
constexpr float strip_baseline = 470.0f; // baseline of the weather numbers
constexpr float rain_cap_baseline = 512.0f;
constexpr float rain_chart_top = 524.0f;
constexpr float rain_chart_h = 30.0f;
constexpr float rain_axis_baseline = 570.0f;
} // namespace Config

// ---------------------------------------------------------------- colour --

struct Col {
  float r = 0, g = 0, b = 0, a = 255; // 0..255, kept as float for cheap mixing
};
constexpr Col mix(const Col &a, const Col &b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

struct Palette {
  Col bg0, bg1, ink, inkDim, inkMute, accent, rain, hair;
};
// night: deep blue ink, warm amber accent, cool rain-blue
constexpr Palette kDark{{13, 20, 29}, {10, 15, 22},  {238, 243, 249}, {125, 138, 160},
                        {77, 87, 105}, {240, 182, 94}, {111, 177, 227}, {255, 255, 255, 23}};
// day: cool off-white, ink text
constexpr Palette kLight{{238, 241, 245}, {228, 232, 238}, {18, 24, 33},  {86, 98, 116},
                         {151, 161, 176}, {193, 125, 28},  {47, 127, 196}, {18, 24, 33, 31}};

Palette mixPalette(float f) {
  return {mix(kDark.bg0, kLight.bg0, f),       mix(kDark.bg1, kLight.bg1, f),     mix(kDark.ink, kLight.ink, f),
          mix(kDark.inkDim, kLight.inkDim, f), mix(kDark.inkMute, kLight.inkMute, f),
          mix(kDark.accent, kLight.accent, f), mix(kDark.rain, kLight.rain, f),   mix(kDark.hair, kLight.hair, f)};
}

// Day factor 0 (night) .. 1 (day), with a one-hour crossfade at 07:00 and 19:00.
float dayFactor(const std::tm &tm) {
  float h = tm.tm_hour + tm.tm_min / 60.0f;
  if (h <= 7.0f || h >= 19.0f) return 0.0f;
  if (h >= 8.0f && h <= 18.0f) return 1.0f;
  return (h < 8.0f) ? (h - 7.0f) : (19.0f - h);
}

// ------------------------------------------------------------- weather ----

enum class Icon { ClearDay, ClearNight, PartlyDay, PartlyNight, Cloudy, Fog, Rain, Snow, Thunder, Wind, COUNT };

Icon iconFor(int code, bool day) {
  switch (code) {
  case 0: return day ? Icon::ClearDay : Icon::ClearNight;
  case 1:
  case 2: return day ? Icon::PartlyDay : Icon::PartlyNight;
  case 3: return Icon::Cloudy;
  case 45:
  case 48: return Icon::Fog;
  case 71:
  case 73:
  case 75:
  case 77:
  case 85:
  case 86: return Icon::Snow;
  case 95:
  case 96:
  case 99: return Icon::Thunder;
  default: return Icon::Rain; // drizzle 51-57, rain 61-67, showers 80-82
  }
}

std::string conditionText(int code) {
  switch (code) {
  case 0: return "clear sky";
  case 1: return "mainly clear";
  case 2: return "partly cloudy";
  case 3: return "overcast";
  case 45: return "fog";
  case 48: return "rime fog";
  case 51: return "light drizzle";
  case 53: return "drizzle";
  case 55: return "dense drizzle";
  case 61: return "light rain";
  case 63: return "rain";
  case 65: return "heavy rain";
  case 71: return "light snow";
  case 73: return "snow";
  case 75: return "heavy snow";
  case 80: return "rain showers";
  case 81: return "rain showers";
  case 82: return "violent rain showers";
  case 95: return "thunderstorm";
  case 96:
  case 99: return "thunderstorm with hail";
  default: return "unknown";
  }
}

std::string basicAdvice(double t) {
  if (t < -10) return "Heavy winter coat, hat, scarf and warm boots.";
  if (t < 0) return "A winter coat and warm layers.";
  if (t < 10) return "A warm coat and a hat.";
  if (t < 20) return "A light jacket or a sweater.";
  return "Light clothing is fine.";
}

constexpr std::array<std::string_view, 7> kWeekdays = {"Sunday",   "Monday", "Tuesday",  "Wednesday",
                                                       "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 12> kMonths = {"January", "February", "March",     "April",   "May",      "June",
                                                      "July",    "August",   "September", "October", "November", "December"};

std::string dateString(const std::tm &tm) {
  return std::format("{}, {} {} {}", kWeekdays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900);
}

std::tm localNow() {
  auto t = std::time(nullptr);
  return *std::localtime(&t);
}

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
  int weathercode = 0;
  std::string advice;
  std::vector<float> rain; // precipitation mm per 15-min step, starting ~now
};

// ------------------------------------------------------------- helpers ----

std::size_t utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

// A cached single-run text texture, always rendered white and tinted at draw time
// so theme colour changes and blinking are free (no re-rasterisation).
struct Label {
  TexturePtr tex;
  float w = 0, h = 0;
  int ascent = 0;
  std::string cache;
  int wrapCache = -1;

  void set(SDL_Renderer *r, TTF_Font *f, const std::string &s, int wrap = 0) {
    if (s == cache && tex && wrap == wrapCache) return;
    cache = s;
    wrapCache = wrap;
    ascent = TTF_GetFontAscent(f);
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
    }
  }

  void drawTop(SDL_Renderer *r, float x, float y, Col c, float alpha = 1.0f) const {
    if (!tex) return;
    SDL_SetTextureColorMod(tex.get(), (Uint8)c.r, (Uint8)c.g, (Uint8)c.b);
    SDL_SetTextureAlphaMod(tex.get(), (Uint8)std::clamp(c.a * alpha, 0.0f, 255.0f));
    SDL_FRect dst{x, y, w, h};
    SDL_RenderTexture(r, tex.get(), nullptr, &dst);
  }
  // place so the text baseline sits at baselineY
  void drawBase(SDL_Renderer *r, float x, float baselineY, Col c, float alpha = 1.0f) const {
    drawTop(r, x, baselineY - ascent, c, alpha);
  }
};

// Per-glyph rendered run so we can apply letter tracking (SDL_ttf has none).
struct TrackedLabel {
  std::string cache;
  float totalW = 0;
  int ascent = 0;
  struct G {
    TexturePtr t;
    float x = 0, w = 0, h = 0;
  };
  std::vector<G> gs;

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
    for (const auto &g : gs) {
      if (!g.t) continue;
      SDL_SetTextureColorMod(g.t.get(), (Uint8)c.r, (Uint8)c.g, (Uint8)c.b);
      SDL_SetTextureAlphaMod(g.t.get(), (Uint8)c.a);
      SDL_FRect dst{ox + g.x, baselineY - ascent, g.w, g.h};
      SDL_RenderTexture(r, g.t.get(), nullptr, &dst);
    }
  }
};

// ------------------------------------------------------------- the app ----

class Clock {
public:
  Clock() = default;
  Clock(const Clock &) = delete;
  Clock &operator=(const Clock &) = delete;

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

    if (!TTF_Init()) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL_ttf: %s", SDL_GetError());
      return false;
    }
    auto open = [](const unsigned char *data, unsigned int len, float size) {
      return FontPtr(TTF_OpenFontIO(SDL_IOFromConstMem(data, len), true, size));
    };
    fTime = open(SpaceGrotesk_Medium_ttf, SpaceGrotesk_Medium_ttf_len, Config::time_size);
    fTempNum = open(SpaceGrotesk_Medium_ttf, SpaceGrotesk_Medium_ttf_len, 46.0f);
    fWindNum = open(SpaceGrotesk_Medium_ttf, SpaceGrotesk_Medium_ttf_len, 30.0f);
    fUnitLg = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 22.0f);
    fUnitSm = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 16.0f);
    fDate = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 22.0f);
    fAxis = open(Inter_Medium_ttf, Inter_Medium_ttf_len, 12.0f);
    fAdvice = open(VictorMono_Italic_ttf, VictorMono_Italic_ttf_len, 33.0f);
    fRainCap = open(VictorMono_Italic_ttf, VictorMono_Italic_ttf_len, 21.0f);
    if (!fTime || !fTempNum || !fWindNum || !fUnitLg || !fUnitSm || !fDate || !fAxis || !fAdvice || !fRainCap) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't load embedded fonts: %s", SDL_GetError());
      return false;
    }

    // Prefer proper typographic glyphs where the font carries them.
    windUnit = TTF_FontHasGlyph(fUnitSm.get(), 0x2044) ? "m\xE2\x81\x84s" : "m/s"; // m⁄s
    approx = TTF_FontHasGlyph(fRainCap.get(), 0x2248) ? "\xE2\x89\x88" : "~";      // ≈

    LoadIcons();

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

    weatherLoaderThread = std::jthread(&Clock::FetchWeather, this);
    return true;
  }

  SDL_AppResult Iterate() {
    Render();
    if (shotPath && ++frameCount >= shotFrame) return SDL_APP_SUCCESS;
    SDL_Delay(16);
    return SDL_APP_CONTINUE;
  }

private:
  WindowPtr window;
  RendererPtr renderer;

  FontPtr fTime, fTempNum, fWindNum, fUnitLg, fUnitSm, fDate, fAxis, fAdvice, fRainCap;
  std::array<TexturePtr, (std::size_t)Icon::COUNT> icons;
  std::string windUnit = "m/s";
  std::string approx = "~";
  const std::string deg = "\xC2\xB0";   // °
  const std::string mdot = " \xC2\xB7 "; // · with spaces

  std::jthread weatherLoaderThread;
  std::mutex weatherMutex;
  WeatherState weather;

  const char *shotPath = nullptr;
  int shotFrame = 180;
  int frameCount = 0;

  // Cached label runs
  TrackedLabel lDate;
  Label lHH, lColon, lMM;
  Label lTempNum, lTempUnit, lWindNum, lWindUnit;
  Label lAdvice, lRainCap, lRainDry;
  Label lAxisNow, lAxisMid, lAxisEnd;

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

  void fillRect(float x, float y, float w, float h, Col c) {
    SDL_SetRenderDrawColor(renderer.get(), (Uint8)c.r, (Uint8)c.g, (Uint8)c.b, (Uint8)c.a);
    SDL_FRect r{x, y, w, h};
    SDL_RenderFillRect(renderer.get(), &r);
  }

  void fillCircle(float cx, float cy, float r, Col c) {
    constexpr int seg = 20;
    SDL_FColor fc{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
    SDL_Vertex v[seg + 1];
    int idx[seg * 3];
    v[0] = {{cx, cy}, fc, {0, 0}};
    for (int i = 0; i < seg; ++i) {
      float a = (float)i / seg * 2.0f * (float)M_PI;
      v[i + 1] = {{cx + std::cos(a) * r, cy + std::sin(a) * r}, fc, {0, 0}};
      idx[i * 3] = 0;
      idx[i * 3 + 1] = i + 1;
      idx[i * 3 + 2] = (i + 1) % seg + 1;
    }
    SDL_RenderGeometry(renderer.get(), nullptr, v, seg + 1, idx, seg * 3);
  }

  void drawGradient(Col top, Col bot) {
    SDL_FColor ct{top.r / 255.f, top.g / 255.f, top.b / 255.f, 1.f};
    SDL_FColor cb{bot.r / 255.f, bot.g / 255.f, bot.b / 255.f, 1.f};
    const float W = Config::screen_width, H = Config::screen_height;
    SDL_Vertex v[4] = {
        {{0, 0}, ct, {0, 0}}, {{W, 0}, ct, {0, 0}}, {{W, H}, cb, {0, 0}}, {{0, H}, cb, {0, 0}}};
    int idx[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(renderer.get(), nullptr, v, 4, idx, 6);
  }

  // --------------------------------------------------------------- data --
  void FetchWeather(std::stop_token stopToken) {
    const auto url = cpr::Url{"https://api.open-meteo.com/v1/forecast"};
    const auto params = cpr::Parameters{{"latitude", "52.3738"},         {"longitude", "4.8910"},
                                        {"current_weather", "true"},     {"minutely_15", "precipitation"},
                                        {"windspeed_unit", "ms"},        {"forecast_days", "2"},
                                        {"timezone", "auto"}};

    while (!stopToken.stop_requested()) {
      bool ok = false;
      double temp = 0, wind = 0;
      int code = 0;
      std::vector<float> rain;
      try {
        cpr::Response resp = cpr::Get(url, params, cpr::Timeout{60000});
        if (resp.status_code == 200) {
          auto j = json::parse(resp.text);
          const auto &cw = j.at("current_weather");
          temp = cw.at("temperature").get<double>();
          wind = cw.at("windspeed").get<double>();
          code = cw.at("weathercode").get<int>();

          if (j.contains("minutely_15")) {
            const auto &m = j.at("minutely_15");
            const auto &times = m.at("time");
            const auto &precip = m.at("precipitation");
            std::time_t now = std::time(nullptr);
            std::size_t start = 0;
            while (start < times.size() && parseTs(times[start].get<std::string>()) < now - 450) start++;
            for (std::size_t i = start; i < start + 8 && i < precip.size(); ++i) {
              rain.push_back(precip[i].is_null() ? 0.0f : (float)precip[i].get<double>());
            }
          }
          ok = true;
        }
      } catch (const std::exception &e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Weather fetch failed: %s", e.what());
      }

      std::string advice;
      if (ok) advice = FetchAdvice(temp, code);

      {
        std::scoped_lock lock(weatherMutex);
        if (ok) {
          weather.valid = true;
          weather.temperature = temp;
          weather.windspeed = wind;
          weather.weathercode = code;
          weather.rain = std::move(rain);
          weather.advice = std::move(advice);
        } else {
          weather = WeatherState{};
        }
      }

      std::mutex m;
      std::unique_lock lock(m);
      std::condition_variable_any().wait_for(lock, stopToken, std::chrono::minutes(5),
                                             [&stopToken] { return stopToken.stop_requested(); });
    }
  }

  std::string FetchAdvice(double temp, int code) {
    std::string apiKey = Config::GroqApiKey;
    if (apiKey.empty() || apiKey == "MISSING_KEY") return basicAdvice(temp);
    try {
      std::tm tm = localNow();
      std::string prompt = std::format(
          "I live in Amsterdam. Today is {}, the time is {:02}:{:02} and the weather is {} ({:.0f}C). "
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
          cpr::Timeout{60000});
      if (r.status_code == 200) {
        auto j = json::parse(r.text);
        std::string out = j.at("choices").at(0).at("message").at("content").get<std::string>();
        if (out.size() > 1 && out.front() == '"' && out.back() == '"') out = out.substr(1, out.size() - 2);
        if (!out.empty()) return out;
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
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
  }

  void Render() {
    std::tm tm = localNow();
    float f = dayFactor(tm);
    bool isDay = f >= 0.5f;
    Palette p = mixPalette(f);

    WeatherState w;
    {
      std::scoped_lock lock(weatherMutex);
      w = weather;
    }

    drawGradient(p.bg0, p.bg1);
    DrawDate(tm, p);
    DrawTime(tm, p);
    DrawWeatherStrip(w, isDay, p);
    DrawRain(w, p);

    if (shotPath && frameCount + 1 >= shotFrame) {
      if (SDL_Surface *s = SDL_RenderReadPixels(renderer.get(), nullptr)) {
        IMG_SavePNG(s, shotPath);
        SDL_DestroySurface(s);
      }
    }
    SDL_RenderPresent(renderer.get());
  }

  void DrawDate(const std::tm &tm, const Palette &p) {
    lDate.set(renderer.get(), fDate.get(), toUpper(dateString(tm)), 3.2f); // tracked caps
    // amber dot then date
    float dotR = 3.6f;
    float dotX = Config::pad_x;
    float baseline = Config::date_baseline;
    fillCircle(dotX + dotR, baseline - lDate.ascent * 0.42f, dotR, p.accent);
    lDate.draw(renderer.get(), dotX + dotR * 2 + 12.0f, baseline, p.inkDim);
  }

  void DrawTime(const std::tm &tm, const Palette &p) {
    lHH.set(renderer.get(), fTime.get(), std::format("{:02}", tm.tm_hour));
    lMM.set(renderer.get(), fTime.get(), std::format("{:02}", tm.tm_min));
    lColon.set(renderer.get(), fTime.get(), ":");

    float gap = Config::time_size * 0.02f;
    float total = lHH.w + gap + lColon.w + gap + lMM.w;
    float x = (Config::screen_width - total) / 2.0f;
    float base = Config::time_baseline;

    // gentle colon pulse
    float ph = (float)(SDL_GetTicks() % 2000) / 2000.0f;
    float pulse = 0.45f + 0.55f * (0.5f + 0.5f * std::cos(ph * 2.0f * (float)M_PI));

    lHH.drawBase(renderer.get(), x, base, p.ink);
    x += lHH.w + gap;
    lColon.drawBase(renderer.get(), x, base - Config::time_size * 0.02f, p.accent, pulse);
    x += lColon.w + gap;
    lMM.drawBase(renderer.get(), x, base, p.ink);
  }

  void DrawWeatherStrip(const WeatherState &w, bool isDay, const Palette &p) {
    const float B = Config::strip_baseline;
    std::string tempStr = w.valid ? std::format("{:.0f}", w.temperature) : "--";
    std::string windStr = w.valid ? std::format("{:.0f}", w.windspeed) : "--";
    lTempNum.set(renderer.get(), fTempNum.get(), tempStr);
    lTempUnit.set(renderer.get(), fUnitLg.get(), deg + "C");
    lWindNum.set(renderer.get(), fWindNum.get(), windStr);
    lWindUnit.set(renderer.get(), fUnitSm.get(), windUnit);

    // --- temperature cell: [condition icon] NN °C ---
    float x = Config::pad_x;
    Icon cond = w.valid ? iconFor(w.weathercode, isDay) : Icon::Cloudy;
    float tIcon = 40.0f;
    float numCenter = B - lTempNum.ascent * 0.36f; // rough optical centre of the figures
    drawIcon(cond, x, numCenter - tIcon / 2.0f, tIcon, p.inkDim);
    x += tIcon + 16.0f;
    lTempNum.drawBase(renderer.get(), x, B, p.ink);
    x += lTempNum.w + 6.0f;
    lTempUnit.drawBase(renderer.get(), x, B, p.inkDim);
    x += lTempUnit.w;

    // divider
    float divTop = B - 34.0f, divBot = B + 6.0f;
    float d1 = x + 30.0f;
    fillRect(d1, divTop, 1.0f, divBot - divTop, p.hair);

    // --- wind cell: [wind icon] N m/s ---
    x = d1 + 30.0f;
    float wIcon = 27.0f;
    float wNumCenter = B - lWindNum.ascent * 0.36f;
    drawIcon(Icon::Wind, x, wNumCenter - wIcon / 2.0f, wIcon, p.inkDim);
    x += wIcon + 12.0f;
    lWindNum.drawBase(renderer.get(), x, B, p.ink);
    x += lWindNum.w + 5.0f;
    lWindUnit.drawBase(renderer.get(), x, B, p.inkDim);
    x += lWindUnit.w;

    float d2 = x + 30.0f;
    fillRect(d2, divTop, 1.0f, divBot - divTop, p.hair);

    // --- advice cell: cursive, right-aligned, fills remaining width ---
    float adviceRight = Config::screen_width - Config::pad_x;
    int wrapW = (int)std::clamp(adviceRight - (d2 + 30.0f), 220.0f, 470.0f);
    lAdvice.set(renderer.get(), fAdvice.get(), w.advice, wrapW);
    if (lAdvice.tex) {
      float ax = adviceRight - lAdvice.w;
      float ay = (numCenter) - lAdvice.h / 2.0f + 4.0f;
      lAdvice.drawTop(renderer.get(), ax, ay, p.ink);
    }
  }

  void DrawRain(const WeatherState &w, const Palette &p) {
    const float left = Config::pad_x;
    const float right = Config::screen_width - Config::pad_x;
    const float CW = right - left;

    bool hasRain = false;
    float peak = 0;
    int peakIdx = 0;
    for (std::size_t i = 0; i < w.rain.size(); ++i) {
      if (w.rain[i] > peak) {
        peak = w.rain[i];
        peakIdx = (int)i;
      }
      if (w.rain[i] > 0.05f) hasRain = true;
    }

    if (!w.valid || !hasRain) {
      std::string msg = w.valid ? "No rain expected \xC2\xB7 next 2h" : "Checking the sky\xE2\x80\xA6";
      lRainDry.set(renderer.get(), fRainCap.get(), msg);
      lRainDry.drawBase(renderer.get(), left + (CW - lRainDry.w) / 2.0f, Config::rain_chart_top + 24.0f, p.inkDim);
      return;
    }

    // caption
    const char *word = peak < 0.3f ? "Light" : (peak < 1.0f ? "Moderate" : "Heavy");
    int mins = peakIdx * 15;
    std::string cap = mins <= 0 ? std::format("{} rain{}peak now", word, mdot)
                                : std::format("{} rain{}peak in {}{} min", word, mdot, approx, mins);
    lRainCap.set(renderer.get(), fRainCap.get(), cap);
    lRainCap.drawBase(renderer.get(), left, Config::rain_cap_baseline, p.rain);

    // bars
    const float chartBottom = Config::rain_chart_top + Config::rain_chart_h;
    fillRect(left, chartBottom, CW, 1.0f, p.hair);
    std::size_t n = w.rain.size();
    if (n == 0) return;
    const float gap = 6.0f;
    const float bw = (CW - gap * (n - 1)) / n;
    const float scale = 2.0f; // mm per 15 min mapped to full height
    for (std::size_t i = 0; i < n; ++i) {
      float hh = std::clamp(w.rain[i] / scale, 0.0f, 1.0f) * (Config::rain_chart_h - 1.0f);
      if (hh < 1.0f) continue;
      fillRect(left + i * (bw + gap), chartBottom - hh, bw, hh, p.rain);
    }

    // axis
    lAxisNow.set(renderer.get(), fAxis.get(), "NOW");
    lAxisMid.set(renderer.get(), fAxis.get(), "+1H");
    lAxisEnd.set(renderer.get(), fAxis.get(), "+2H");
    lAxisNow.drawBase(renderer.get(), left, Config::rain_axis_baseline, p.inkMute);
    lAxisMid.drawBase(renderer.get(), left + (CW - lAxisMid.w) / 2.0f, Config::rain_axis_baseline, p.inkMute);
    lAxisEnd.drawBase(renderer.get(), right - lAxisEnd.w, Config::rain_axis_baseline, p.inkMute);
  }
};

SDL_AppResult SDL_AppInit(void **appstate, int, char *[]) {
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
  delete static_cast<Clock *>(appstate);
  TTF_Quit();
}
