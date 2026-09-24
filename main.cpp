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
#include <cstring>
#include <ctime>
#include <execution>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "font_data.h"

using namespace std::string_literals;
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
constexpr int font_big_size = 382;
constexpr int font_normal_size = 48;
constexpr int font_small_size = 32;
// Dark outline thickness per font. It is baked into the text textures, so it costs nothing per frame
// and keeps the text readable on any part of any picture.
constexpr int font_big_outline = 7;
constexpr int font_normal_outline = 3;
constexpr int font_small_outline = 2;
constexpr float background_fade_seconds = 1.5f;
// SDL_AppIterate rate while nothing animates: the clock only needs to notice the minute change.
constexpr const char *idle_iterate_rate_hz = "4";
constexpr auto http_connect_timeout = std::chrono::seconds(15);
constexpr auto http_timeout = std::chrono::seconds(60);
constexpr int num_snowflakes = 666;
constexpr const char *AppName = "Digital Clock v3";
constexpr const char *AppVersion = "0.3.0";

// GROQ_API_KEY is defined via CMake target_compile_definitions
#ifndef GROQ_API_KEY
constexpr const char *GroqApiKey = "";
#else
constexpr const char *GroqApiKey = GROQ_API_KEY;
#endif
} // namespace Config

struct BingImage {
  std::string fullUrl;
  std::string date; // format "2025-11-22"
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BingImage, fullUrl, date)

struct CurrentWeather {
  double temperature;
  double windspeed;
  int weathercode;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CurrentWeather, temperature, windspeed, weathercode)

struct WeatherData {
  CurrentWeather current_weather;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WeatherData, current_weather)

struct LlmMessage {
  std::string role;
  std::string content;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LlmMessage, role, content)
struct LlmChoice {
  int index;
  LlmMessage message;
  std::string finish_reason;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LlmChoice, index, message, finish_reason)
struct LlmResponse {
  std::string id;
  std::vector<LlmChoice> choices;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LlmResponse, id, choices)

namespace {
const std::map<int, std::string_view> WEATHER_CODE_RU = {{0, "ясно"},
                                                         {1, "редкие облака"},
                                                         {2, "переменная облачность"},
                                                         {3, "облачно"},
                                                         {45, "туман"},
                                                         {48, "изморозь"},
                                                         {51, "легкая морось"},
                                                         {53, "моросит"},
                                                         {55, "плотно моросит"},
                                                         {56, "ледяная морось"},
                                                         {57, "тяжелая ледяная морось"},
                                                         {61, "легкий дождик"},
                                                         {63, "дождь"},
                                                         {65, "ливень"},
                                                         {66, "холодный дождь"},
                                                         {67, "ледяной ливень"},
                                                         {71, "снежок"},
                                                         {73, "снегопад"},
                                                         {75, "сильный снегопад"},
                                                         {77, "снежный град"},
                                                         {80, "ливневый дождик"},
                                                         {81, "ливни"},
                                                         {82, "плотные ливни"},
                                                         {85, "снежный дождик"},
                                                         {86, "снежные дожди"},
                                                         {95, "небольшая гроза"},
                                                         {96, "гроза с маленьким градом"},
                                                         {99, "град с грозой"}};
[[nodiscard]] std::string_view getWindspeedType(double windspeed) {
  if (windspeed < 1.0) return "штиль";
  if (windspeed <= 5.0) return "ветерок";
  if (windspeed <= 10.0) return "ветер";
  if (windspeed <= 15.0) return "сильный ветер";
  if (windspeed <= 20.0) return "шквальный ветер";
  return "ураган";
}
[[nodiscard]] std::string getBasicAdvice(double temperature) {
  if (temperature < -10) {
    return "Наденьте теплую зимнюю куртку, шапку, шарф и теплые ботинки.";
  } else if (temperature < 0) {
    return "Наденьте зимнюю куртку и теплые аксессуары.";
  } else if (temperature < 10) {
    return "Наденьте куртку и шапку.";
  } else if (temperature < 20) {
    return "Наденьте легкую куртку или свитер.";
  } else {
    return "Наденьте легкую одежду.";
  }
}
constexpr std::array<std::string_view, 7> weekdays = {"воскресенье", "понедельник", "вторник", "среда",
                                                      "четверг",     "пятница",     "суббота"};
constexpr std::array<std::string_view, 12> months = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                                                     "июля",   "августа", "сентября", "октября", "ноября", "декабря"};
} // namespace

// std::localtime returns a pointer to shared static storage, which races between the render thread and the
// weather thread. localtime_r fills a caller-owned struct instead.
std::tm getLocalTime() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  return tm;
}

std::string getCurrentTime() {
  const auto tm = getLocalTime();
  return std::format("{}:{:02}", tm.tm_hour, tm.tm_min);
}

