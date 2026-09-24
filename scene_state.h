#pragma once

// What the world outside looks like right now, as a handful of numbers (SceneState), and the colours of the one
// landscape that follow from them (Look).
//
//   time + place ──► sun and moon (astro.h) ─┐
//                                            ├─► SceneState ──► Look ──► Scene (scene.h) draws the layers
//   forecast ─────► clouds, rain, fog, wind ─┘                    └────► TextTheme: readable ink over it
//
// Nothing in here picks a background: the sky colours are keyed to the sun's elevation (so dawn is dawn in June and
// in December), weather grades them (grey, darker, hazier), and every combination of the two falls out of the
// same few functions. This file has no SDL in it, so it can be checked on its own.

#include "astro.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace Screen {
constexpr int width = 1024;
constexpr int height = 600;
} // namespace Screen

// Where things sit on screen (logical pixels).
namespace Layout {
// Text. The date and the time sit on the sky; the weather row, the advice and the event horizon on the land.
constexpr float padX = 48.0f;
constexpr float dateBaseline = 60.0f;
constexpr float timeSize = 250.0f;
constexpr float timeBaseline = 294.0f;
constexpr float timeTop = 118.0f; // top of the digits
constexpr float stripBaseline = 368.0f;
constexpr float adviceBaseline = 410.0f;
constexpr float horizonTitleBaseline = 514.0f; // event horizon: a mark's title,
constexpr float horizonWhenBaseline = 532.0f;  // its time (and the rain and snow captions),
constexpr float horizonLineY = 550.0f;         // the time scale,
constexpr float horizonAxisBaseline = 578.0f;  // and NOW .. +4H

// Landscape, back to front.
constexpr float skyMidY = 250.0f;                      // sky gradient stop (the top is at 0)
constexpr float horizonY = 440.0f;                     // the far shore of the lake: sky gradient bottom, sun at 0°
constexpr float farTop = 176.0f, farBottom = 270.0f;   // ridge-line range of the distant mountains
constexpr float midTop = 238.0f, midBottom = 300.0f;   // a middle range
constexpr float nearTop = 304.0f, nearBottom = 334.0f; // forested hills, which carry the weather row
constexpr float lakeBottom = 494.0f;                   // the near shore starts here
constexpr float shoreTop = 488.0f, shoreBottom = 508.0f;
} // namespace Layout

// ---------------------------------------------------------------- colour --

