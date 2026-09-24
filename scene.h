#pragma once

// The animated background: a sky that follows the sun, the sun and moon, drifting clouds, three layers of hills
// with a slow parallax sway, swaying foliage, and weather (rain, snow, fog, lightning).
//
// It is one of eighteen "worlds": six times of day (night, blue hour, dawn, day, golden hour, dusk) for each of
// three kinds of weather (clear, grey, storm). Within a kind the worlds blend smoothly through the day; the
// particles (clouds, rain, snow, fog, lightning) ease in and out on their own. Everything is drawn on the GPU as
// a handful of batched geometry calls over a few small sprites baked at start-up, so a frame stays cheap on a Pi.

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <memory>
#include <random>
#include <vector>

template <typename T, auto Deleter>
using SdlPtr = std::unique_ptr<T, std::integral_constant<decltype(Deleter), Deleter>>;
using SurfacePtr = SdlPtr<SDL_Surface, SDL_DestroySurface>;
using TexturePtr = SdlPtr<SDL_Texture, SDL_DestroyTexture>;

namespace Screen {
constexpr int width = 1024;
constexpr int height = 600;
} // namespace Screen

// ---------------------------------------------------------------- colour --

struct Col {
  float r = 0, g = 0, b = 0, a = 255; // 0..255, kept as float for cheap mixing
};
constexpr Col mix(const Col &a, const Col &b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
inline float smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

struct Palette {
  Col skyTop, skyMid, skyLow, glow, sun; // sky from the top to the horizon; glow carries its strength in alpha
  Col far, mid, nearTop, nearLow, leaf;  // land, back to front
  Col cloudLit, cloudShade;              // cloud tops and bellies
  Col ink, inkDim, accent;               // date and time, which sit on the sky
  Col bars;                              // rain chart and caption, which sit on the land
  bool dark = true;                      // light ink on a dark sky, or dark ink on a light sky
};

constexpr std::array kPaletteColours{&Palette::skyTop,  &Palette::skyMid, &Palette::skyLow,   &Palette::glow,
                                     &Palette::sun,     &Palette::far,    &Palette::mid,      &Palette::nearTop,
                                     &Palette::nearLow, &Palette::leaf,   &Palette::cloudLit, &Palette::cloudShade,
                                     &Palette::ink,     &Palette::inkDim, &Palette::accent,   &Palette::bars};

inline Palette mixPalette(const Palette &a, const Palette &b, float t) {
  Palette r = a;
  for (auto m : kPaletteColours)
    r.*m = mix(a.*m, b.*m, t);
  r.dark = t < 0.5f ? a.dark : b.dark;
  return r;
}

// The text over the land. The foreground hills are dark in every world, so it is always light ink.
namespace Land {
constexpr Col ink{244, 246, 250};
constexpr Col inkDim{204, 210, 222};
constexpr Col inkMute{184, 192, 206};
constexpr Col hair{255, 255, 255, 40};
} // namespace Land

// Where the layers sit on screen (logical pixels). The date and time sit on the sky above the hills, and the
// weather strip and rain chart on the dark foreground below them.
namespace Layout {
constexpr float skyMidY = 250.0f, horizonY = 410.0f;   // sky gradient stops (skyTop is at 0)
constexpr float farTop = 272.0f, farBottom = 350.0f;   // ridge-line ranges of each layer
constexpr float midTop = 362.0f, midBottom = 382.0f;   // below the feet of the digits
constexpr float nearTop = 382.0f, nearBottom = 396.0f; // above the weather strip
constexpr float foreTop = 474.0f, foreBottom = 512.0f; // a darker swell in front, behind the rain chart
} // namespace Layout

inline Col skyAt(const Palette &p, float y) {
  if (y <= Layout::skyMidY) return mix(p.skyTop, p.skyMid, y / Layout::skyMidY);
  const float t = std::min(1.0f, (y - Layout::skyMidY) / (Layout::horizonY - Layout::skyMidY));
  return mix(p.skyMid, p.skyLow, t * t); // the horizon colour gathers low
}
inline Col nearAt(const Palette &p, float y) {
  return mix(p.nearTop, p.nearLow, std::clamp((y - Layout::nearTop) / (Screen::height - Layout::nearTop), 0.0f, 1.0f));
}

// ---------------------------------------------------------------- worlds --

struct DayPalettes {
  Palette night, blueHour, dawn, day, golden, dusk;
};

// Each palette is designed for contrast on its own (checked for every minute of the day in debug builds), and the
// day only ever blends between two palettes of the same polarity: blending a light sky into a dark one would pass
// through a moment where the sky and the ink are the same grey. The one flip per sunrise and sunset is a short
// fade in the renderer.
//
// Field order: skyTop skyMid skyLow glow sun / far mid nearTop nearLow leaf / cloudLit cloudShade /
//              ink inkDim accent / bars / dark
namespace Worlds {
// clang-format off
constexpr DayPalettes Clear{
    // night: deep indigo, silver moon
    {{7, 11, 32}, {16, 26, 64}, {50, 64, 128}, {120, 140, 255, 26}, {255, 236, 200},
     {28, 34, 80}, {18, 24, 56}, {11, 15, 36}, {5, 7, 18}, {3, 5, 14},
     {54, 62, 108}, {28, 34, 70},
     {240, 242, 252}, {172, 180, 216}, {196, 168, 255},
     {160, 170, 255}, true},
    // blue hour: indigo to rose at the horizon
    {{18, 22, 64}, {58, 48, 112}, {186, 106, 138}, {255, 150, 180, 34}, {255, 200, 160},
     {56, 46, 104}, {32, 28, 70}, {19, 18, 46}, {9, 9, 24}, {6, 6, 18},
     {86, 64, 112}, {50, 42, 92},
     {244, 240, 252}, {194, 188, 224}, {255, 172, 146},
     {255, 176, 168}, true},
    // dawn: peach and rose, violet hills
    {{246, 192, 198}, {255, 214, 180}, {255, 178, 124}, {255, 244, 214, 90}, {255, 206, 120},
     {206, 138, 162}, {128, 72, 108}, {56, 30, 58}, {24, 13, 32}, {22, 10, 28},
     {255, 238, 232}, {228, 172, 184},
     {34, 20, 36}, {96, 62, 88}, {176, 60, 28},
     {255, 176, 140}, false},
    // day: azure sky, green hills, deep teal foreground
    {{104, 178, 240}, {168, 212, 246}, {222, 240, 250}, {255, 255, 255, 80}, {255, 240, 160},
     {132, 176, 214}, {84, 160, 96}, {20, 64, 58}, {8, 30, 38}, {10, 42, 34},
     {255, 255, 255}, {206, 224, 242},
     {14, 22, 34}, {40, 60, 90}, {136, 76, 0},
     {130, 206, 255}, false},
    // golden hour: apricot sky, coral mountains
    {{248, 162, 122}, {255, 196, 132}, {255, 224, 150}, {255, 232, 170, 90}, {255, 224, 124},
     {222, 124, 108}, {150, 62, 64}, {66, 22, 34}, {28, 9, 20}, {24, 7, 14},
     {255, 214, 176}, {232, 146, 124},
     {40, 18, 12}, {100, 46, 30}, {160, 46, 14},
     {255, 164, 128}, false},
    // dusk: violet with an ember horizon
    {{24, 18, 58}, {70, 34, 92}, {178, 80, 92}, {255, 120, 90, 40}, {255, 170, 110},
     {64, 32, 80}, {38, 20, 54}, {22, 12, 36}, {10, 6, 18}, {8, 4, 14},
     {104, 56, 96}, {58, 34, 80},
     {246, 238, 244}, {202, 182, 204}, {255, 152, 96},
     {255, 160, 140}, true},
};

constexpr DayPalettes Grey{
    // night: navy rain
    {{12, 20, 42}, {24, 38, 70}, {42, 60, 96}, {120, 160, 220, 18}, {220, 230, 255},
     {30, 44, 76}, {19, 31, 56}, {11, 19, 37}, {5, 9, 19}, {4, 7, 15},
     {52, 64, 94}, {28, 38, 62},
     {236, 242, 250}, {162, 178, 204}, {110, 190, 255},
     {110, 196, 255}, true},
    // blue hour
    {{22, 28, 56}, {50, 56, 94}, {96, 92, 128}, {160, 170, 230, 22}, {230, 220, 255},
     {46, 52, 88}, {30, 34, 62}, {17, 21, 41}, {8, 10, 22}, {6, 8, 18},
     {64, 68, 100}, {42, 46, 78},
     {240, 240, 250}, {180, 182, 212}, {150, 190, 255},
     {146, 186, 255}, true},
    // dawn: blue-grey with a faint warm break
    {{152, 178, 208}, {186, 204, 224}, {220, 214, 210}, {255, 226, 190, 60}, {255, 214, 160},
     {120, 146, 184}, {70, 98, 138}, {22, 38, 60}, {9, 17, 30}, {8, 15, 26},
     {226, 234, 242}, {156, 172, 196},
     {12, 20, 34}, {36, 52, 76}, {26, 88, 160},
     {130, 206, 255}, false},
    // day: overcast
    {{186, 196, 206}, {214, 220, 226}, {230, 232, 232}, {255, 255, 255, 50}, {255, 250, 230},
     {150, 164, 178}, {92, 116, 108}, {30, 44, 46}, {12, 19, 23}, {9, 17, 17},
     {242, 244, 246}, {168, 178, 190},
     {14, 20, 28}, {52, 62, 76}, {62, 80, 108},
     {170, 204, 236}, false},
    // golden hour: muted, warm grey
    {{190, 170, 162}, {226, 198, 172}, {240, 212, 172}, {255, 212, 164, 60}, {255, 212, 150},
     {170, 130, 122}, {110, 74, 76}, {48, 28, 34}, {20, 11, 16}, {16, 9, 12},
     {242, 224, 210}, {184, 154, 148},
     {28, 16, 12}, {72, 46, 38}, {128, 50, 20},
     {255, 176, 146}, false},
    // dusk
    {{28, 26, 52}, {62, 52, 86}, {120, 84, 110}, {255, 140, 120, 24}, {255, 180, 140},
     {54, 44, 76}, {36, 30, 56}, {21, 17, 37}, {10, 8, 20}, {8, 6, 16},
     {80, 68, 98}, {48, 40, 72},
     {244, 240, 248}, {190, 182, 206}, {255, 160, 120},
     {200, 176, 255}, true},
};

// Storms are dark at every hour: purple, bruised skies with light ink.
constexpr DayPalettes Storm{
    // night
    {{9, 9, 26}, {21, 21, 46}, {38, 34, 66}, {140, 120, 255, 20}, {220, 220, 255},
     {29, 26, 56}, {18, 16, 38}, {11, 10, 27}, {5, 5, 14}, {4, 4, 10},
     {48, 46, 80}, {24, 22, 46},
     {238, 238, 252}, {172, 172, 206}, {170, 150, 255},
     {170, 160, 255}, true},
    // blue hour
    {{20, 18, 44}, {44, 34, 78}, {82, 58, 104}, {200, 140, 255, 26}, {230, 200, 255},
     {44, 34, 76}, {28, 22, 54}, {17, 13, 35}, {8, 6, 18}, {6, 4, 14},
     {72, 58, 104}, {38, 30, 68},
     {242, 238, 252}, {186, 178, 216}, {192, 162, 255},
     {180, 160, 255}, true},
    // dawn
    {{38, 34, 74}, {82, 62, 108}, {150, 96, 122}, {255, 160, 170, 30}, {255, 190, 160},
     {70, 54, 100}, {42, 34, 70}, {23, 19, 43}, {10, 8, 22}, {8, 6, 18},
     {84, 68, 106}, {56, 46, 90},
     {244, 240, 250}, {198, 188, 216}, {255, 172, 150},
     {190, 176, 255}, true},
    // day: slate
    {{40, 46, 68}, {66, 74, 98}, {104, 110, 130}, {200, 210, 255, 20}, {240, 240, 255},
     {60, 68, 92}, {38, 48, 66}, {21, 29, 41}, {9, 13, 19}, {7, 11, 15},
     {72, 80, 102}, {48, 54, 76},
     {240, 244, 250}, {188, 196, 212}, {250, 212, 110},
     {150, 196, 255}, true},
    // golden hour: plum sky, magenta horizon
    {{48, 24, 78}, {104, 38, 98}, {190, 70, 92}, {255, 110, 120, 40}, {255, 170, 120},
     {88, 40, 98}, {52, 24, 68}, {29, 13, 43}, {13, 6, 21}, {10, 4, 16},
     {104, 54, 108}, {62, 32, 88},
     {248, 240, 248}, {212, 192, 216}, {255, 172, 112},
     {206, 160, 255}, true},
    // dusk
    {{28, 17, 54}, {66, 28, 82}, {128, 48, 86}, {255, 100, 120, 30}, {255, 150, 120},
     {60, 28, 80}, {37, 18, 55}, {21, 11, 37}, {10, 5, 18}, {8, 4, 14},
     {98, 54, 108}, {48, 27, 72},
     {246, 238, 248}, {202, 184, 212}, {255, 152, 110},
     {196, 156, 255}, true},
};
// clang-format on
} // namespace Worlds

struct SunTimes {
  std::time_t sunrise = 0, sunset = 0;
};

// Palette for `now`, keyed to the real sunrise and sunset. Blends are smooth within a polarity; the flip at
// sunrise and sunset is handled by a short fade in the renderer.
inline Palette skyPalette(std::time_t now, SunTimes sun, const DayPalettes &w) {
  constexpr std::time_t min = 60;
  struct Key {
    std::time_t t;
    const Palette *p;
  };
  const std::array<Key, 8> keys{{
      {sun.sunrise - 75 * min, &w.night},
      {sun.sunrise - 20 * min, &w.blueHour},
      {sun.sunrise, &w.dawn},
      {sun.sunrise + 90 * min, &w.day},
      {sun.sunset - 100 * min, &w.day},
      {sun.sunset - 30 * min, &w.golden},
      {sun.sunset, &w.dusk},
      {sun.sunset + 80 * min, &w.night},
  }};
  if (now < keys.front().t || now >= keys.back().t) return w.night;
  for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
    const Key &a = keys[i], &b = keys[i + 1];
    if (now >= b.t) continue;
    if (a.p->dark != b.p->dark) return *a.p; // hold until the flip at sunrise / sunset
    const float t = (float)(now - a.t) / (float)std::max<std::time_t>(1, b.t - a.t);
    return mixPalette(*a.p, *b.p, t);
  }
  return w.night;
}