std::string getCurrentDate() {
  const auto tm = getLocalTime();
  return std::format("{}, {} {} {} года", weekdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
}

// Interruptible sleep: returns early when the owning std::jthread is asked to stop.
void sleepFor(const std::stop_token &stopToken, std::chrono::milliseconds duration) {
  std::mutex sleepMutex;
  std::unique_lock lock(sleepMutex);
  std::condition_variable_any().wait_for(lock, stopToken, duration, [] { return false; });
}

// Every request gets hard timeouts and aborts as soon as a stop is requested, so a hung server can neither
// freeze a worker thread forever nor delay shutdown.
cpr::ProgressCallback abortOnStop(std::stop_token stopToken) {
  return cpr::ProgressCallback{[stopToken](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                           intptr_t) { return !stopToken.stop_requested(); }};
}

template <typename... Options>
cpr::Response httpGet(const std::stop_token &stopToken, const cpr::Url &url, Options &&...options) {
  return cpr::Get(url, cpr::ConnectTimeout{Config::http_connect_timeout}, cpr::Timeout{Config::http_timeout},
                  abortOnStop(stopToken), std::forward<Options>(options)...);
}

// Turns a photo into a painting and picks text colours from it. Everything here runs once per picture on a
// low-priority worker thread, so the render loop only ever draws one plain texture.
namespace Painter {
struct Rgb {
  float r, g, b;
};

struct Palette {
  SDL_Color hours;   // accent taken from the dominant hue of the painting
  SDL_Color minutes; // second accent: another strong hue of the painting
  SDL_Color neutral; // date, colon, weather: warm off-white
  SDL_Color outline; // very dark shade that separates every glyph from the picture
};

struct Painting {
  SurfacePtr surface; // RGBA32, exactly screen sized
  Palette palette;
};

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float relativeLuminance(Rgb c) {
  return 0.2126f * srgbToLinear(c.r) + 0.7152f * srgbToLinear(c.g) + 0.0722f * srgbToLinear(c.b);
}

Rgb hsvToRgb(float h, float s, float v) {
  h = (h - std::floor(h)) * 6.0f;
  const int sector = static_cast<int>(h) % 6;
  const float f = h - std::floor(h);
  const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
  switch (sector) {
  case 0:
    return {v, t, p};
  case 1:
    return {q, v, p};
  case 2:
    return {p, v, t};
  case 3:
    return {p, q, v};
  case 4:
    return {t, p, v};
  default:
    return {v, p, q};
  }
}

SDL_Color toColor(Rgb c, Uint8 alpha = SDL_ALPHA_OPAQUE) {
  auto byte = [](float v) { return static_cast<Uint8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return {byte(c.r), byte(c.g), byte(c.b), alpha};
}

// The most colourful tint of `hue` that is still bright enough to stand out against the dark outline.
// Blue and purple hues are dark by nature, so they get desaturated further than yellow or green.
Rgb readableTint(float hue) {
  constexpr float minLuminance = 0.5f;
  for (float s = 0.6f; s > 0.0f; s -= 0.02f) {
    if (const Rgb c = hsvToRgb(hue, s, 1.0f); relativeLuminance(c) >= minLuminance) return c;
  }
  return {1.0f, 1.0f, 1.0f};
}

Palette makePalette(float primaryHue, float secondaryHue) {
  return {toColor(readableTint(primaryHue)), toColor(readableTint(secondaryHue)),
          toColor(hsvToRgb(primaryHue, 0.06f, 1.0f)), toColor(hsvToRgb(primaryHue, 0.6f, 0.08f), 230)};
}

Palette defaultPalette() { return makePalette(0.09f, 0.52f); } // amber and teal

SurfacePtr coverScale(SDL_Surface *src, int width, int height) {
  SurfacePtr rgba(SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32));
  SurfacePtr dst(SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32));
  if (!rgba || !dst) return {};
  // Same as CSS "object-fit: cover": scale to fill, crop the overflow evenly on both sides.
  const float scale = std::max(static_cast<float>(width) / rgba->w, static_cast<float>(height) / rgba->h);
  const int cropW = std::min(rgba->w, static_cast<int>(std::lround(width / scale)));
  const int cropH = std::min(rgba->h, static_cast<int>(std::lround(height / scale)));
  const SDL_Rect crop{(rgba->w - cropW) / 2, (rgba->h - cropH) / 2, cropW, cropH};
  SDL_SetSurfaceBlendMode(rgba.get(), SDL_BLENDMODE_NONE);
  if (!SDL_BlitSurfaceScaled(rgba.get(), &crop, dst.get(), nullptr, SDL_SCALEMODE_LINEAR)) return {};
  return dst;
}

// Working image for the brush-stroke painter: RGB floats in 0..1.
struct Image {
  int w = 0, h = 0;
  std::vector<float> px;

  Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 3) {}
  float *at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 3]; }
  [[nodiscard]] const float *at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 3]; }
  [[nodiscard]] float luma(int x, int y) const {
    const float *p = at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1));
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
  }
};

Image fromSurface(const SDL_Surface *surface) {
  Image image(surface->w, surface->h);
  for (int y = 0; y < image.h; ++y) {
    const Uint8 *src = static_cast<const Uint8 *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    float *dst = image.at(0, y);
    for (int x = 0; x < image.w; ++x, src += 4, dst += 3) {
      dst[0] = src[0] / 255.0f, dst[1] = src[1] / 255.0f, dst[2] = src[2] / 255.0f;
    }
  }
  return image;
}

void toSurface(const Image &image, SDL_Surface *surface) {
  for (int y = 0; y < image.h; ++y) {
    Uint8 *dst = static_cast<Uint8 *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    const float *src = image.at(0, y);
    for (int x = 0; x < image.w; ++x, src += 3, dst += 4) {
      *reinterpret_cast<SDL_Color *>(dst) = toColor({src[0], src[1], src[2]});
    }
  }
}

// Separable box blur with running sums: cost is independent of the radius.
Image boxBlur(const Image &src, int radius) {
  Image tmp(src.w, src.h), out(src.w, src.h);
  const float norm = 1.0f / static_cast<float>(2 * radius + 1);
  auto pass = [&](const Image &in, Image &result, int length, int lines, auto &&pixel) {
    for (int line = 0; line < lines; ++line) {
      for (int c = 0; c < 3; ++c) {
        float sum = 0.0f;
        for (int i = -radius; i <= radius; ++i)
          sum += pixel(in, std::clamp(i, 0, length - 1), line)[c];
        for (int i = 0; i < length; ++i) {
          pixel(result, i, line)[c] = sum * norm;
          sum += pixel(in, std::min(i + radius + 1, length - 1), line)[c] - pixel(in, std::max(i - radius, 0), line)[c];
        }
      }
    }
  };
  pass(src, tmp, src.w, src.h, [](auto &img, int i, int line) { return img.at(i, line); });
  pass(tmp, out, src.h, src.w, [](auto &img, int i, int line) { return img.at(line, i); });
  return out;
}

// One elongated, soft-edged dab of paint with faint bristle streaks running along it.
void drawStroke(Image &canvas, float cx, float cy, float angle, float halfLength, float halfWidth, const float *color,
                float bristlePhase) {
  const float c = std::cos(angle), s = std::sin(angle);
  const float ex = std::abs(c) * halfLength + std::abs(s) * halfWidth;
  const float ey = std::abs(s) * halfLength + std::abs(c) * halfWidth;
  const int x0 = std::max(0, static_cast<int>(cx - ex)), x1 = std::min(canvas.w - 1, static_cast<int>(cx + ex) + 1);
  const int y0 = std::max(0, static_cast<int>(cy - ey)), y1 = std::min(canvas.h - 1, static_cast<int>(cy + ey) + 1);
  for (int y = y0; y <= y1; ++y) {
    float *p = canvas.at(x0, y);
    for (int x = x0; x <= x1; ++x, p += 3) {
      const float dx = x - cx, dy = y - cy;
      const float u = (dx * c + dy * s) / halfLength, v = (dy * c - dx * s) / halfWidth;
      const float d = u * u + v * v;
      if (d >= 1.0f) continue;
      const float alpha = 0.9f * std::min(1.0f, (1.0f - d) * 3.0f);
      const float bristle = 0.95f + 0.05f * std::sin(v * 11.0f + bristlePhase);
      for (int k = 0; k < 3; ++k)
        p[k] += (color[k] * bristle - p[k]) * alpha;
    }
  }
}