struct Col {
  float r = 0, g = 0, b = 0, a = 255; // 0..255, kept as float for cheap mixing
};
constexpr Col mix(const Col &a, const Col &b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
constexpr Col scale(const Col &c, float k) { return {c.r * k, c.g * k, c.b * k, c.a}; }
inline float smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

inline float relativeLuminance(Col c) {
  auto lin = [](float v) {
    v = std::clamp(v, 0.0f, 255.0f) / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
  };
  return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
inline float contrast(Col a, Col b) {
  float la = relativeLuminance(a), lb = relativeLuminance(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05f) / (lb + 0.05f);
}

// Greys a colour out (keeping a hint of its hue, nudged cool, as under cloud) and dims it.
inline Col grade(Col c, float desat, float dim) {
  const float y = 0.30f * c.r + 0.59f * c.g + 0.11f * c.b;
  const Col grey = mix(Col{y - 3.0f, y, y + 6.0f, c.a}, c, 0.2f);
  return scale(mix(c, grey, std::clamp(desat, 0.0f, 1.0f)), dim);
}

// ----------------------------------------------------------- scene state --

// Everything the scene is drawn from. The time fields follow the clock; the weather fields ease towards the
// forecast so a change rolls in over a few seconds (and wetness and snow cover over minutes).
struct SceneState {
  // time and sky
  float timeOfDay = 0;       // local hours, 0..24
  float sunElevation = -30;  // degrees above the horizon
  float sunAzimuth = 0;      // degrees from north
  float evening = 0;         // 0 while the sun climbs, 1 while it sinks (which palette of the day: dawn or dusk)
  float moonElevation = -30; // degrees
  float moonAzimuth = 180;   // degrees
  float moonPhase = 0.5f;    // 0 new, 0.5 full
  // weather, 0..1 unless noted
  float cloudiness = 0.2f;
  float rainIntensity = 0;
  float snowIntensity = 0;
  float storm = 0;
  float visibility = 1;      // 1 crisp, 0 thick fog
  float windSpeed = 3;       // m/s
  float windDirection = 270; // degrees the wind comes from
  float wetness = 0;         // the land darkens after rain starts and dries out slowly after it stops
  float snowCover = 0;       // snow settles on the land
  // derived
  float sceneBrightness = 0; // relative luminance of the sky, 0..1
  float atmosphere = 0;      // haze over the distance, 0..1
};

// The forecast, as the scene needs it.
struct WeatherInput {
  bool valid = false;
  int code = -1;       // WMO weather code
  float windSpeed = 3; // m/s
  float windDirection = 270;
  float rainNow = 0; // mm expected in the current 15 minutes
};

// Target weather fields for a forecast (visibility, clouds, precipitation), from the WMO weather code.
inline void applyWeather(SceneState &s, const WeatherInput &w) {
  s.windSpeed = w.windSpeed;
  s.windDirection = w.windDirection;
  float clouds = 0.2f, rain = 0, snow = 0, storm = 0, vis = 1.0f;
  switch (w.valid ? w.code : -1) {
  case 0:
    clouds = 0.05f;
    break;
  case 1:
    clouds = 0.25f;
    break;
  case 2:
    clouds = 0.5f;
    break;
  case 3:
    clouds = 1.0f;
    vis = 0.85f;
    break;
  case 45:
  case 48:
    clouds = 0.75f;
    vis = 0.12f;
    break;
  case 51:
  case 53:
  case 55:
  case 56:
  case 57:
    clouds = 0.9f;
    rain = 0.15f + 0.1f * (float)((w.code - 51) % 5 / 2);
    vis = 0.6f;
    break;
  case 61:
  case 66:
  case 80:
    clouds = 0.92f;
    rain = 0.45f;
    vis = 0.7f;
    break;
  case 63:
  case 81:
    clouds = 1.0f;
    rain = 0.7f;
    vis = 0.55f;
    break;
  case 65:
  case 67:
  case 82:
    clouds = 1.0f;
    rain = 1.0f;
    vis = 0.38f;
    break;
  case 71:
  case 77:
    clouds = 0.92f;
    snow = 0.4f;
    vis = 0.6f;
    break;
  case 73:
  case 85:
    clouds = 1.0f;
    snow = 0.7f;
    vis = 0.45f;
    break;
  case 75:
  case 86:
    clouds = 1.0f;
    snow = 1.0f;
    vis = 0.3f;
    break;
  case 95:
  case 96:
  case 99:
    clouds = 1.0f;
    rain = 0.9f;
    storm = 1.0f;
    vis = 0.45f;
    break;
  default:
    break; // no forecast yet: a fair sky
  }
  // The 15-minute forecast knows about a shower before the current weather code catches up.
  if (w.valid && w.rainNow > 0.05f && snow == 0.0f) {
    rain = std::max(rain, std::clamp(w.rainNow / 1.5f, 0.2f, 1.0f));
    clouds = std::max(clouds, 0.85f);
    vis = std::min(vis, 1.0f - 0.5f * rain);
  }
  s.cloudiness = clouds;
  s.rainIntensity = rain;
  s.snowIntensity = snow;
  s.storm = storm;
  s.visibility = vis;
  s.wetness = rain > 0.05f ? 1.0f : 0.0f;
  s.snowCover = snow > 0.05f ? 1.0f : 0.0f;
}

// Sets the time fields from the sky over the place at the moment.
inline void applySky(SceneState &s, const Astro::Sky &sky, float timeOfDay) {
  s.timeOfDay = timeOfDay;
  s.sunElevation = (float)sky.sun.elevation;
  s.sunAzimuth = (float)sky.sun.azimuth;
  s.moonElevation = (float)sky.moon.elevation;
  s.moonAzimuth = (float)sky.moon.azimuth;
  s.moonPhase = (float)sky.moonPhase;
  // Morning or evening light, by which side of the meridian the sun is on: pure at sunrise and sunset, and blended
  // around noon and midnight, where the two palettes meet.
  const float side = (float)std::sin((sky.sun.azimuth - 180.0) * Astro::kDeg);
  s.evening = smooth01(0.5f + 0.9f * side);
}

// Moves the weather fields of `cur` towards `target` over `dt` seconds; the time fields are taken as they are.
inline void easeScene(SceneState &cur, const SceneState &target, float dt) {
  auto ease = [dt](float &v, float t, float secs) { v += (t - v) * (1.0f - std::exp(-dt / secs)); };
  const SceneState was = cur;
  cur = target;
  cur.cloudiness = was.cloudiness;
  cur.rainIntensity = was.rainIntensity;
  cur.snowIntensity = was.snowIntensity;
  cur.storm = was.storm;
  cur.visibility = was.visibility;
  cur.windSpeed = was.windSpeed;
  cur.wetness = was.wetness;
  cur.snowCover = was.snowCover;
  ease(cur.cloudiness, target.cloudiness, 8.0f);
  ease(cur.rainIntensity, target.rainIntensity, 6.0f);
  ease(cur.snowIntensity, target.snowIntensity, 6.0f);
  ease(cur.storm, target.storm, 8.0f);
  ease(cur.visibility, target.visibility, 10.0f);
  ease(cur.windSpeed, target.windSpeed, 10.0f);
  // Wet within a couple of minutes of rain; dry over a quarter of an hour. Snow settles and melts slower still.
  ease(cur.wetness, target.wetness, target.wetness > cur.wetness ? 90.0f : 900.0f);
  ease(cur.snowCover, target.snowCover * target.snowIntensity, target.snowCover > 0 ? 600.0f : 1800.0f);
}

// ------------------------------------------------------------------ look --

// The colours of every layer of the landscape at one moment, plus how visible the lights in the sky are.
struct Look {
  Col skyTop, skyMid, skyLow; // sky gradient, top to horizon
  Col glow;                   // horizon glow around the sun (strength in alpha)
  Col sun;                    // sun disc
  Col cloudLit, cloudShade;   // cloud tops and bellies
  Col far, mid, near;         // distant mountains, the middle range and the forested hills
  Col forest;                 // the far shore: forest and town
  Col ground, fore;           // the near shore and the foreground trees
  Col haze;                   // fog and mist
  Col accent;                 // the scene's accent hue (before it is made readable)
  float sunVis = 0, moonVis = 0, starVis = 0, lightsVis = 0, glowVis = 0;
  float overcast = 0; // how much of the sky the cloud deck covers
};

// Keyframes of the day by the sun's elevation; one set for the morning, one for the evening.
struct SkyKey {
  float elevation;
  Col skyTop, skyMid, skyLow, glow, sun, cloudLit, cloudShade, far, near, forest, ground, fore, accent;
};

constexpr std::array kKeyColours{&SkyKey::skyTop, &SkyKey::skyMid,   &SkyKey::skyLow,     &SkyKey::glow,
                                 &SkyKey::sun,    &SkyKey::cloudLit, &SkyKey::cloudShade, &SkyKey::far,
                                 &SkyKey::near,   &SkyKey::forest,   &SkyKey::ground,     &SkyKey::fore,
                                 &SkyKey::accent};

namespace Keys {
// Field order: elevation / skyTop skyMid skyLow glow sun / cloudLit cloudShade / far near forest ground fore /
// accent. The land from `near` forwards stays dark at every hour: the weather row and the event horizon sit on it.
// clang-format off
constexpr SkyKey kNight{-18,
    {6, 10, 30}, {14, 24, 62}, {34, 50, 104}, {80, 100, 190, 0}, {255, 200, 150},
    {46, 56, 98}, {22, 28, 58},
    {28, 38, 84}, {16, 24, 56}, {10, 15, 38}, {8, 12, 30}, {4, 7, 18},
    {170, 150, 255}};
constexpr SkyKey kDay{30,
    {58, 134, 222}, {124, 184, 240}, {202, 228, 248}, {255, 255, 240, 50}, {255, 252, 235},
    {255, 255, 255}, {200, 214, 234},
    {122, 160, 208}, {42, 76, 100}, {22, 52, 42}, {28, 58, 40}, {10, 28, 22},
    {70, 176, 255}};

constexpr std::array<SkyKey, 7> kMorning{{
    kNight,
    // nautical dawn: deep blue, the faintest violet glow
    {-10, {12, 18, 52}, {36, 46, 104}, {96, 96, 160}, {170, 140, 210, 40}, {255, 200, 160},
     {70, 74, 130}, {34, 38, 80},
     {40, 48, 100}, {24, 28, 66}, {14, 17, 44}, {11, 14, 34}, {6, 8, 22},
     {180, 160, 255}},
    // civil dawn: rose horizon under a blue sky
    {-4, {44, 56, 122}, {140, 118, 178}, {250, 164, 150}, {255, 170, 150, 110}, {255, 206, 160},
     {236, 160, 170}, {96, 82, 136},
     {98, 80, 140}, {52, 44, 92}, {28, 22, 54}, {22, 18, 44}, {12, 9, 26},
     {255, 170, 140}},
    // sunrise: pink and peach
    {0, {86, 112, 182}, {226, 160, 170}, {255, 196, 140}, {255, 206, 150, 150}, {255, 214, 160},
     {255, 210, 190}, {170, 120, 150},
     {150, 112, 150}, {62, 52, 92}, {32, 26, 54}, {26, 22, 44}, {14, 11, 26},
     {255, 160, 110}},
    // early morning: the blue comes back, a warm horizon
    {6, {104, 158, 222}, {196, 200, 222}, {255, 222, 180}, {255, 230, 190, 110}, {255, 236, 190},
     {255, 240, 226}, {178, 176, 204},
     {140, 150, 190}, {52, 70, 100}, {26, 44, 48}, {26, 44, 40}, {12, 22, 22},
     {255, 176, 110}},
    // morning: cool blue
    {15, {78, 150, 226}, {150, 198, 240}, {214, 232, 246}, {255, 250, 230, 70}, {255, 248, 220},
     {255, 255, 255}, {196, 212, 232},
     {126, 160, 206}, {44, 78, 102}, {22, 50, 44}, {26, 54, 40}, {10, 26, 22},
     {80, 180, 255}},
    kDay,
}};

constexpr std::array<SkyKey, 7> kEvening{{
    kNight,
    // late dusk: indigo with a plum horizon
    {-10, {14, 16, 50}, {40, 38, 98}, {104, 72, 140}, {180, 110, 190, 40}, {255, 190, 150},
     {76, 62, 120}, {36, 32, 76},
     {44, 40, 96}, {26, 24, 62}, {15, 14, 40}, {12, 11, 32}, {6, 6, 20},
     {176, 150, 255}},
    // twilight: violet over an ember horizon
    {-4, {34, 30, 92}, {104, 70, 146}, {236, 128, 120}, {255, 130, 110, 110}, {255, 170, 120},
     {220, 130, 150}, {86, 62, 122},
     {86, 62, 128}, {48, 36, 86}, {26, 18, 50}, {20, 14, 40}, {11, 7, 24},
     {255, 140, 120}},
    // sunset: orange and coral
    {0, {70, 72, 148}, {214, 124, 128}, {255, 160, 90}, {255, 150, 80, 170}, {255, 176, 96},
     {255, 176, 140}, {150, 90, 120},
     {146, 90, 120}, {64, 42, 78}, {34, 22, 46}, {28, 18, 38}, {16, 10, 22},
     {255, 130, 100}},
    // golden hour
    {6, {96, 132, 206}, {232, 178, 150}, {255, 200, 126}, {255, 196, 120, 140}, {255, 220, 150},
     {255, 222, 190}, {200, 150, 150},
     {162, 130, 150}, {60, 58, 86}, {32, 36, 44}, {32, 38, 36}, {16, 18, 20},
     {255, 150, 90}},
    // afternoon: a touch warmer than the morning
    {15, {80, 146, 218}, {166, 198, 232}, {236, 230, 214}, {255, 240, 210, 80}, {255, 244, 210},
     {255, 252, 244}, {206, 208, 222},
     {134, 158, 198}, {46, 76, 98}, {24, 50, 42}, {30, 56, 38}, {12, 26, 20},
     {90, 176, 255}},
    kDay,
}};
// clang-format on
} // namespace Keys

inline SkyKey mixKey(const SkyKey &a, const SkyKey &b, float t) {
  SkyKey r = a;
  r.elevation = a.elevation + (b.elevation - a.elevation) * t;
  for (auto m : kKeyColours)
    r.*m = mix(a.*m, b.*m, t);
  return r;
}

template <std::size_t N> SkyKey keyAt(const std::array<SkyKey, N> &keys, float elevation) {
  if (elevation <= keys.front().elevation) return keys.front();
  for (std::size_t i = 0; i + 1 < N; ++i) {
    const SkyKey &a = keys[i], &b = keys[i + 1];
    if (elevation < b.elevation) return mixKey(a, b, (elevation - a.elevation) / (b.elevation - a.elevation));
  }
  return keys.back();
}

inline Look lookFor(const SceneState &s) {
  const SkyKey k = mixKey(keyAt(Keys::kMorning, s.sunElevation), keyAt(Keys::kEvening, s.sunElevation), s.evening);
  Look l;
  l.skyTop = k.skyTop, l.skyMid = k.skyMid, l.skyLow = k.skyLow, l.glow = k.glow, l.sun = k.sun;
  l.cloudLit = k.cloudLit, l.cloudShade = k.cloudShade;
  l.far = k.far, l.mid = mix(k.far, k.near, 0.3f), l.near = k.near, l.forest = k.forest, l.ground = k.ground,
  l.fore = k.fore;

  const float fog = 1.0f - s.visibility;
  const float overcast = smooth01((s.cloudiness - 0.3f) / 0.7f);
  l.overcast = overcast;
  // Under cloud the light goes grey and dim; rain and storms darken it further.
  const float desat = std::min(1.0f, 0.75f * overcast + 0.15f * s.rainIntensity + 0.3f * fog);
  const float dim = 1.0f - 0.16f * overcast - 0.16f * s.rainIntensity - 0.28f * s.storm;
  for (Col *c : {&l.skyTop, &l.skyMid, &l.skyLow, &l.far, &l.mid})
    *c = grade(*c, desat, dim);
  for (Col *c : {&l.near, &l.forest, &l.ground, &l.fore})
    *c = grade(*c, desat * 0.6f, dim);
  l.cloudLit = grade(l.cloudLit, 0.6f * overcast + 0.2f * s.rainIntensity, dim * (1.0f - 0.08f * overcast));
  l.cloudShade = grade(l.cloudShade, 0.5f * overcast, dim * (1.0f - 0.18f * overcast - 0.2f * s.rainIntensity));
  if (s.storm > 0.0f) { // a bruised, violet storm light
    const Col bruise{58, 48, 96};
    for (Col *c : {&l.skyTop, &l.skyMid, &l.skyLow, &l.far, &l.mid, &l.cloudLit, &l.cloudShade})
      *c = mix(*c, scale(bruise, 0.5f + relativeLuminance(*c) * 1.5f), 0.55f * s.storm);
  }

  // Haze: fog and heavy rain wash out the distance first.
  l.haze = grade(mix(l.skyLow, l.cloudLit, 0.5f), 0.6f, 0.95f);
  const float atmos = std::clamp(s.atmosphere, 0.0f, 1.0f);
  l.skyTop = mix(l.skyTop, l.haze, 0.55f * fog);
  l.skyMid = mix(l.skyMid, l.haze, 0.75f * fog);
  l.skyLow = mix(l.skyLow, l.haze, 0.9f * fog);
  l.far = mix(l.far, l.haze, std::min(1.0f, atmos * 1.15f));
  l.mid = mix(l.mid, l.haze, std::min(1.0f, atmos * 0.95f));
  l.near = mix(l.near, l.haze, 0.7f * atmos);
  l.forest = mix(l.forest, l.haze, 0.5f * atmos);
  l.ground = mix(l.ground, l.haze, 0.18f * atmos);
  l.fore = mix(l.fore, l.haze, 0.08f * atmos);

  // Snow settles on the land; the light off it goes with the daylight.
  if (s.snowCover > 0.001f) {
    const Col snow = mix(Col{52, 60, 104}, Col{226, 234, 246}, smooth01((s.sunElevation + 12.0f) / 18.0f) * dim);
    const float sc = s.snowCover;
    l.far = mix(l.far, snow, 0.5f * sc);
    l.mid = mix(l.mid, scale(snow, 0.8f), 0.45f * sc);
    l.near = mix(l.near, scale(snow, 0.62f), 0.45f * sc);
    l.forest = mix(l.forest, scale(snow, 0.45f), 0.3f * sc);
    l.ground = mix(l.ground, scale(snow, 0.8f), 0.7f * sc);
    l.fore = mix(l.fore, scale(snow, 0.4f), 0.25f * sc);
  }
  // Wet ground is darker.
  l.ground = scale(l.ground, 1.0f - 0.2f * s.wetness);
  l.fore = scale(l.fore, 1.0f - 0.12f * s.wetness);

  // Accent: the scene's hue, pulled cool in rain, electric in a storm, icy in snow, grey in fog.
  l.accent = k.accent;
  l.accent = mix(l.accent, Col{110, 176, 255}, 0.8f * s.rainIntensity);
  l.accent = mix(l.accent, Col{176, 140, 255}, 0.9f * s.storm);
  l.accent = mix(l.accent, Col{200, 220, 255}, 0.7f * s.snowIntensity);
  l.accent = mix(l.accent, Col{190, 200, 215}, 0.5f * fog);

  // Lights in the sky and on the land.
  const float night = smooth01((-6.0f - s.sunElevation) / 8.0f);
  const float clearSky = std::clamp(1.0f - s.cloudiness * 1.15f, 0.0f, 1.0f) * (1.0f - fog);
  l.starVis = night * clearSky;
  l.sunVis = smooth01((s.sunElevation + 1.5f) / 3.0f) * (1.0f - 0.9f * overcast) * (1.0f - 0.7f * fog);
  const float moonUp = smooth01((s.moonElevation + 1.0f) / 4.0f);
  l.moonVis = moonUp * (0.3f + 0.7f * night) * (1.0f - 0.92f * overcast) * (1.0f - 0.7f * fog);
  l.lightsVis = smooth01((-1.0f - s.sunElevation) / 6.0f) * (1.0f - 0.5f * fog);
  l.glowVis = (l.glow.a / 255.0f) * (1.0f - 0.8f * overcast) * (1.0f - 0.7f * fog);
  return l;
}

// Fills in the derived fields.
inline void deriveScene(SceneState &s) {
  s.atmosphere = std::clamp(1.0f - s.visibility + 0.15f * s.rainIntensity, 0.0f, 1.0f);
  s.sceneBrightness = relativeLuminance(lookFor(s).skyMid);
}

// Sky position (degrees) to the screen: looking south, east on the left; 0° elevation on the far shore. Heights
// are stretched near the horizon, so a winter sun a dozen degrees up clears the mountains, and a summer noon sun
// still stays on screen.
struct ScreenPos {
  float x, y;
};
inline ScreenPos skyToScreen(float azimuth, float elevation) {
  constexpr float span = 100.0f; // degrees of azimuth from the middle of the screen to its edge
  const float x = Screen::width * 0.5f + (azimuth - 180.0f) / span * Screen::width * 0.5f;
  const float up = std::sin(std::abs(elevation) * (float)Astro::kDeg);
  const float y = Layout::horizonY - (elevation < 0 ? -1.0f : 1.0f) * 420.0f * std::pow(up, 0.4f);
  return {x, y};
}

// The lake at the far shore: the horizon sky, mirrored and darkened.
inline Col waterColour(const Look &l) { return scale(mix(l.skyLow, l.near, 0.22f), 0.86f); }

inline Col skyAt(const Look &l, float y) {
  if (y <= Layout::skyMidY) return mix(l.skyTop, l.skyMid, std::max(0.0f, y) / Layout::skyMidY);
  const float t = std::min(1.0f, (y - Layout::skyMidY) / (Layout::horizonY - Layout::skyMidY));
  return mix(l.skyMid, l.skyLow, t * t); // the horizon colour gathers low
}

// ------------------------------------------------------------ text theme --

// The colours of the text over the scene, and how strong a soft backing it needs behind it to stay readable.
// The sky text flips between light and dark ink with the sky; the land text is always light, over land that is dark
// by design. Wherever the scene alone falls short of the targets (twilight skies halfway between light and dark,
// fog, snow on the ground), a soft halo of the opposite tone is laid behind the text, as strong as it needs to be
// and no stronger — so the text is readable by construction, whatever the weather does.
struct TextTheme {
  bool lightInk = true;
  Col ink, inkDim, accent; // date, time, condition, colon
  Col halo;                // behind the sky text
  float haloTime = 0, haloTop = 0;
  Col landInk{244, 246, 250}, landDim{208, 214, 226}, landMute{188, 196, 210}, landAccent;
  Col scrim{6, 8, 18}; // behind the land text
  float scrimStrip = 0, scrimHorizon = 0;
};

namespace Ink {
constexpr Col light{246, 246, 252}, lightDim{212, 216, 232};
constexpr Col dark{12, 18, 34}, darkDim{34, 46, 72};
constexpr Col lightHalo{6, 8, 22}, darkHalo{250, 250, 255};
} // namespace Ink

namespace Readability {
constexpr float time = 5.0f; // huge digits
constexpr float text = 4.5f; // everything else
constexpr float accent = 2.5f; // the colon: huge and decorative, so it keeps its colour at sunrise and sunset
constexpr float landInk = 6.0f;
} // namespace Readability

// The least backing (0..maxA) of colour `halo` that lifts `ink` to `target` against every backdrop.
inline float backingNeeded(Col ink, Col halo, std::initializer_list<Col> backdrops, float target, float maxA = 0.8f) {
  float need = 0.0f;
  for (const Col &bg : backdrops) {
    if (contrast(ink, bg) >= target) continue;
    float lo = 0.0f, hi = maxA;
    for (int i = 0; i < 12; ++i) {
      const float m = 0.5f * (lo + hi);
      (contrast(ink, mix(bg, halo, m)) >= target ? hi : lo) = m;
    }
    need = std::max(need, hi);
  }
  return need;
}

// The accent hue, lightened (on a dark sky) or deepened (on a light one) until it reads against every backdrop. It
// deepens through a darker shade of the same hue first, so a sunset accent stays orange rather than going brown.
inline Col readableAccent(Col hue, Col towards, std::initializer_list<Col> backdrops, float target) {
  const bool deepen = relativeLuminance(towards) < relativeLuminance(hue);
  const float top = std::max({hue.r, hue.g, hue.b, 1.0f});
  auto saturated = [&](float c) { return c * std::pow(c / top, 1.5f) * 0.7f; };
  const Col shade =
      deepen ? Col{saturated(hue.r), saturated(hue.g), saturated(hue.b)} : mix(hue, Col{255, 255, 255}, 0.5f);
  for (int i = 0; i <= 40; ++i) {
    const float k = i / 20.0f;
    const Col c = k <= 1.0f ? mix(hue, shade, k) : mix(shade, towards, k - 1.0f);
    bool ok = true;
    for (const Col &bg : backdrops)
      ok = ok && contrast(c, bg) >= target;
    if (ok) return c;
  }
  return towards;
}

// How the backing is laid: mostly as a soft shadow (or, behind dark ink, a glow) around the glyphs themselves,
// which reads as depth rather than as a patch; only what the glyph shadow cannot supply goes into a soft panel. The
// blurred shadow is about half its strength right at the edge of a stroke, which is where it has to count.
struct Backing {
  float glyph = 0; // strength of the glyph shadow or glow, 0..1
  float panel = 0; // strength of the soft panel behind the whole run of text
};
inline Backing backingFor(float need, float base) {
  Backing b;
  b.glyph = std::clamp(base + 2.0f * need, 0.0f, 1.0f);
  b.panel = std::max(0.0f, need - 0.5f * b.glyph);
  return b;
}

// Samples of what lies behind each piece of text, taken from the look (clouds at full strength, so anything
// partial in between is covered).
struct Backdrops {
  Col top[4];  // date and condition
  Col time[8]; // the digits
  Col strip[5], horizon[4];
};
inline Backdrops backdropsFor(const Look &l, const SceneState &s) {
  const float cloudy = s.cloudiness > 0.02f ? 1.0f : 0.0f;
  auto sky = [&](float y) { return skyAt(l, y); };
  auto cloud = [&](float y, const Col &c) { return mix(sky(y), c, cloudy * 0.95f); };
  Backdrops b{};
  b.top[0] = sky(40), b.top[1] = sky(64), b.top[2] = cloud(50, l.cloudLit), b.top[3] = cloud(50, l.cloudShade);
  const Col glowSky = mix(sky(Layout::timeBaseline), l.glow, 0.5f * l.glowVis);
  b.time[0] = sky(Layout::timeTop), b.time[1] = sky(210), b.time[2] = sky(Layout::timeBaseline);
  b.time[3] = glowSky, b.time[4] = l.far;
  b.time[5] = cloud(200, l.cloudLit), b.time[6] = cloud(200, l.cloudShade), b.time[7] = l.mid;
  const Col mist = l.haze;
  const float fogBand = 0.45f * (1.0f - s.visibility);
  b.strip[0] = l.near, b.strip[1] = mix(l.near, l.forest, 0.6f), b.strip[2] = l.forest;
  b.strip[3] = mix(l.near, mist, fogBand), b.strip[4] = mix(l.forest, mist, fogBand);
  b.horizon[0] = l.ground, b.horizon[1] = mix(l.ground, l.fore, 0.6f), b.horizon[2] = l.fore;
  b.horizon[3] = mix(l.ground, mist, 0.5f * fogBand);
  return b;
}

// Picks the ink for the sky text (with a little hysteresis, so it does not flicker at the crossover) and works out
// the backing each piece of text needs.
inline TextTheme textThemeFor(const Look &l, const SceneState &s, bool wasLight) {
  const Backdrops b = backdropsFor(l, s);
  auto worst = [](Col ink, const auto &bgs) {
    float m = 99.0f;
    for (const Col &bg : bgs)
      m = std::min(m, contrast(ink, bg));
    return m;
  };
  const float lightC = std::min(worst(Ink::light, b.time), worst(Ink::lightDim, b.top));
  const float darkC = std::min(worst(Ink::dark, b.time), worst(Ink::darkDim, b.top));
  TextTheme t;
  t.lightInk = wasLight ? lightC * 1.2f >= darkC : lightC > darkC * 1.2f;
  t.ink = t.lightInk ? Ink::light : Ink::dark;
  t.inkDim = t.lightInk ? Ink::lightDim : Ink::darkDim;
  t.halo = t.lightInk ? Ink::lightHalo : Ink::darkHalo;
  const auto &T = b.time;
  const auto &P = b.top;
  t.haloTime = backingNeeded(t.ink, t.halo, {T[0], T[1], T[2], T[3], T[4], T[5], T[6], T[7]}, Readability::time);
  t.haloTop = backingNeeded(t.inkDim, t.halo, {P[0], P[1], P[2], P[3]}, Readability::text);
  auto backed = [&](Col c, float a) { return mix(c, t.halo, a); };
  const float ha = t.haloTime;
  t.accent = readableAccent(l.accent, t.ink,
                            {backed(T[0], ha), backed(T[1], ha), backed(T[2], ha), backed(T[3], ha), backed(T[4], ha),
                             backed(T[5], ha), backed(T[6], ha), backed(T[7], ha)},
                            Readability::accent);

  // Land text: always light ink, with a dark scrim only where the land is not dark enough on its own.
  t.landAccent = mix(l.accent, Col{255, 255, 255}, 0.35f);
  const auto &S = b.strip;
  const auto &R = b.horizon;
  auto scrimFor = [&](std::initializer_list<Col> bgs) {
    return std::max({backingNeeded(t.landInk, t.scrim, bgs, Readability::landInk, 0.85f),
                     backingNeeded(t.landMute, t.scrim, bgs, Readability::text, 0.85f),
                     backingNeeded(t.landAccent, t.scrim, bgs, Readability::text, 0.85f)});
  };
  t.scrimStrip = scrimFor({S[0], S[1], S[2], S[3], S[4]});
  t.scrimHorizon = scrimFor({R[0], R[1], R[2], R[3]});
  return t;
}