// How dark it is outside: 0 while the sun is up, 1 from about an hour after sunset to an hour before sunrise.
inline float nightness(std::time_t now, SunTimes sun) {
  if (now >= sun.sunrise && now < sun.sunset) return 0.0f;
  const float minutes = (float)(now < sun.sunrise ? sun.sunrise - now : now - sun.sunset) / 60.0f;
  return smooth01((minutes - 5.0f) / 50.0f);
}

// --------------------------------------------------------------- weather --

enum class WeatherKind { Clear, Grey, Storm };

inline const DayPalettes &worldsFor(WeatherKind k) {
  switch (k) {
  case WeatherKind::Grey:
    return Worlds::Grey;
  case WeatherKind::Storm:
    return Worlds::Storm;
  default:
    return Worlds::Clear;
  }
}

// What the scene shows, each 0..1 except wind (m/s). The scene eases towards these, so a new forecast fades in.
struct Conditions {
  float clouds = 0.2f, rain = 0, snow = 0, fog = 0, storm = 0, wind = 3;
};

struct WeatherLook {
  WeatherKind kind = WeatherKind::Clear;
  Conditions c;
};

// `code` is the WMO weather code (-1 when there is no forecast yet), `rainNow` the precipitation expected in the
// current 15 minutes (mm).
inline WeatherLook weatherLook(int code, float wind, float rainNow) {
  WeatherLook l;
  l.c.wind = wind;
  auto grey = [&](float clouds) {
    l.kind = WeatherKind::Grey;
    l.c.clouds = clouds;
  };
  switch (code) {
  case 0:
    l.c.clouds = 0.08f;
    break;
  case 1:
    l.c.clouds = 0.28f;
    break;
  case 2:
    l.c.clouds = 0.55f;
    break;
  case 3:
    grey(1.0f);
    break;
  case 45:
  case 48:
    grey(0.7f);
    l.c.fog = 1.0f;
    break;
  case 51:
  case 53:
  case 55:
  case 56:
  case 57:
    grey(0.9f);
    l.c.rain = 0.15f + 0.1f * (float)((code - 51) % 5 / 2);
    l.c.fog = 0.3f;
    break;
  case 61:
  case 66:
  case 80:
    grey(0.95f);
    l.c.rain = 0.45f;
    break;
  case 63:
  case 81:
    grey(1.0f);
    l.c.rain = 0.7f;
    break;
  case 65:
  case 67:
  case 82:
    grey(1.0f);
    l.c.rain = 1.0f;
    break;
  case 71:
  case 77:
    grey(0.95f);
    l.c.snow = 0.4f;
    break;
  case 73:
  case 85:
    grey(1.0f);
    l.c.snow = 0.7f;
    break;
  case 75:
  case 86:
    grey(1.0f);
    l.c.snow = 1.0f;
    break;
  case 95:
  case 96:
  case 99:
    l.kind = WeatherKind::Storm;
    l.c.clouds = 1.0f;
    l.c.rain = 0.9f;
    l.c.storm = 1.0f;
    break;
  default:
    break; // no forecast yet: a fair sky
  }
  // The 15-minute forecast knows about a shower before the current weather code catches up.
  if (rainNow > 0.05f && l.c.snow == 0.0f) {
    l.c.rain = std::max(l.c.rain, std::clamp(rainNow / 1.5f, 0.2f, 1.0f));
    l.c.clouds = std::max(l.c.clouds, 0.8f);
  }
  return l;
}