// Painterly rendering in the spirit of Hertzmann (1998): a blurry underpainting, then layers of ever smaller brush
// strokes that follow the edges of the picture. Finer layers only touch areas that still differ from the photo,
// so large calm areas keep their broad strokes and details get small ones.
Image paintStrokes(const Image &reference, std::mt19937 &rng) {
  struct Layer {
    float halfWidth;
    float errorThreshold; // mean RGB difference below which a spot is left as is
  };
  constexpr std::array<Layer, 3> layers = {Layer{9.0f, 0.0f}, Layer{4.5f, 0.06f}, Layer{2.2f, 0.1f}};
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);

  const Image contours = boxBlur(reference, 3); // stroke directions come from its (smoothed) gradient
  Image canvas = boxBlur(reference, 12);
  struct Stroke {
    float x, y, angle, halfLength;
    std::array<float, 3> color;
  };
  std::vector<Stroke> strokes;
  for (const auto &layer : layers) {
    const Image target = boxBlur(reference, std::max(1, static_cast<int>(layer.halfWidth * 0.5f)));
    const float step = layer.halfWidth * 1.5f;
    strokes.clear();
    for (float gy = 0.0f; gy < canvas.h; gy += step) {
      for (float gx = 0.0f; gx < canvas.w; gx += step) {
        const int x = std::min(canvas.w - 1, static_cast<int>(gx + unit(rng) * step));
        const int y = std::min(canvas.h - 1, static_cast<int>(gy + unit(rng) * step));
        const float *want = target.at(x, y), *have = canvas.at(x, y);
        const float error =
            (std::abs(want[0] - have[0]) + std::abs(want[1] - have[1]) + std::abs(want[2] - have[2])) / 3;
        if (error < layer.errorThreshold) continue;
        const float gxL = contours.luma(x + 2, y) - contours.luma(x - 2, y);
        const float gyL = contours.luma(x, y + 2) - contours.luma(x, y - 2);
        // Strokes run along edges; where the picture is flat they follow a gentle wavy flow instead.
        const float angle = (gxL * gxL + gyL * gyL > 1e-4f) ? std::atan2(gyL, gxL) + std::numbers::pi_v<float> / 2.0f
                                                            : 0.35f * std::sin(x * 0.011f + y * 0.006f) + 0.2f;
        const float shade = 0.94f + unit(rng) * 0.12f; // slight per-stroke variation, like remixed paint
        strokes.push_back({static_cast<float>(x),
                           static_cast<float>(y),
                           angle,
                           layer.halfWidth * (2.0f + unit(rng) * 1.5f),
                           {want[0] * shade, want[1] * shade, want[2] * shade}});
      }
    }
    std::ranges::shuffle(strokes, rng); // no visible scan order
    for (const auto &st : strokes) {
      drawStroke(canvas, st.x, st.y, st.angle, st.halfLength, layer.halfWidth, st.color.data(), unit(rng) * 6.3f);
    }
  }
  return canvas;
}

// Richer colours, a brightness cap so the brightest parts of the picture still sit well below the text,
// a soft vignette, and a faint canvas grain.
void grade(SDL_Surface *surface) {
  constexpr float saturationBoost = 1.35f;
  constexpr float maxLuma = 160.0f; // the 90th luma percentile is pulled down to this (0..255)
  const int w = surface->w, h = surface->h;
  std::array<uint32_t, 256> histogram{};

  for (int y = 0; y < h; ++y) {
    Uint8 *px = static_cast<Uint8 *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    for (int x = 0; x < w; ++x, px += 4) {
      const float luma = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
      for (int c = 0; c < 3; ++c) {
        px[c] = static_cast<Uint8>(std::clamp(luma + (px[c] - luma) * saturationBoost, 0.0f, 255.0f));
      }
      ++histogram[(77u * px[0] + 150u * px[1] + 29u * px[2]) >> 8];
    }
  }

  const uint32_t target = static_cast<uint32_t>(static_cast<uint64_t>(w) * h * 9 / 10);
  uint32_t seen = 0;
  int p90 = 255;
  for (int i = 0; i < 256; ++i) {
    if ((seen += histogram[i]) >= target) {
      p90 = i;
      break;
    }
  }
  const float gain = std::min(1.0f, maxLuma / static_cast<float>(std::max(p90, 1)));

  const float cx = w * 0.5f, cy = h * 0.5f, maxDist2 = cx * cx + cy * cy;
  for (int y = 0; y < h; ++y) {
    Uint8 *px = static_cast<Uint8 *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    for (int x = 0; x < w; ++x, px += 4) {
      const float d2 = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxDist2;
      uint32_t hash = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
      hash ^= hash >> 13, hash *= 0x5bd1e995u, hash ^= hash >> 15;
      const float grain = static_cast<float>(hash & 7u) - 3.5f;
      const float k = gain * (1.0f - 0.3f * d2);
      for (int c = 0; c < 3; ++c)
        px[c] = static_cast<Uint8>(std::clamp(px[c] * k + grain, 0.0f, 255.0f));
    }
  }
}

// The two strongest hues of the painting, weighted by how colourful each pixel is.
Palette extractPalette(SDL_Surface *surface) {
  constexpr int bins = 24;
  std::array<double, bins> weight{};
  for (int y = 0; y < surface->h; y += 2) {
    const Uint8 *px = static_cast<const Uint8 *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    for (int x = 0; x < surface->w; x += 2) {
      const float r = px[x * 4] / 255.0f, g = px[x * 4 + 1] / 255.0f, b = px[x * 4 + 2] / 255.0f;
      const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), chroma = mx - mn;
      if (chroma < 0.08f) continue; // greys say nothing about the mood of the picture
      float hue = mx == r ? (g - b) / chroma : mx == g ? 2.0f + (b - r) / chroma : 4.0f + (r - g) / chroma;
      hue = hue / 6.0f - std::floor(hue / 6.0f);
      weight[static_cast<int>(hue * bins) % bins] += chroma * mx;
    }
  }
  std::array<double, bins> smooth{};
  for (int i = 0; i < bins; ++i) {
    smooth[i] = 0.25 * weight[(i + bins - 1) % bins] + 0.5 * weight[i] + 0.25 * weight[(i + 1) % bins];
  }
  const int first = static_cast<int>(std::ranges::max_element(smooth) - smooth.begin());
  if (smooth[first] <= 0.0) return defaultPalette();

  int second = -1;
  for (int i = 0; i < bins; ++i) {
    const int distance = std::min((i - first + bins) % bins, (first - i + bins) % bins);
    if (distance >= bins / 6 && (second < 0 || smooth[i] > smooth[second])) second = i;
  }
  const float firstHue = (first + 0.5f) / bins;
  // A picture with one dominant colour gets a complementary second accent instead of a muddy one.
  const float secondHue =
      (second >= 0 && smooth[second] >= 0.15 * smooth[first]) ? (second + 0.5f) / bins : firstHue + 0.5f;
  return makePalette(firstHue, secondHue);
}

// An abstract colour-field canvas used until the first photo arrives (or when there is no network), so the
// screen is never a black void.
SurfacePtr makeCanvas(int width, int height, std::mt19937 &rng) {
  SurfacePtr surface(SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32));
  if (!surface) return {};
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  const float baseHue = unit(rng);
  const Rgb top = hsvToRgb(baseHue, 0.7f, 0.35f), bottom = hsvToRgb(baseHue + 0.12f, 0.75f, 0.6f);
  struct Blob {
    float x, y, radius2;
    Rgb color;
  };
  std::vector<Blob> blobs(7);
  for (auto &b : blobs) {
    const float radius = 90.0f + unit(rng) * 200.0f;
    b = {unit(rng) * width, unit(rng) * height, radius * radius,
         hsvToRgb(baseHue + (unit(rng) - 0.5f) * 0.6f, 0.6f + unit(rng) * 0.3f, 0.5f + unit(rng) * 0.4f)};
  }
  std::uniform_real_distribution<float> noise(-0.03f, 0.03f);
  for (int y = 0; y < height; ++y) {
    Uint8 *px = static_cast<Uint8 *>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    const float t = static_cast<float>(y) / height;
    for (int x = 0; x < width; ++x, px += 4) {
      Rgb c{top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t, top.b + (bottom.b - top.b) * t};
      for (const auto &b : blobs) {
        const float d2 = (x - b.x) * (x - b.x) + (y - b.y) * (y - b.y);
        const float a = 0.85f * std::exp(-d2 / b.radius2);
        c = {c.r + (b.color.r - c.r) * a, c.g + (b.color.g - c.g) * a, c.b + (b.color.b - c.b) * a};
      }
      const float n = noise(rng); // gives the paint filter some texture to turn into strokes
      *reinterpret_cast<SDL_Color *>(px) = toColor({c.r + n, c.g + n, c.b + n});
    }
  }
  return surface;
}

std::optional<Painting> paint(SDL_Surface *source, std::mt19937 &rng) {
  const Uint64 startNs = SDL_GetTicksNS();
  SurfacePtr canvas = coverScale(source, Config::screen_width, Config::screen_height);
  if (!canvas) return std::nullopt;
  toSurface(paintStrokes(fromSurface(canvas.get()), rng), canvas.get());
  grade(canvas.get());
  Palette palette = extractPalette(canvas.get());
  SDL_Log("Painted background in %.0f ms", static_cast<double>(SDL_GetTicksNS() - startNs) / SDL_NS_PER_MS);
  return Painting{std::move(canvas), palette};
}
} // namespace Painter

struct TextStyle {
  TTF_Font *fill;
  TTF_Font *outline; // same face and size with TTF_SetFontOutline applied
  int outlinePx;
  SDL_Color color;
  SDL_Color outlineColor;
};