// ----------------------------------------------------------------- scene --

class Scene {
public:
  bool Init(SDL_Renderer *r) {
    std::mt19937 rng(20260924u); // fixed seed: the same landscape on every start
    std::uniform_real_distribution<float> U(0.0f, 1.0f);

    dot = makeSprite(r, 16, 16, [](float x, float y) {
      const float d = std::hypot(x - 8.0f, y - 8.0f) / 8.0f;
      return smooth01(1.0f - d) * smooth01(1.0f - d);
    });
    glow = makeSprite(r, 128, 128, [](float x, float y) {
      const float d = std::hypot(x - 64.0f, y - 64.0f) / 64.0f;
      return std::pow(smooth01(1.0f - d), 1.6f);
    });
    disc = makeSprite(r, 64, 64, [](float x, float y) {
      return std::clamp(30.0f - std::hypot(x - 32.0f, y - 32.0f) + 0.5f, 0.0f, 1.0f);
    });
    crescent = makeSprite(r, 64, 64, [](float x, float y) {
      const float outer = std::clamp(30.0f - std::hypot(x - 32.0f, y - 32.0f) + 0.5f, 0.0f, 1.0f);
      const float bite = std::clamp(26.0f - std::hypot(x - 45.0f, y - 24.0f) + 0.5f, 0.0f, 1.0f);
      return outer * (1.0f - bite);
    });
    for (std::size_t i = 0; i < cloudTex.size(); ++i)
      cloudTex[i] = makeCloud(r, rng);
    canvas = makeCanvas(r, Screen::width / 2, Screen::height / 2);
    if (!dot || !glow || !disc || !crescent || !canvas || !cloudTex[0] || !cloudTex[1] || !cloudTex[2]) return false;

    ridges[0] = makeRidge(rng, Layout::farTop, Layout::farBottom, true);
    ridges[1] = makeRidge(rng, Layout::midTop, Layout::midBottom, false);
    ridges[2] = makeRidge(rng, Layout::nearTop, Layout::nearBottom, false);
    ridges[3] = makeRidge(rng, Layout::foreTop, Layout::foreBottom, false);

    for (int i = 0; i < kStars; ++i) {
      stars.push_back({U(rng) * Screen::width, std::pow(U(rng), 1.4f) * 330.0f, 3.0f + U(rng) * U(rng) * 5.0f,
                       0.45f + U(rng) * 0.55f, 0.6f + U(rng) * 2.2f, U(rng) * 6.28f});
    }
    for (int i = 0; i < kClouds; ++i) {
      const float depth = (float)i / kClouds; // small, slow and high first; big, faster and lower later
      clouds.push_back({U(rng), 30.0f + U(rng) * 150.0f + depth * 90.0f, 0.55f + depth * 0.7f + U(rng) * 0.25f,
                        4.0f + depth * 7.0f + U(rng) * 3.0f, 0.75f + U(rng) * 0.25f, (int)(U(rng) * 3) % 3});
    }
    for (int i = 0; i < kLights; ++i) {
      lights.push_back({U(rng) * (Screen::width + 2 * kMargin) - kMargin, 3.0f + U(rng) * 26.0f, 1.2f + U(rng) * 1.6f,
                        0.5f + U(rng) * 2.0f, U(rng) * 6.28f});
    }
    for (int i = 0; i < kDrops; ++i) {
      const bool nearDrop = U(rng) < 0.35f;
      drops.push_back({U(rng), U(rng), (nearDrop ? 1050.0f : 760.0f) + U(rng) * 250.0f,
                       (nearDrop ? 26.0f : 14.0f) + U(rng) * 12.0f, nearDrop});
    }
    for (int i = 0; i < kFlakes; ++i) {
      flakes.push_back({U(rng), U(rng), 22.0f + U(rng) * 38.0f, 2.5f + U(rng) * 3.5f, 0.4f + U(rng) * 0.9f,
                        U(rng) * 6.28f, 6.0f + U(rng) * 14.0f});
    }
    // Two fronds of long leaves rising from the bottom corners, leaning away from the text.
    for (int side = 0; side < 2; ++side) {
      for (int i = 0; i < 7; ++i) {
        Leaf l;
        const float t = (float)i / 6.0f;
        l.bx = -12.0f + U(rng) * 48.0f;
        l.by = 404.0f + U(rng) * 26.0f; // above the temperature icon
        l.len = 100.0f + (1.0f - std::abs(t - 0.5f) * 2.0f) * 90.0f + U(rng) * 30.0f;
        l.width = 16.0f + U(rng) * 10.0f;
        l.angle = 1.0f + t * 0.85f + (U(rng) - 0.5f) * 0.12f; // radians above the horizontal
        l.bend = -0.12f - U(rng) * 0.2f;                      // curl back, away from the digits
        l.freq = 0.55f + U(rng) * 0.5f;
        l.phase = U(rng) * 6.28f;
        if (side == 1) {
          l.bx = Screen::width - l.bx;
          l.angle = (float)M_PI - l.angle;
          l.bend = -l.bend;
        }
        leaves.push_back(l);
      }
    }
    return true;
  }