bool sameColor(SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

// One line of text with its dark outline baked in: fill glyphs blended over the stroked glyphs.
SurfacePtr renderOutlinedLine(const TextStyle &style, std::string_view text) {
  SurfacePtr stroke(TTF_RenderText_Blended(style.outline, text.data(), text.size(), style.outlineColor));
  SurfacePtr fill(TTF_RenderText_Blended(style.fill, text.data(), text.size(), style.color));
  if (!stroke || !fill) return {};
  SDL_SetSurfaceBlendMode(fill.get(), SDL_BLENDMODE_BLEND);
  SDL_Rect dst{style.outlinePx, style.outlinePx, fill->w, fill->h};
  if (!SDL_BlitSurface(fill.get(), nullptr, stroke.get(), &dst)) return {};
  return stroke;
}

// Greedy word wrap measured with the fill font. Wrapping is done here rather than by SDL_ttf so that the
// outline and fill renders are guaranteed to break lines at the same words.
std::vector<std::string> wrapWords(TTF_Font *font, std::string_view text, int maxWidth) {
  std::vector<std::string> lines;
  std::string current;
  for (auto word : text | std::views::split(' ')) {
    std::string_view w(word.begin(), word.end());
    if (w.empty()) continue;
    std::string candidate = current.empty() ? std::string(w) : std::format("{} {}", current, w);
    int width = 0;
    if (current.empty() || (TTF_GetStringSize(font, candidate.c_str(), 0, &width, nullptr) && width <= maxWidth)) {
      current = std::move(candidate);
    } else {
      lines.push_back(std::move(current));
      current = std::string(w);
    }
  }
  if (!current.empty()) lines.push_back(std::move(current));
  return lines;
}

SurfacePtr renderOutlinedText(const TextStyle &style, std::string_view text, int wrapWidth) {
  if (wrapWidth <= 0) return renderOutlinedLine(style, text);
  std::vector<SurfacePtr> lines;
  int width = 0;
  for (const auto &line : wrapWords(style.fill, text, wrapWidth)) {
    SurfacePtr surf = renderOutlinedLine(style, line);
    if (!surf) return {};
    width = std::max(width, surf->w);
    lines.push_back(std::move(surf));
  }
  if (lines.empty()) return {};
  if (lines.size() == 1) return std::move(lines.front());
  const int lineSkip = TTF_GetFontLineSkip(style.fill);
  const int height = lineSkip * static_cast<int>(lines.size() - 1) + lines.back()->h;
  SurfacePtr result(SDL_CreateSurface(width, height, lines.front()->format));
  if (!result || !SDL_FillSurfaceRect(result.get(), nullptr, 0)) return {};
  for (size_t i = 0; i < lines.size(); ++i) {
    SDL_SetSurfaceBlendMode(lines[i].get(), SDL_BLENDMODE_BLEND);
    SDL_Rect dst{(width - lines[i]->w) / 2, static_cast<int>(i) * lineSkip, lines[i]->w, lines[i]->h};
    SDL_BlitSurface(lines[i].get(), nullptr, result.get(), &dst);
  }
  return result;
}

class SnowSystem {
public:
  struct Flake {
    float x, y;
    float size;
    float speedY;
    float swayPhase;
    float swaySpeed;
    float depth; // 0.0 (far) to 1.0 (near)
    SDL_FColor color;
  };

  void Init(int width, int height, int count = 200) {
    screenWidth = (float)width;
    screenHeight = (float)height;

    flakes.resize(count);
    vertices.resize(count * 4);
    indices.resize(count * 6);

    std::vector<int> indexPattern = {0, 1, 2, 2, 3, 0};
    for (size_t i = 0; i < flakes.size(); ++i) {
      int vStart = static_cast<int>(i * 4);
      int iStart = static_cast<int>(i * 6);
      for (int k = 0; k < 6; ++k) {
        indices[iStart + k] = vStart + indexPattern[k];
      }
    }
    std::mt19937 gen(std::random_device{}());
    for (auto &f : flakes) {
      ResetFlake(f, gen, true);
    }
  }

  void Update(double dt) {
    windTimer += dt;
    const float slowWind = 20.0f * std::sin((float)windTimer * 0.5f);
    const float gustWind = 10.0f * std::sin((float)windTimer * 2.5f);
    const float currentWind = slowWind + gustWind + 5.0f;
    const float fDt = static_cast<float>(dt);

    std::for_each(std::execution::par_unseq, flakes.begin(), flakes.end(), [&, this](Flake &f) {
      f.y += f.speedY * fDt;
      float individualSway = std::sin((float)windTimer * f.swaySpeed + f.swayPhase) * (10.0f * (1.0f - f.depth));
      f.x += (currentWind * f.depth + individualSway) * fDt;
      if (f.y > screenHeight) {
        f.y = -f.size;
        f.x = std::fmod(f.x + 100.0f, screenWidth);
      }
      if (f.x > screenWidth)
        f.x = -f.size;
      else if (f.x < -f.size)
        f.x = screenWidth;
    });

    auto indicesView = std::views::iota(size_t{0}, flakes.size()) | std::views::common;
    std::for_each(std::execution::par_unseq, indicesView.begin(), indicesView.end(), [this](size_t i) {
      const auto &f = flakes[i];
      size_t vIdx = i * 4;
      const float right = f.x + f.size;
      const float bottom = f.y + f.size;
      vertices[vIdx + 0].position = {f.x, f.y};
      vertices[vIdx + 0].color = f.color;
      vertices[vIdx + 1].position = {right, f.y};
      vertices[vIdx + 1].color = f.color;
      vertices[vIdx + 2].position = {right, bottom};
      vertices[vIdx + 2].color = f.color;
      vertices[vIdx + 3].position = {f.x, bottom};
      vertices[vIdx + 3].color = f.color;
    });
  }

  void Draw(SDL_Renderer *renderer) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
  }

private:
  float screenWidth = 0;
  float screenHeight = 0;
  double windTimer = 0.0;
  std::vector<Flake> flakes;
  std::vector<SDL_Vertex> vertices;
  std::vector<int> indices;
  std::uniform_real_distribution<float> distDepth{0.2f, 1.0f};
  std::uniform_real_distribution<float> distPhase{0.0f, 2.0f * std::numbers::pi_v<float>};

  void ResetFlake(Flake &f, std::mt19937 &gen, bool randomizeY) {
    std::uniform_real_distribution<float> distX(0.0f, screenWidth);
    std::uniform_real_distribution<float> distY(-50.0f, screenHeight);
    f.depth = distDepth(gen);
    f.size = 2.0f + (f.depth * 3.0f);
    f.speedY = 30.0f + (f.depth * 60.0f);
    f.swayPhase = distPhase(gen);
    f.swaySpeed = 1.0f + (f.depth * 2.0f);
    f.x = distX(gen);
    f.y = randomizeY ? distY(gen) : -f.size;
    float alphaVal = 0.2f + (f.depth * 0.8f);
    f.color = {1.0f, 1.0f, 1.0f, alphaVal};
  }
};

class Clock {
public:
  Clock() = default;
  Clock(const Clock &) = delete;
  Clock &operator=(const Clock &) = delete;

  // The worker threads use the other members: stop and join them before anything else is destroyed.
  ~Clock() {
    for (auto *thread : {&bgLoaderThread, &weatherLoaderThread}) {
      thread->request_stop();
      if (thread->joinable()) thread->join();
    }
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
    if (!SDL_SetRenderVSync(renderer.get(), 1)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't enable vsync: %s", SDL_GetError());
    }

    if (!TTF_Init()) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL_ttf: %s", SDL_GetError());
      return false;
    }
    auto openFont = [](int size, int outline) {
      FontPtr font(TTF_OpenFontIO(SDL_IOFromConstMem(BellotaText_Bold_ttf, BellotaText_Bold_ttf_len), true, size));
      if (font && outline > 0 && !TTF_SetFontOutline(font.get(), outline)) font.reset();
      return font;
    };
    fontBig = openFont(Config::font_big_size, 0);
    fontBigOutline = openFont(Config::font_big_size, Config::font_big_outline);
    fontNormal = openFont(Config::font_normal_size, 0);
    fontNormalOutline = openFont(Config::font_normal_size, Config::font_normal_outline);
    fontSmall = openFont(Config::font_small_size, 0);
    fontSmallOutline = openFont(Config::font_small_size, Config::font_small_outline);
    if (!fontNormal || !fontBig || !fontSmall || !fontNormalOutline || !fontBigOutline || !fontSmallOutline) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't load embedded font: %s", SDL_GetError());
      return false;
    }

    if (!SDL_SetRenderLogicalPresentation(renderer.get(), Config::screen_width, Config::screen_height,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't set logical presentation: %s", SDL_GetError());
    }
#ifdef APP_DEBUG
    // Keep cursor visible in debug for easier window movement/closing
#else
    if (!SDL_HideCursor()) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Couldn't hide cursor: %s", SDL_GetError());
    }
#endif

    // snow.Init(Config::screen_width, Config::screen_height, Config::num_snowflakes);

    SetIterateRate(false);

    // SDL fills some global caches (CPU features, pixel format details) lazily on first use, and the CPU feature
    // cache has no lock. Fill them here so worker threads never race the main thread to initialise them.
    SDL_GetSIMDAlignment();
    for (auto format : {SDL_PIXELFORMAT_RGBA32, SDL_PIXELFORMAT_ARGB8888, SDL_PIXELFORMAT_XRGB8888,
                        SDL_PIXELFORMAT_RGB24, SDL_PIXELFORMAT_INDEX8}) {
      SDL_GetPixelFormatDetails(format);
    }

    // Start Data Threads
    bgLoaderThread = std::jthread(&Clock::FetchBackgroundImage, this);
    weatherLoaderThread = std::jthread(&Clock::FetchWeather, this);

    lastPerformanceCounter = SDL_GetPerformanceCounter();

    return true;
  }

  SDL_AppResult Iterate() {
    UpdateTiming();
    // snow.Update(deltaTime);
    UpdateBackground();
    if (UpdateTextures()) needsRedraw = true;
    // Nothing on screen moves between minutes, so a frame is only drawn when something changed. This keeps the
    // Pi's CPU and GPU idle almost all the time instead of redrawing the same picture 60 times a second.
    if (needsRedraw || fading) {
      Render();
      needsRedraw = false;
    }
    return SDL_APP_CONTINUE;
  }

  void HandleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_RENDER_DEVICE_RESET: // every texture is gone; rebuild them from the CPU-side copies
      prevBgTexture.reset();
      bgTexture.reset();
      if (bgSurface) bgTexture.reset(SDL_CreateTextureFromSurface(renderer.get(), bgSurface.get()));
      for (auto *label : AllLabels())
        label->invalidate();
      needsRedraw = true;
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_RENDER_TARGETS_RESET:
      needsRedraw = true;
      break;
    default:
      break;
    }
  }

private:
  WindowPtr window;
  RendererPtr renderer;
  FontPtr fontBig;
  FontPtr fontBigOutline;
  FontPtr fontNormal;
  FontPtr fontNormalOutline;
  FontPtr fontSmall;
  FontPtr fontSmallOutline;

  // SnowSystem snow;

  // Background painting. The worker thread hands over a finished painting through pendingPainting;
  // everything else here is only touched by the main thread.
  std::mutex bgImageLoaderMutex;
  std::optional<Painter::Painting> pendingPainting;
  SurfacePtr bgSurface; // kept to recreate the texture after a render device reset
  TexturePtr bgTexture;
  TexturePtr prevBgTexture; // the painting being faded out
  Uint64 fadeStartNs = 0;
  bool fading = false;
  Painter::Palette palette = Painter::defaultPalette();

  // Weather Data
  std::mutex weatherMutex;
  std::string weatherString;

  // Clothing Advice (LLM)
  std::mutex adviceMutex;
  std::string adviceString;

  Uint64 lastPerformanceCounter = 0;
  double fps = 0.0;
  double deltaTime = 0.0;
  bool needsRedraw = true;
  bool fastIterate = true;

  struct TextLabel {
    std::string text;
    SDL_Color color{};
    int lastWrapWidth = 0;
    TexturePtr texture;
    SDL_FRect rect{}; // where the texture is drawn, outline included
    float pad = 0.0f; // outline thickness around the glyph box

    // Re-renders only when the text, colour or wrap width changed. Returns true when the label changed.
    bool update(SDL_Renderer *renderer, const TextStyle &style, std::string_view newText, int wrapWidth = 0) {
      if (text == newText && sameColor(color, style.color) && wrapWidth == lastWrapWidth && (texture || text.empty()))
        return false;
      text = newText;
      color = style.color;
      lastWrapWidth = wrapWidth;
      texture.reset();
      rect = {};
      pad = static_cast<float>(style.outlinePx);
      if (text.empty()) return true;

      SurfacePtr surf = renderOutlinedText(style, text, wrapWidth);
      if (surf) texture.reset(SDL_CreateTextureFromSurface(renderer, surf.get()));
      if (!texture) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't render text \"%s\": %s", text.c_str(), SDL_GetError());
        return true;
      }
      rect = {0.0f, 0.0f, static_cast<float>(surf->w), static_cast<float>(surf->h)};
      return true;
    }

    void invalidate() {
      text.clear();
      texture.reset();
      rect = {};
    }

    // Size of the text itself, without the outline padding, so layout matches plain text rendering.
    [[nodiscard]] float width() const { return texture ? rect.w - 2.0f * pad : 0.0f; }
    [[nodiscard]] float height() const { return texture ? rect.h - 2.0f * pad : 0.0f; }
    void place(float x, float y) {
      rect.x = x - pad;
      rect.y = y - pad;
    }

    void draw(SDL_Renderer *renderer) const {
      if (texture) SDL_RenderTexture(renderer, texture.get(), nullptr, &rect);
    }
  };

  TextLabel dateLabel;
  TextLabel hoursLabel;
  TextLabel colonLabel;
  TextLabel minutesLabel;
  TextLabel weatherLabel;
  TextLabel adviceLabel;

  std::array<TextLabel *, 6> AllLabels() {
    return {&dateLabel, &hoursLabel, &colonLabel, &minutesLabel, &weatherLabel, &adviceLabel};
  }

  // Declared last: the worker threads use the members above.
  std::jthread bgLoaderThread;
  std::jthread weatherLoaderThread;

  void PublishPainting(Painter::Painting painting) {
    std::lock_guard lock(bgImageLoaderMutex);
    pendingPainting = std::move(painting);
  }

  // SDL keeps per-thread error strings for threads it did not create; they must be freed by the thread itself.
  struct SdlThreadCleanup {
    ~SdlThreadCleanup() { SDL_CleanupTLS(); }
  };

  void FetchBackgroundImage(std::stop_token stopToken) {
    SdlThreadCleanup cleanup;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW); // painting must never steal time from rendering
    std::mt19937 rng(std::random_device{}());
    try {
      if (SurfacePtr canvas = Painter::makeCanvas(Config::screen_width, Config::screen_height, rng)) {
        if (auto painting = Painter::paint(canvas.get(), rng)) PublishPainting(std::move(*painting));
      }
    } catch (const std::exception &e) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Placeholder painting failed: %s", e.what());
    }
#ifdef APP_DEBUG
    // Paint a local photo without network access: CLOCK_TEST_IMAGE=/path/to/photo.jpg
    if (const char *testImage = SDL_getenv("CLOCK_TEST_IMAGE")) {
      if (SurfacePtr photo{IMG_Load(testImage)}) {
        if (auto painting = Painter::paint(photo.get(), rng)) PublishPainting(std::move(*painting));
      }
    }