  // `secs` is a monotonic clock that drives the motion; `now` and `sun` place the sun, moon and stars.
  void Draw(SDL_Renderer *r, const Palette &p, const Conditions &target, std::time_t now, SunTimes sun, double secs) {
    const float dt = haveCur ? (float)std::clamp(secs - lastSecs, 0.0, 0.5) : 0.0f;
    lastSecs = secs;
    Ease(target, dt);
    const Conditions &c = cur;
    const float W = Screen::width, H = Screen::height;
    const float night = nightness(now, sun);

    // Parallax: the hills breathe sideways, each by an amount proportional to its nearness.
    const float sway = (float)std::sin(secs * 2.0 * M_PI / 150.0);
    const float dx[4] = {3.0f * sway, 8.0f * sway, 15.0f * sway, 24.0f * sway};

    // --- sky ---
    for (int i = 0; i <= 24; ++i) {
      const float y = H * i / 24.0f;
      const SDL_FColor col = fc(skyAt(p, y));
      b.add(0, y, col);
      b.add(W, y, col);
      if (i > 0) b.quad(2 * i - 2, 2 * i - 1, 2 * i + 1, 2 * i);
    }
    b.flush(r, nullptr);
    b.sprite(W * 0.5f, H * 0.12f, W * 1.44f, H * 0.96f, fc(p.glow, p.glow.a / 255.0f));
    b.flush(r, glow.get());

    // --- stars and the odd shooting star ---
    const float starVis = night * std::clamp(1.0f - c.clouds * 1.25f - c.fog, 0.0f, 1.0f);
    if (starVis > 0.01f) {
      for (const Star &s : stars) {
        const float tw = 0.55f + 0.45f * (float)std::sin(secs * s.freq + s.phase);
        b.sprite(s.x, s.y, s.size, s.size, fc({255, 250, 240}, starVis * s.bright * tw));
      }
      b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
    }
    DrawMeteor(r, secs, starVis);

    // --- sun and moon, behind the clouds and hills ---
    const double dayLen = (double)std::max<std::time_t>(1, sun.sunset - sun.sunrise);
    const float frac = (float)((double)(now - sun.sunrise) / dayLen);
    if (frac > -0.05f && frac < 1.05f) {
      const float x = 70.0f + frac * (W - 140.0f);
      const float y = 432.0f - 364.0f * std::sin((float)M_PI * std::clamp(frac, 0.0f, 1.0f));
      const float vis = std::clamp(1.0f - 0.92f * c.clouds - 0.5f * c.fog, 0.0f, 1.0f);
      b.sprite(x, y, 330.0f, 330.0f, fc(p.sun, 0.55f * vis));
      b.flush(r, glow.get(), SDL_BLENDMODE_ADD);
      b.sprite(x, y, 60.0f, 60.0f, fc(p.sun, vis));
      b.flush(r, disc.get());
    }
    const float moonVis = night * std::clamp(1.0f - 0.95f * c.clouds - 0.5f * c.fog, 0.0f, 1.0f);
    if (moonVis > 0.01f) {
      b.sprite(916.0f, 92.0f, 240.0f, 240.0f, fc({190, 200, 255}, 0.3f * moonVis));
      b.flush(r, glow.get(), SDL_BLENDMODE_ADD);
      b.sprite(916.0f, 92.0f, 58.0f, 58.0f, fc({250, 244, 226}, moonVis));
      b.flush(r, crescent.get());
    }

    // --- clouds, drifting with the wind ---
    const float drift = 0.5f + c.wind * 0.12f;
    for (std::size_t i = 0; i < clouds.size(); ++i) {
      Cloud &cl = clouds[i];
      cl.x += cl.speed * drift * dt / (W + 700.0f);
      cl.x -= std::floor(cl.x);
      const float vis = std::clamp(c.clouds * kClouds * 1.1f - (float)i, 0.0f, 1.0f);
      if (vis <= 0.0f) continue;
      const float w = 340.0f * cl.scale * (0.85f + 0.45f * c.clouds), h = w * 0.4f;
      const float x = cl.x * (W + w + 200.0f) - w * 0.5f - 100.0f;
      const float a = vis * cl.opacity * (0.7f + 0.3f * c.clouds);
      b.spriteV(x, cl.y, w, h, fc(p.cloudLit, a), fc(p.cloudShade, a));
      b.flush(r, cloudTex[cl.variant].get()); // in order: later clouds are nearer
    }

    DrawLightning(r, secs, c, p);

    // --- land: far mountains, mid hills with the town, near hills and a swell in front ---
    Ridge(ridges[0], dx[0], p.far, mix(p.far, p.skyLow, 0.35f), Layout::horizonY + 20.0f);
    b.flush(r, nullptr);
    const float fogVis = std::clamp(c.fog * 0.65f + c.rain * 0.12f, 0.0f, 1.0f);
    Fog(r, secs, 330.0f, fogVis, mix(p.skyLow, p.cloudLit, 0.5f), 5.0f);
    Ridge(ridges[1], dx[1], p.mid, mix(p.mid, p.nearTop, 0.55f), Layout::horizonY + 40.0f);
    b.flush(r, nullptr);
    DrawLights(r, secs, night * (1.0f - 0.5f * c.fog), dx[1]);
    Fog(r, secs, 372.0f, fogVis, mix(p.skyLow, p.cloudLit, 0.35f), 9.0f);
    Ridge(ridges[2], dx[2], p.nearTop, p.nearLow, H);
    Ridge(ridges[3], dx[3], mix(p.nearTop, p.nearLow, 0.5f), p.nearLow, H);
    b.flush(r, nullptr);

    DrawLeaves(r, secs, c, p);
    DrawRain(r, secs, c, p);
    DrawSnow(r, secs, c);

    // Painted-canvas brush strokes over everything but the text.
    const SDL_FRect full{0, 0, W, H};
    SDL_RenderTexture(r, canvas.get(), nullptr, &full);
  }

private:
  static constexpr int kStars = 120, kClouds = 16, kLights = 36, kDrops = 280, kFlakes = 170;
  static constexpr int kMargin = 64, kStep = 8; // ridge lines extend past both edges for the parallax
  static constexpr float kEaseSecs = 6.0f;

  struct Star {
    float x, y, size, bright, freq, phase;
  };
  struct Cloud {
    float x, y, scale, speed, opacity; // x: 0..1 across the wrap-around track
    int variant;
  };
  struct Light {
    float x, depth, size, freq, phase; // depth: pixels below the mid ridge
  };
  struct Drop {
    float x0, y0, speed, len;
    bool nearDrop;
  };
  struct Flake {
    float x0, y0, speed, size, freq, phase, amp;
  };
  struct Leaf {
    float bx, by, len, width, angle, bend, freq, phase;
  };

  // Vertices and indices for one SDL_RenderGeometry call.
  struct Batch {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;

    int add(float x, float y, SDL_FColor c, float u = 0, float w = 0) {
      v.push_back({{x, y}, c, {u, w}});
      return (int)v.size() - 1;
    }
    void quad(int a, int b, int c, int d) { idx.insert(idx.end(), {a, b, c, a, c, d}); } // a b c d around
    // A textured quad centred on (cx, cy), with a top and a bottom colour.
    void spriteV(float cx, float cy, float w, float h, SDL_FColor top, SDL_FColor bottom) {
      const float x = cx - w * 0.5f, y = cy - h * 0.5f;
      const int a = add(x, y, top, 0, 0), bb = add(x + w, y, top, 1, 0);
      quad(a, bb, add(x + w, y + h, bottom, 1, 1), add(x, y + h, bottom, 0, 1));
    }
    void sprite(float cx, float cy, float w, float h, SDL_FColor c) { spriteV(cx, cy, w, h, c, c); }
    // A line segment of width `w` fading from colour `a` at p0 to `bcol` at p1.
    void segment(float x0, float y0, float x1, float y1, float w, SDL_FColor a, SDL_FColor bcol) {
      const float len = std::max(0.001f, std::hypot(x1 - x0, y1 - y0));
      const float nx = -(y1 - y0) / len * w * 0.5f, ny = (x1 - x0) / len * w * 0.5f;
      const int i0 = add(x0 + nx, y0 + ny, a), i1 = add(x0 - nx, y0 - ny, a);
      quad(i0, i1, add(x1 - nx, y1 - ny, bcol), add(x1 + nx, y1 + ny, bcol));
    }
    void flush(SDL_Renderer *r, SDL_Texture *t, SDL_BlendMode mode = SDL_BLENDMODE_BLEND) {
      if (!idx.empty()) {
        if (t)
          SDL_SetTextureBlendMode(t, mode);
        else
          SDL_SetRenderDrawBlendMode(r, mode);
        SDL_RenderGeometry(r, t, v.data(), (int)v.size(), idx.data(), (int)idx.size());
        if (!t) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
      }
      v.clear();
      idx.clear();
    }
  };