#endif

    std::string lastLoadedUrl;
    auto retryDelay = std::chrono::minutes(1);
    while (!stopToken.stop_requested()) {
      bool ok = false;
      try {
        cpr::Response response = httpGet(stopToken, cpr::Url{"https://peapix.com/bing/feed?country=us"});
        if (response.status_code == 200) {
          auto response_json = json::parse(response.text);
          const auto &images = response_json.get<std::vector<BingImage>>();
          if (!images.empty()) {
            // TODO: instead of grabbing the first image, grab the image with today's date
            std::string imgUrl = images[0].fullUrl;
            if (imgUrl == lastLoadedUrl) {
              ok = true;
            } else {
              cpr::Response imgResp = httpGet(stopToken, cpr::Url{imgUrl}, cpr::ReserveSize{2 * 1024 * 1024});
              if (imgResp.status_code == 200) {
                SurfacePtr loadedSurf(IMG_Load_IO(SDL_IOFromConstMem(imgResp.text.data(), imgResp.text.size()), true));
                if (!loadedSurf) {
                  SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't decode %s: %s", imgUrl.c_str(), SDL_GetError());
                } else if (auto painting = Painter::paint(loadedSurf.get(), rng)) {
                  PublishPainting(std::move(*painting));
                  lastLoadedUrl = imgUrl;
                  ok = true;
                }
              }
            }
          }
        }
      } catch (const std::exception &e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Background image fetch failed: %s", e.what());
      }
      // The Pi often starts before Wi-Fi is up: retry soon, backing off, instead of waiting hours.
      if (ok) {
        retryDelay = std::chrono::minutes(1);
        sleepFor(stopToken, std::chrono::hours(1));
      } else {
        sleepFor(stopToken, retryDelay);
        retryDelay = std::min(retryDelay * 2, std::chrono::minutes(30));
      }
    }
  }

  // One short sentence, or the offline fallback when the model returns nothing usable.
  static std::string SanitizeAdvice(std::string advice, double temperature) {
    std::ranges::replace(advice, '\n', ' ');
    const auto first = advice.find_first_not_of(" \t\r\"");
    const auto last = advice.find_last_not_of(" \t\r\"");
    advice = first == std::string::npos ? std::string() : advice.substr(first, last - first + 1);
    if (advice.empty() || advice.size() > 240) return getBasicAdvice(temperature);
    return advice;
  }

  void FetchWeather(std::stop_token stopToken) {
    SdlThreadCleanup cleanup;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    const auto url = cpr::Url{"https://api.open-meteo.com/v1/forecast"};
    const auto params = cpr::Parameters{{"latitude", "52.3738"},
                                        {"longitude", "4.8910"},
                                        {"current_weather", "true"},
                                        {"windspeed_unit", "ms"},
                                        {"timezone", "auto"}};

    while (!stopToken.stop_requested()) {
      std::string weatherDescForLLM = "unknown";
      double tempForLLM = 0.0;
      bool weatherFetched = false;
      try {
        cpr::Response response = httpGet(stopToken, url, params);
        if (response.status_code == 200) {
          auto json_data = json::parse(response.text);
          auto wd = json_data.get<WeatherData>();
          std::string_view weatherDesc = "Неизвестно";
          if (auto it = WEATHER_CODE_RU.find(wd.current_weather.weathercode); it != WEATHER_CODE_RU.end()) {
            weatherDesc = it->second;
          }
          weatherDescForLLM = std::string(weatherDesc);
          tempForLLM = wd.current_weather.temperature;
          double ws = wd.current_weather.windspeed;
          std::string windStr(getWindspeedType(ws));
          if (ws >= 1.0) {
            windStr = std::format("{} {:.0f} м/с", windStr, ws);
          }
          std::string result = std::format("{:.0f}°C, {}, {}", wd.current_weather.temperature, weatherDesc, windStr);
          {
            std::scoped_lock lock(weatherMutex);
            weatherString = std::move(result);
            weatherFetched = true;
          }
        }
      } catch (const std::exception &e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Weather fetch failed: %s", e.what());
      }
      if (!weatherFetched) {
        {
          std::scoped_lock lock(weatherMutex);
          weatherString.clear();
        }
        {
          std::scoped_lock lock(adviceMutex);
          adviceString.clear();
        }
      }
      if (weatherFetched) {
        std::string finalAdvice;
        std::string apiKey = Config::GroqApiKey;
        auto useFallback = [&]() { finalAdvice = getBasicAdvice(tempForLLM); };
        if (!apiKey.empty() && apiKey != "MISSING_KEY") {
          try {
            std::string prompt = std::format(
                "I live in Amsterdam. Today is {}, the time is {} and the weather is: {} ({:.0f}C). "
                "What should I wear? Please answer in one short sentence, in russian. "
                "Only say what clothes I should wear, there's no need to mention city, current weather or time and "
                "date. "
                "Basically, just continue the phrase: You should wear..., without saying the 'you should wear' part.",
                getCurrentDate(), getCurrentTime(), weatherDescForLLM, tempForLLM);
            json payload = {
                {"model", "openai/gpt-oss-120b"},
                {"max_tokens", 300},
                {"temperature", 0.7},
                {"messages",
                 {{{"role", "system"}, {"content", "You are a helpful assistant providing concise clothing advice."}},
                  {{"role", "user"}, {"content", prompt}}}}};
            cpr::Response r = cpr::Post(
                cpr::Url{"https://api.groq.com/openai/v1/chat/completions"}, cpr::Body{payload.dump()},
                cpr::Header{{"Authorization", std::string("Bearer ") + apiKey}, {"Content-Type", "application/json"}},
                cpr::ConnectTimeout{Config::http_connect_timeout}, cpr::Timeout{Config::http_timeout},
                abortOnStop(stopToken));
            if (r.status_code == 200) {
              auto llmResp = json::parse(r.text).get<LlmResponse>();
              if (!llmResp.choices.empty()) {
                finalAdvice = SanitizeAdvice(llmResp.choices[0].message.content, tempForLLM);
              } else {
                useFallback();
              }
            } else {
              SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LLM fetch failed code %ld: %s", r.status_code,
                           r.text.c_str());
              useFallback();
            }
          } catch (const std::exception &e) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LLM fetch exception: %s", e.what());
            useFallback();
          }
        } else {
          // No valid key
          useFallback();
        }
        {
          std::lock_guard lock(adviceMutex);
          adviceString = finalAdvice;
        }
      }

      // Fetch weather every 5 minutes; after a failure try again sooner.
      sleepFor(stopToken, weatherFetched ? std::chrono::minutes(5) : std::chrono::minutes(1));
    }
  }

  void SetIterateRate(bool fast) {
    if (fast == fastIterate) return;
    fastIterate = fast;
    // "0" = as fast as vsync allows, used only while a new painting fades in.
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, fast ? "0" : Config::idle_iterate_rate_hz);
  }

  void UpdateTiming() {
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 diff = now - lastPerformanceCounter;
    lastPerformanceCounter = now;
    deltaTime = (double)diff / (double)SDL_GetPerformanceFrequency();
    fps = deltaTime > 0.0 ? 1.0 / deltaTime : 0.0;
  }

  void UpdateBackground() {
    std::optional<Painter::Painting> painting;
    {
      std::lock_guard lock(bgImageLoaderMutex);
      painting.swap(pendingPainting);
    }
    if (painting) {
      TexturePtr texture(SDL_CreateTextureFromSurface(renderer.get(), painting->surface.get()));
      if (!texture) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create background texture: %s", SDL_GetError());
      } else {
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        prevBgTexture = std::move(bgTexture);
        bgTexture = std::move(texture);
        bgSurface = std::move(painting->surface);
        palette = painting->palette; // labels pick up the new colours in UpdateTextures
        fadeStartNs = SDL_GetTicksNS();
        fading = true;
        SetIterateRate(true);
      }
    }
    if (fading && FadeProgress() >= 1.0f) {
      fading = false;
      prevBgTexture.reset();
      SDL_SetTextureBlendMode(bgTexture.get(), SDL_BLENDMODE_NONE); // opaque: cheapest possible full-screen draw
      needsRedraw = true;
      SetIterateRate(false);
    }
  }

  [[nodiscard]] float FadeProgress() const {
    const double elapsed = static_cast<double>(SDL_GetTicksNS() - fadeStartNs) / SDL_NS_PER_SECOND;
    return std::clamp(static_cast<float>(elapsed / Config::background_fade_seconds), 0.0f, 1.0f);
  }

  // Returns true when any label changed and the screen has to be redrawn.
  bool UpdateTextures() {
    auto style = [this](TTF_Font *fill, TTF_Font *outline, int outlinePx, SDL_Color color) {
      return TextStyle{fill, outline, outlinePx, color, palette.outline};
    };
    const TextStyle big = style(fontBig.get(), fontBigOutline.get(), Config::font_big_outline, palette.neutral);
    const TextStyle normal =
        style(fontNormal.get(), fontNormalOutline.get(), Config::font_normal_outline, palette.neutral);
    const TextStyle small = style(fontSmall.get(), fontSmallOutline.get(), Config::font_small_outline, palette.hours);
    TextStyle hours = big, minutes = big;
    hours.color = palette.hours;
    minutes.color = palette.minutes;

    const auto tm = getLocalTime();
    std::string currentW;
    {
      std::lock_guard lock(weatherMutex);
      currentW = weatherString;
    }
    std::string currentAdvice;
    {
      std::lock_guard lock(adviceMutex);
      currentAdvice = adviceString;
    }

    bool changed = false;
    changed |= dateLabel.update(renderer.get(), normal, getCurrentDate());
    changed |= hoursLabel.update(renderer.get(), hours, std::to_string(tm.tm_hour));
    changed |= colonLabel.update(renderer.get(), big, ":");
    changed |= minutesLabel.update(renderer.get(), minutes, std::format("{:02}", tm.tm_min));
    changed |= weatherLabel.update(renderer.get(), normal, currentW);
    changed |= adviceLabel.update(renderer.get(), small, currentAdvice, static_cast<int>(Config::screen_width * 0.95f));
    if (changed) Layout();
    return changed;
  }

  void Layout() {
    constexpr float W = Config::screen_width, H = Config::screen_height;
    dateLabel.place((W - dateLabel.width()) / 2.0f, 60.0f);

    // Hours, colon and minutes are separate textures so they can have their own colours.
    const float timeW = hoursLabel.width() + colonLabel.width() + minutesLabel.width();
    const float timeH = hoursLabel.height();
    const float timeY = (H - timeH) / 2.0f - 20.0f;
    float x = (W - timeW) / 2.0f;
    for (auto *part : {&hoursLabel, &colonLabel, &minutesLabel}) {
      part->place(x, timeY);
      x += part->width();
    }

    const float timeBottom = timeY + timeH;
    // If time texture isn't ready yet, guess a position, otherwise use relative
    const float weatherY = (timeH > 0) ? timeBottom - 80.0f : (H / 2.0f + 140.0f);
    weatherLabel.place((W - weatherLabel.width()) / 2.0f, weatherY);
    const float weatherBottom = weatherY + weatherLabel.height();
    adviceLabel.place((W - adviceLabel.width()) / 2.0f, weatherBottom + 10.0f); // 10px padding
  }

  void Render() {
    SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer.get());

    if (prevBgTexture) RenderTextureCover(prevBgTexture.get());
    if (bgTexture) {
      SDL_SetTextureAlphaModFloat(bgTexture.get(), fading ? FadeProgress() : 1.0f);
      RenderTextureCover(bgTexture.get());
    }
    // snow.Draw(renderer.get());
    for (const auto *label : AllLabels())
      label->draw(renderer.get());