  static SDL_FColor fc(Col c, float alpha = 1.0f) {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, std::clamp(alpha, 0.0f, 1.0f)};
  }

  TexturePtr dot, glow, disc, crescent, canvas;
  std::array<TexturePtr, 3> cloudTex;
  std::array<std::vector<float>, 4> ridges; // ridge-line y every kStep px, starting at -kMargin
  std::vector<Star> stars;
  std::vector<Cloud> clouds;
  std::vector<Light> lights;
  std::vector<Drop> drops;
  std::vector<Flake> flakes;
  std::vector<Leaf> leaves;
  Batch b;

  Conditions cur;
  bool haveCur = false;
  double lastSecs = 0;
  std::mt19937 events{7u}; // lightning and shooting stars
  double nextStrike = 0, strikeAt = -100, nextMeteor = 20, meteorAt = -100;
  unsigned strikeSeed = 0;
  float meteorX = 0, meteorY = 0, meteorDir = 0;

  // ------------------------------------------------------------ sprites --
  template <typename F> static TexturePtr makeSprite(SDL_Renderer *r, int w, int h, F alphaAt) {
    SurfacePtr s(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32));
    if (!s) return {};
    for (int y = 0; y < h; ++y) {
      auto *row = (Uint8 *)s->pixels + (std::size_t)y * s->pitch;
      for (int x = 0; x < w; ++x) {
        row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
        row[x * 4 + 3] = (Uint8)(std::clamp(alphaAt(x + 0.5f, y + 0.5f), 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    TexturePtr t(SDL_CreateTextureFromSurface(r, s.get()));
    if (t) SDL_SetTextureScaleMode(t.get(), SDL_SCALEMODE_LINEAR);
    return t;
  }

  // A cumulus: a row of soft puffs, taller in the middle, with a flattened base.
  static TexturePtr makeCloud(SDL_Renderer *r, std::mt19937 &rng) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    struct Puff {
      float x, y, rad;
    };
    std::vector<Puff> puffs;
    const int n = 6 + (int)(U(rng) * 4);
    for (int i = 0; i < n; ++i) {
      const float t = (i + 0.5f) / n;
      const float rad = 14.0f + 22.0f * std::sin((float)M_PI * t) + U(rng) * 8.0f;
      puffs.push_back({28.0f + t * 200.0f + (U(rng) - 0.5f) * 14.0f, 80.0f - rad * 0.9f, rad});
    }
    return makeSprite(r, 256, 102, [&](float x, float y) {
      float a = 0.0f;
      for (const Puff &p : puffs) {
        const float d = std::hypot(x - p.x, y - p.y);
        a = std::max(a, smooth01((p.rad - d) / 10.0f + 0.5f));
      }
      return a * smooth01((86.0f - y) / 12.0f);
    });
  }

  // Brush strokes, light and dark, mostly horizontal sweeps with bristle streaks. Neutral, so the same strokes
  // brush every palette.
  static TexturePtr makeCanvas(SDL_Renderer *r, int w, int h) {
    std::vector<float> paint((std::size_t)w * h, 0.0f); // -1 dark stroke .. +1 light stroke
    std::mt19937 rng(20260924u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const int strokes = w * h / 220;
    for (int i = 0; i < strokes; ++i) {
      const float cx = unit(rng) * w, cy = unit(rng) * h;
      const float halfLen = 18.0f + unit(rng) * 55.0f, halfWid = 2.5f + unit(rng) * 5.5f;
      const float angle = (unit(rng) - 0.5f) * 0.35f + 0.12f * std::sin(cy * 0.03f);
      const float value = (unit(rng) - 0.5f) * 2.0f;
      const float phase = unit(rng) * 6.28f;
      const float c = std::cos(angle), sn = std::sin(angle);
      const float ex = std::abs(c) * halfLen + std::abs(sn) * halfWid,
                  ey = std::abs(sn) * halfLen + std::abs(c) * halfWid;
      const int x0 = std::max(0, (int)(cx - ex)), x1 = std::min(w - 1, (int)(cx + ex) + 1);
      const int y0 = std::max(0, (int)(cy - ey)), y1 = std::min(h - 1, (int)(cy + ey) + 1);
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const float dx = x - cx, dy = y - cy;
          const float u = (dx * c + dy * sn) / halfLen, v = (dy * c - dx * sn) / halfWid;
          const float d = u * u + v * v;
          if (d >= 1.0f) continue;
          const float bristle = 0.6f + 0.4f * std::sin(v * 9.0f + phase);
          const float alpha = 0.55f * std::min(1.0f, (1.0f - d) * 3.0f) * bristle;
          float &px = paint[(std::size_t)y * w + x];
          px += (value - px) * alpha;
        }
      }
    }
    SurfacePtr s(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32));
    if (!s) return {};
    constexpr float strength = 16.0f; // alpha of a full-strength stroke
    for (int y = 0; y < h; ++y) {
      auto *row = (Uint8 *)s->pixels + (std::size_t)y * s->pitch;
      for (int x = 0; x < w; ++x) {
        const float v = paint[(std::size_t)y * w + x];
        row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = v > 0 ? 255 : 0;
        row[x * 4 + 3] = (Uint8)(std::abs(v) * strength);
      }
    }
    TexturePtr t(SDL_CreateTextureFromSurface(r, s.get()));
    if (t) SDL_SetTextureScaleMode(t.get(), SDL_SCALEMODE_LINEAR);
    return t;
  }

  // A ridge line between `top` and `bottom`: smooth value noise, with folded ("ridged") octaves for mountains.
  static std::vector<float> makeRidge(std::mt19937 &rng, float top, float bottom, bool peaky) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int n = (Screen::width + 2 * kMargin) / kStep + 1;
    std::vector<float> h(n, 0.0f);
    const float periods[4] = {430.0f, 190.0f, 80.0f, 34.0f};
    const float amps[4] = {1.0f, 0.45f, peaky ? 0.2f : 0.08f, peaky ? 0.08f : 0.02f};
    for (int o = 0; o < 4; ++o) {
      std::vector<float> lattice((std::size_t)(n * kStep / periods[o]) + 3);
      for (float &v : lattice)
        v = U(rng);
      for (int i = 0; i < n; ++i) {
        const float u = i * kStep / periods[o];
        const int k = (int)u;
        const float f = smooth01(u - k);
        float v = lattice[k] + (lattice[k + 1] - lattice[k]) * f;
        if (peaky && o < 2) v = 1.0f - std::abs(2.0f * v - 1.0f);
        h[i] += v * amps[o];
      }
    }
    const auto [lo, hi] = std::minmax_element(h.begin(), h.end());
    const float mn = *lo, span = std::max(0.001f, *hi - *lo);
    for (float &v : h)
      v = bottom - (bottom - top) * (v - mn) / span;
    return h;
  }

  // ------------------------------------------------------------ drawing --
  void Ease(const Conditions &target, float dt) {
    if (!haveCur) {
      cur = target;
      haveCur = true;
      return;
    }
    const float k = 1.0f - std::exp(-dt / kEaseSecs);
    auto ease = [k](float &v, float t) { v += (t - v) * k; };
    ease(cur.clouds, target.clouds);
    ease(cur.rain, target.rain);
    ease(cur.snow, target.snow);
    ease(cur.fog, target.fog);
    ease(cur.storm, target.storm);
    ease(cur.wind, target.wind);
  }

  // The hill body with a vertical gradient from its ridge line down to `bottomY`, plus a one-pixel feather along
  // the ridge that stands in for anti-aliasing.
  void Ridge(const std::vector<float> &ys, float dx, Col top, Col bottom, float bottomY) {
    const SDL_FColor ct = fc(top), cb = fc(bottom), clear = fc(top, 0.0f);
    for (std::size_t i = 0; i < ys.size(); ++i) {
      const float x = -kMargin + (float)i * kStep + dx, y = ys[i];
      b.add(x, y, ct);
      b.add(x, bottomY, cb);
      b.add(x, y - 1.2f, clear);
      if (i > 0) {
        const int k = 3 * (int)i;
        b.quad(k - 3, k, k + 1, k - 2);
        b.quad(k - 1, k + 2, k, k - 3);
      }
    }
  }

  float RidgeY(int layer, float x) const {
    const std::vector<float> &ys = ridges[layer];
    const float u = std::clamp((x + kMargin) / kStep, 0.0f, (float)ys.size() - 1.001f);
    const int i = (int)u;
    return ys[i] + (ys[i + 1] - ys[i]) * (u - (float)i);
  }

  void Fog(SDL_Renderer *r, double secs, float y, float vis, Col col, float speed) {
    if (vis < 0.01f) return;
    constexpr float w = 760.0f, period = w * 1.6f;
    const float off = (float)std::fmod(secs * speed, (double)period);
    for (float x = off - period; x < Screen::width + period; x += period * 0.5f)
      b.sprite(x, y, w, 110.0f, fc(col, 0.55f * vis));
    b.flush(r, glow.get());
  }

  void DrawLights(SDL_Renderer *r, double secs, float vis, float dx) {
    if (vis < 0.01f) return;
    for (const Light &l : lights) {
      const float x = l.x + dx, y = RidgeY(1, l.x) + l.depth;
      const float tw = 0.75f + 0.25f * (float)std::sin(secs * l.freq + l.phase);
      b.sprite(x, y, 14.0f, 14.0f, fc({255, 190, 110}, 0.35f * vis * tw));
    }
    b.flush(r, glow.get(), SDL_BLENDMODE_ADD);
    for (const Light &l : lights) {
      const float x = l.x + dx, y = RidgeY(1, l.x) + l.depth;
      b.sprite(x, y, l.size * 2.0f, l.size * 2.0f, fc({255, 222, 160}, vis));
    }
    b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
  }

  void DrawLeaves(SDL_Renderer *r, double secs, const Conditions &c, const Palette &p) {
    constexpr int seg = 12;
    const float gust = 0.03f + 0.012f * std::min(c.wind, 15.0f);
    const SDL_FColor edge = fc(p.leaf), rib = fc(mix(p.leaf, p.mid, 0.22f));
    for (const Leaf &l : leaves) {
      const float a = l.angle + gust * (float)std::sin(secs * l.freq + l.phase);
      const float ux = std::cos(a), uy = -std::sin(a); // along the leaf (screen y points down)
      const float nx = -uy, ny = ux;                   // across it
      const int first = (int)b.v.size();
      for (int i = 0; i <= seg; ++i) {
        const float s = (float)i / seg;
        const float along = s * l.len, off = l.bend * s * s * l.len;
        const float cx = l.bx + ux * along + nx * off, cy = l.by + uy * along + ny * off;
        const float swell = std::max(0.0f, std::sin((float)M_PI * std::min(s * 1.08f, 1.0f)));
        const float half = l.width * std::pow(swell, 0.8f) * (1.1f - 0.4f * s);
        b.add(cx + nx * half, cy + ny * half, edge);
        b.add(cx, cy, rib);
        b.add(cx - nx * half, cy - ny * half, edge);
        if (i > 0) {
          const int k = first + 3 * i;
          b.quad(k - 3, k, k + 1, k - 2);
          b.quad(k - 2, k + 1, k + 2, k - 1);
        }
      }
    }
    b.flush(r, nullptr);
  }

  void DrawRain(SDL_Renderer *r, double secs, const Conditions &c, const Palette &p) {
    const int n = (int)(c.rain * kDrops);
    if (n <= 0) return;
    const float W = Screen::width, span = Screen::height + 80.0f;
    const float slant = std::clamp(0.06f + c.wind * 0.035f, 0.0f, 0.5f);
    const float norm = std::sqrt(1.0f + slant * slant);
    const Col col = p.dark ? Col{186, 206, 240} : Col{240, 246, 255};
    const float strength = 0.55f + 0.45f * c.rain;
    for (int i = 0; i < n; ++i) {
      const Drop &d = drops[i];
      const float y = (float)std::fmod(d.y0 * span + d.speed * secs, (double)span) - 40.0f;
      const float x = -slant * span + d.x0 * (W + slant * span) + slant * y;
      const float len = d.len * (0.8f + 0.4f * c.rain);
      const float a = (d.nearDrop ? 0.5f : 0.3f) * strength;
      b.segment(x - slant / norm * len, y - len / norm, x, y, d.nearDrop ? 1.7f : 1.1f, fc(col, 0.0f), fc(col, a));
    }
    b.flush(r, nullptr);
  }

  void DrawSnow(SDL_Renderer *r, double secs, const Conditions &c) {
    const int n = (int)(c.snow * kFlakes);
    if (n <= 0) return;
    const float W = Screen::width, span = Screen::height + 30.0f;
    for (int i = 0; i < n; ++i) {
      const Flake &f = flakes[i];
      const float y = (float)std::fmod(f.y0 * span + f.speed * secs, (double)span) - 15.0f;
      const float x =
          f.x0 * (W + 40.0f) - 20.0f + f.amp * (float)std::sin(secs * f.freq + f.phase) + y * c.wind * 0.03f;
      b.sprite(x, y, f.size * 2.0f, f.size * 2.0f, fc({255, 255, 255}, 0.9f));
    }
    b.flush(r, dot.get());
  }

  void DrawMeteor(SDL_Renderer *r, double secs, float starVis) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    if (secs >= nextMeteor) {
      nextMeteor = secs + 35.0 + U(events) * 70.0;
      if (starVis > 0.6f) {
        meteorAt = secs;
        meteorX = 200.0f + U(events) * 650.0f;
        meteorY = 20.0f + U(events) * 90.0f;
        meteorDir = (U(events) < 0.5f ? 0.45f : (float)M_PI - 0.45f) + (U(events) - 0.5f) * 0.3f;
      }
    }
    const float u = (float)(secs - meteorAt) / 0.9f;
    if (u < 0.0f || u >= 1.0f) return;
    const float hx = meteorX + std::cos(meteorDir) * u * 380.0f, hy = meteorY + std::sin(meteorDir) * u * 380.0f;
    const float tail = 110.0f * std::sin((float)M_PI * u);
    const float a = starVis * (1.0f - u * u * u * u);
    b.segment(hx - std::cos(meteorDir) * tail, hy - std::sin(meteorDir) * tail, hx, hy, 1.8f, fc({255, 250, 240}, 0.0f),
              fc({255, 250, 240}, a));
    b.flush(r, nullptr, SDL_BLENDMODE_ADD);
  }

  // Brightness of a lightning strike `t` seconds after it began: a double flicker, then a fading afterglow.
  static float flashAt(float t) {
    if (t < 0.0f) return 0.0f;
    if (t < 0.07f) return 1.0f;
    if (t < 0.14f) return 0.25f;
    if (t < 0.22f) return 0.85f;
    return std::exp(-(t - 0.22f) * 5.0f);
  }

  void DrawLightning(SDL_Renderer *r, double secs, const Conditions &c, const Palette &p) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    if (c.storm > 0.5f && secs >= nextStrike) {
      if (nextStrike > 0) {
        strikeAt = secs;
        strikeSeed = events();
      }
      nextStrike = secs + 5.0 + U(events) * 11.0;
    }
    const float t = (float)(secs - strikeAt);
    const float f = flashAt(t) * c.storm;
    if (f < 0.01f) return;

    // The sky lights up behind the hills, which stay dark silhouettes.
    const Col sky = mix(p.cloudLit, {235, 225, 255}, 0.6f);
    b.spriteV(Screen::width * 0.5f, Layout::horizonY * 0.5f + 10.0f, Screen::width, Layout::horizonY + 20.0f,
              fc(sky, 0.3f * f), fc(sky, 0.12f * f));
    b.flush(r, nullptr, SDL_BLENDMODE_ADD);
    if (t > 0.5f) return;

    // A jagged bolt with one branch, reproducible from its seed for the whole strike.
    std::mt19937 bolt(strikeSeed);
    float x = 560.0f + U(bolt) * 400.0f, y = -10.0f;
    const float yEnd = Layout::farTop + U(bolt) * 50.0f;
    const int steps = 10, branchAt = 3 + (int)(U(bolt) * 3);
    const SDL_FColor core = fc({245, 240, 255}, f), halo = fc({190, 170, 255}, 0.3f * f);
    for (int i = 0; i < steps; ++i) {
      const float nx = x + (U(bolt) - 0.5f) * 46.0f, ny = y + (yEnd + 10.0f) / steps;
      b.segment(x, y, nx, ny, 8.0f, halo, halo);
      b.segment(x, y, nx, ny, 2.4f, core, core);
      if (i == branchAt) {
        float bx = nx, by = ny;
        const float dir = U(bolt) < 0.5f ? -1.0f : 1.0f;
        for (int j = 0; j < 4; ++j) {
          const float ex = bx + dir * (10.0f + U(bolt) * 22.0f), ey = by + 16.0f + U(bolt) * 10.0f;
          b.segment(bx, by, ex, ey, 1.4f, fc({245, 240, 255}, 0.7f * f), fc({245, 240, 255}, 0.7f * f));
          bx = ex, by = ey;
        }
      }
      x = nx, y = ny;
    }
    b.flush(r, nullptr, SDL_BLENDMODE_ADD);
  }
};