#ifdef APP_DEBUG
    SDL_SetRenderDrawColor(renderer.get(), 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(renderer.get(), 10, 10, "FPS: %.2f", fps);
#endif

    SDL_RenderPresent(renderer.get());
  }

  // Helper to simulate "CSS object-fit: cover"
  void RenderTextureCover(SDL_Texture *texture) {
    float w, h;
    SDL_GetTextureSize(texture, &w, &h);
    float scale = std::max((float)Config::screen_width / w, (float)Config::screen_height / h);
    float newW = w * scale;
    float newH = h * scale;
    SDL_FRect dst = {((float)Config::screen_width - newW) / 2.0f, ((float)Config::screen_height - newH) / 2.0f, newW,
                     newH};
    SDL_RenderTexture(renderer.get(), texture, nullptr, &dst);
  }
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  // libcurl's global init is not thread-safe; do it once before any worker thread starts a request.
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize libcurl");
    return SDL_APP_FAILURE;
  }
  auto *app = new Clock();
  if (!app->Init()) {
    delete app;
    return SDL_APP_FAILURE;
  }
  *appstate = app;
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS;
  }
  static_cast<Clock *>(appstate)->HandleEvent(*event);
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  auto *app = static_cast<Clock *>(appstate);
  return app->Iterate();
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  auto *app = static_cast<Clock *>(appstate);
  delete app; // joins the worker threads
  TTF_Quit();
  curl_global_cleanup();
}
