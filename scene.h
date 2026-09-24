#pragma once

// The living background: one landscape — mountains over a lake, a forest and a small town on the far shore, pines
// in front — drawn in depth layers from a SceneState and its Look (scene_state.h):
//
//   sky · horizon glow · stars · sun and moon · far clouds and the cloud deck · lightning
//   distant mountains · low cloud · mist · nearer mountains · mist · far shore: forest and town (lights at night)
//   lake: sky and mountains mirrored, the sun's or moon's glitter path, shimmer, rain rings
//   near shore and reeds · foreground pines · rain, snow · a painted-canvas texture
//
// The layers sway sideways very slowly by an amount that grows with their nearness (parallax); clouds drift with the
// wind, and wind bends the rain and sways the pines and reeds. Everything is drawn on the GPU as a few dozen batched
// SDL_RenderGeometry calls over a handful of small sprites baked at start-up, so a frame stays cheap on a Pi.

#include "scene_state.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

template <typename T, auto Deleter>
using SdlPtr = std::unique_ptr<T, std::integral_constant<decltype(Deleter), Deleter>>;
using SurfacePtr = SdlPtr<SDL_Surface, SDL_DestroySurface>;
using TexturePtr = SdlPtr<SDL_Texture, SDL_DestroyTexture>;

inline SDL_FColor fc(Col c, float alpha = 1.0f) {
  return {std::clamp(c.r, 0.0f, 255.0f) / 255.0f, std::clamp(c.g, 0.0f, 255.0f) / 255.0f,
          std::clamp(c.b, 0.0f, 255.0f) / 255.0f, std::clamp(alpha, 0.0f, 1.0f)};
}

// A soft-edged rectangle of colour `c`: `alpha` inside (x0, y0)-(x1, y1), fading out over `feather` pixels. Laid
// behind text that needs a little help to stand out from the scene.
inline void drawSoftRect(SDL_Renderer *r, float x0, float y0, float x1, float y1, float feather, Col c, float alpha) {
  if (alpha < 0.004f) return;
  std::array<SDL_Vertex, 12> v;
  const float grow[3] = {0.0f, feather * 0.45f, feather};
  const float fade[3] = {1.0f, 0.42f, 0.0f};
  for (int ring = 0; ring < 3; ++ring) {
    const float g = grow[ring];
    const SDL_FColor col = fc(c, alpha * fade[ring]);
    v[ring * 4 + 0] = {{x0 - g, y0 - g}, col, {0, 0}};
    v[ring * 4 + 1] = {{x1 + g, y0 - g}, col, {0, 0}};
    v[ring * 4 + 2] = {{x1 + g, y1 + g}, col, {0, 0}};
    v[ring * 4 + 3] = {{x0 - g, y1 + g}, col, {0, 0}};
  }
  std::vector<int> idx{0, 1, 2, 0, 2, 3};
  for (int ring = 0; ring < 2; ++ring) {
    for (int side = 0; side < 4; ++side) {
      const int a = ring * 4 + side, b = ring * 4 + (side + 1) % 4;
      idx.insert(idx.end(), {a, a + 4, b + 4, a, b + 4, b});
    }
  }
  SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

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
    for (std::size_t i = 0; i < cloudTex.size(); ++i)
      cloudTex[i] = makeCloud(r, rng);
    canvas = makeCanvas(r, Screen::width / 2, Screen::height / 2);
    if (!dot || !glow || !disc || !canvas || !cloudTex[0] || !cloudTex[1] || !cloudTex[2]) return false;

    farRidge = makeRidge(rng, Layout::farTop, Layout::farBottom, 1.0f);
    midRidge = makeRidge(rng, Layout::midTop, Layout::midBottom, 0.8f);
    nearRidge = makeRidge(rng, Layout::nearTop, Layout::nearBottom, 0.2f);
    forestRidge = makeRidge(rng, Layout::horizonY - 16.0f, Layout::horizonY - 2.0f, 0.0f);
    shoreRidge = makeRidge(rng, Layout::shoreTop, Layout::shoreBottom, 0.0f);

    for (int i = 0; i < kStars; ++i) {
      stars.push_back({U(rng) * Screen::width, std::pow(U(rng), 1.3f) * 380.0f, 2.5f + U(rng) * U(rng) * 5.0f,
                       0.4f + U(rng) * 0.6f, 0.5f + U(rng) * 2.2f, U(rng) * 6.28f});
    }
    for (int i = 0; i < kClouds; ++i) {
      const float depth = (float)i / kClouds; // small, slow and high first; big, faster and lower later
      clouds.push_back({U(rng), 26.0f + U(rng) * 120.0f + depth * 90.0f, 0.5f + depth * 0.75f + U(rng) * 0.25f,
                        3.0f + depth * 9.0f + U(rng) * 3.0f, 0.75f + U(rng) * 0.25f, (int)(U(rng) * 3) % 3});
    }
    for (int i = 0; i < kScud; ++i)
      scud.push_back({U(rng), Layout::farTop + 10.0f + U(rng) * 60.0f, 1.2f + U(rng) * 0.8f, 10.0f + U(rng) * 8.0f,
                      0.7f + U(rng) * 0.3f, i % 3});
    // The far shore: a forest of small pines along the water, with a town to the right of the middle.
    for (float x = -kMargin; x < Screen::width + kMargin; x += 4.0f + U(rng) * 6.0f) {
      if (x > kTownX0 - 6.0f && x < kTownX1 + 6.0f && U(rng) < 0.8f) continue; // a clearing for the town
      farPines.push_back({x, 6.0f + U(rng) * U(rng) * 20.0f, 2.2f + U(rng) * 2.2f});
    }
    // and a treeline along the forested hills, with more trees scattered down their slopes
    for (float x = -kMargin; x < Screen::width + kMargin; x += 3.0f + U(rng) * 5.0f)
      hillPines.push_back({x, 5.0f + U(rng) * 11.0f, 2.0f + U(rng) * 2.0f});
    for (int row = 0; row < 4; ++row) {
      for (float x = -kMargin; x < Screen::width + kMargin; x += 5.0f + U(rng) * 14.0f) {
        const float drop = 16.0f + row * 22.0f + U(rng) * 14.0f, h = 6.0f + row * 2.0f + U(rng) * 8.0f;
        slopePines.push_back({{x, h, 2.2f + row * 0.6f + U(rng) * 1.8f}, drop, row / 3.0f});
      }
    }
    buildTown(rng);
    for (int i = 0; i < kDrops; ++i) {
      const bool nearDrop = U(rng) < 0.35f;
      drops.push_back({U(rng), U(rng), (nearDrop ? 1050.0f : 760.0f) + U(rng) * 250.0f,
                       (nearDrop ? 26.0f : 14.0f) + U(rng) * 12.0f, nearDrop});
    }
    for (int i = 0; i < kFlakes; ++i) {
      flakes.push_back({U(rng), U(rng), 22.0f + U(rng) * 38.0f, 2.5f + U(rng) * 3.5f, 0.4f + U(rng) * 0.9f,
                        U(rng) * 6.28f, 6.0f + U(rng) * 14.0f});
    }
    for (int i = 0; i < kRipples; ++i)
      ripples.push_back({U(rng), U(rng), 0.6f + U(rng) * 0.8f, U(rng)});
    for (int i = 0; i < kShimmer; ++i)
      shimmer.push_back({U(rng), std::pow(U(rng), 0.8f), 10.0f + U(rng) * 30.0f, 0.2f + U(rng) * 0.6f, U(rng) * 6.28f});
    buildPines(rng);
    buildReeds(rng);
    return true;
  }

  // `secs` is a monotonic clock that drives the motion.
  void Draw(SDL_Renderer *r, const SceneState &s, const Look &l, double secs) {
    const float dt = haveTime ? (float)std::clamp(secs - lastSecs, 0.0, 0.5) : 0.0f;
    haveTime = true;
    lastSecs = secs;
    const float W = Screen::width, H = Screen::height;

    // Parallax: a slow sideways breath of the whole view, felt more by the nearer layers.
    const float cam =
        16.0f * (float)std::sin(secs * 2.0 * M_PI / 240.0) + 5.0f * (float)std::sin(secs * 2.0 * M_PI / 97.0 + 1.0);
    const float dxFar = 0.10f * cam, dxMid = 0.15f * cam, dxNear = 0.20f * cam, dxShore = 0.30f * cam;
    const float dxLand = 0.45f * cam;
    const float dxFore = 1.0f * cam;

    // Wind: which way things drift on screen (looking south, a westerly blows right to left), and how hard.
    float windX = (float)std::sin(s.windDirection * Astro::kDeg);
    if (std::abs(windX) < 0.35f) windX = windX < 0 ? -0.35f : 0.35f;
    const float wind = std::max(0.0f, s.windSpeed);

    // --- sky and horizon glow ---
    for (int i = 0; i <= 24; ++i) {
      const float y = Layout::horizonY * i / 24.0f;
      const SDL_FColor col = fc(skyAt(l, y));
      b.add(0, y, col);
      b.add(W, y, col);
      if (i > 0) b.quad(2 * i - 2, 2 * i - 1, 2 * i + 1, 2 * i);
    }
    b.flush(r, nullptr);
    const ScreenPos sunAt = skyToScreen(s.sunAzimuth, s.sunElevation);
    if (l.glowVis > 0.01f) {
      const float gx = std::clamp(sunAt.x, -160.0f, W + 160.0f);
      b.sprite(gx, Layout::horizonY + 10.0f, 1300.0f, 560.0f, fc(l.glow, l.glowVis));
      b.flush(r, glow.get());
    }

    // --- stars, the odd shooting star, sun and moon ---
    if (l.starVis > 0.01f) {
      for (const Star &st : stars) {
        const float tw = 0.55f + 0.45f * (float)std::sin(secs * st.freq + st.phase);
        const float fadeLow = smooth01((Layout::horizonY - 30.0f - st.y) / 90.0f); // lost in the horizon haze
        b.sprite(st.x, st.y, st.size, st.size, fc({255, 250, 240}, l.starVis * st.bright * tw * fadeLow));
      }
      b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
    }
    DrawMeteor(r, secs, l.starVis);
    if (l.sunVis > 0.01f) {
      b.sprite(sunAt.x, sunAt.y, 340.0f, 340.0f, fc(l.sun, 0.5f * l.sunVis));
      b.flush(r, glow.get(), SDL_BLENDMODE_ADD);
      b.sprite(sunAt.x, sunAt.y, 56.0f, 56.0f, fc(l.sun, l.sunVis));
      b.flush(r, disc.get());
    }
    const ScreenPos moonAt = skyToScreen(s.moonAzimuth, s.moonElevation);
    if (l.moonVis > 0.01f) {
      UpdateMoon(r, s.moonPhase);
      const float lit = 0.5f * (1.0f - std::cos(s.moonPhase * 2.0f * (float)M_PI));
      b.sprite(moonAt.x, moonAt.y, 230.0f, 230.0f, fc({190, 200, 255}, 0.3f * l.moonVis * (0.3f + 0.7f * lit)));
      b.flush(r, glow.get(), SDL_BLENDMODE_ADD);
      b.sprite(moonAt.x, moonAt.y, 46.0f, 46.0f, fc({255, 255, 255}, l.moonVis));
      b.flush(r, moon.get());
    }
    DrawBirds(r, secs, dt, s, l);

    // --- clouds: the deck, then separate clouds drifting with the wind ---
    if (l.overcast > 0.01f) {
      const float ceiling = 300.0f + 70.0f * s.rainIntensity; // heavy rain brings the cloud down
      const Col deck = mix(l.cloudShade, l.cloudLit, 0.35f);
      b.spriteV(W * 0.5f, ceiling * 0.5f, W, ceiling, fc(deck, 0.7f * l.overcast), fc(deck, 0.0f));
      b.flush(r, nullptr);
    }
    const float drift = (0.3f + 0.12f * wind) * windX;
    for (std::size_t i = 0; i < clouds.size(); ++i) {
      Cloud &cl = clouds[i];
      cl.x += cl.speed * drift * dt / (W + 800.0f);
      cl.x -= std::floor(cl.x);
      const float vis = std::clamp(s.cloudiness * kClouds * 1.1f - (float)i, 0.0f, 1.0f);
      if (vis <= 0.0f) continue;
      const float w = 340.0f * cl.scale * (0.85f + 0.45f * s.cloudiness), h = w * 0.4f;
      const float x = cl.x * (W + w + 200.0f) - w * 0.5f - 100.0f + 0.05f * cam;
      const float a = vis * cl.opacity * (0.7f + 0.3f * s.cloudiness) * (1.0f - 0.4f * (1.0f - s.visibility));
      b.spriteV(x, cl.y, w, h, fc(l.cloudLit, a), fc(l.cloudShade, a));
      b.flush(r, cloudTex[cl.variant].get()); // in order: later clouds are nearer
    }

    const float flash = DrawLightning(r, secs, s, l);

    // --- distant mountains, low cloud and mist, nearer mountains ---
    Ridge(farRidge, dxFar, l.far, mix(l.far, l.skyLow, 0.3f), Layout::horizonY);
    b.flush(r, nullptr);
    Ridge(midRidge, dxMid, l.mid, mix(l.mid, l.skyLow, 0.2f), Layout::horizonY);
    b.flush(r, nullptr);
    const float lowCloud = l.overcast * std::max(s.rainIntensity, 1.0f - s.visibility);
    if (lowCloud > 0.02f) {
      for (Cloud &c : scud) {
        c.x += c.speed * drift * dt / (W + 900.0f);
        c.x -= std::floor(c.x);
        const float w = 420.0f * c.scale, x = c.x * (W + w + 300.0f) - w * 0.5f - 150.0f;
        b.spriteV(x, c.y, w, w * 0.34f, fc(l.cloudLit, 0.8f * lowCloud * c.opacity),
                  fc(l.cloudShade, 0.8f * lowCloud * c.opacity));
        b.flush(r, cloudTex[c.variant].get());
      }
    }
    const float mist = std::clamp(0.9f * (1.0f - s.visibility) + 0.1f * s.rainIntensity, 0.0f, 1.0f);
    Fog(r, secs, Layout::nearTop + 10.0f, mist, l.haze, 5.0f * windX);
    Ridge(nearRidge, dxNear, l.near, mix(l.near, l.forest, 0.55f), Layout::horizonY);
    Trees(hillPines, nearRidge, dxNear, l.near);
    for (const SlopePine &p : slopePines) {
      const SDL_FColor col = fc(scale(mix(l.near, l.forest, 0.3f + 0.5f * p.shade), 0.9f));
      const float x = p.tree.x + dxNear, y = RidgeY(nearRidge, p.tree.x) + p.drop;
      const int a = b.add(x - p.tree.w, y, col), e = b.add(x + p.tree.w, y, col);
      b.tri(a, b.add(x, y - p.tree.h, col), e);
    }
    b.flush(r, nullptr);
    Fog(r, secs, Layout::horizonY - 30.0f, mist, l.haze, 8.0f * windX);

    // --- far shore: forest and town ---
    Ridge(forestRidge, dxShore, l.forest, l.forest, Layout::horizonY + 1.0f);
    Trees(farPines, forestRidge, dxShore, l.forest);
    Shapes(walls, dxShore, mix(l.forest, l.skyLow, 0.3f));
    Shapes(roofs, dxShore, scale(l.forest, 0.8f));
    b.flush(r, nullptr);
    DrawWindows(r, secs, l.lightsVis, dxShore);

    // --- the lake ---
    DrawLake(r, secs, s, l, sunAt, moonAt, dxFar, dxMid, dxNear, dxShore, wind);

    // --- near shore, reeds, foreground pines ---
    Ridge(shoreRidge, dxLand, l.ground, mix(l.ground, l.fore, 0.6f), H);
    b.flush(r, nullptr);
    const float sway = 0.004f + 0.0022f * std::pow(std::min(wind, 18.0f), 1.3f);
    DrawReeds(r, secs, l, dxLand, sway, windX);
    DrawPines(r, secs, l, dxFore, sway, windX);
    Fog(r, secs, Layout::lakeBottom, mist * 0.6f, l.haze, 11.0f * windX);

    // --- weather in front of everything ---
    DrawRain(r, secs, s, windX, wind);
    DrawSnow(r, secs, s, windX, wind);
    if (flash > 0.01f) { // the whole land catches the lightning for an instant
      b.sprite(W * 0.5f, H * 0.5f, W, H, fc({200, 196, 255}, 0.16f * flash));
      b.flush(r, nullptr, SDL_BLENDMODE_ADD);
    }

    // Painted-canvas brush strokes over everything but the text.
    const SDL_FRect full{0, 0, W, H};
    SDL_RenderTexture(r, canvas.get(), nullptr, &full);
  }

private:
  static constexpr int kStars = 130, kClouds = 16, kScud = 7, kDrops = 300, kFlakes = 170, kRipples = 40, kShimmer = 46;
  static constexpr int kMargin = 64, kStep = 8; // ridge lines extend past both edges for the parallax
  static constexpr float kTownX0 = 640.0f, kTownX1 = 800.0f;

  struct Star {
    float x, y, size, bright, freq, phase;
  };
  struct Cloud {
    float x, y, scale, speed, opacity; // x: 0..1 across the wrap-around track
    int variant;
  };
  struct FarPine {
    float x, h, w;
  };
  struct Window {
    float x, y, period, seed;
  };
  struct Drop {
    float x0, y0, speed, len;
    bool nearDrop;
  };
  struct Flake {
    float x0, y0, speed, size, freq, phase, amp;
  };
  struct Ripple {
    float x, y, rate, phase;
  };
  struct Glint {
    float x, y, len, speed, phase;
  };
  // A pine as triangles around its foot, with the height of each vertex above it, for the sway.
  struct Pine {
    float x, y, height, depth, freq, phase;
    std::vector<SDL_FPoint> pts;
  };
  struct Reed {
    float x, len, lean, width, freq, phase;
  };
  struct Bird {
    float x, y, flap, phase, size;
  };

  // Vertices and indices for one SDL_RenderGeometry call.
  struct Batch {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;

    int add(float x, float y, SDL_FColor c, float u = 0, float w = 0) {
      v.push_back({{x, y}, c, {u, w}});
      return (int)v.size() - 1;
    }
    void tri(int a, int b, int c) { idx.insert(idx.end(), {a, b, c}); }
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

  TexturePtr dot, glow, disc, moon, canvas;
  std::array<TexturePtr, 3> cloudTex;
  std::vector<float> farRidge, midRidge, nearRidge, forestRidge,
      shoreRidge; // ridge-line y every kStep px from -kMargin
  std::vector<Star> stars;
  std::vector<Cloud> clouds, scud;
  std::vector<FarPine> farPines, hillPines;
  struct SlopePine {
    FarPine tree;
    float drop, shade; // how far below the ridge line it stands; nearer rows are darker
  };
  std::vector<SlopePine> slopePines;
  std::vector<SDL_FPoint> walls, roofs; // the town, as triangles
  std::vector<Window> windows;
  std::vector<Drop> drops;
  std::vector<Flake> flakes;
  std::vector<Ripple> ripples;
  std::vector<Glint> shimmer;
  std::vector<Pine> pines;
  std::vector<Reed> reeds;
  std::vector<Bird> birds;
  Batch b;

  bool haveTime = false;
  double lastSecs = 0;
  int moonKey = -1;
  std::mt19937 events{7u}; // lightning, shooting stars, birds
  double nextStrike = 0, strikeAt = -100, nextMeteor = 20, meteorAt = -100, nextFlock = 40;
  unsigned strikeSeed = 0;
  float meteorX = 0, meteorY = 0, meteorDir = 0, flockDir = 1, flockSpeed = 0;

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

  // The moon's disc at `phase` (0 new, 0.5 full), lit on the right while waxing; the dark part keeps a faint
  // earthshine. Rebaked only when the phase has moved on visibly.
  void UpdateMoon(SDL_Renderer *r, float phase) {
    const int key = (int)(phase * 200.0f);
    if (key == moonKey && moon) return;
    moonKey = key;
    const float k = std::cos(phase * 2.0f * (float)M_PI);
    const bool waxing = phase < 0.5f;
    SurfacePtr s(SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32));
    if (!s) return;
    for (int y = 0; y < 64; ++y) {
      auto *row = (Uint8 *)s->pixels + (std::size_t)y * s->pitch;
      for (int x = 0; x < 64; ++x) {
        const float u = (x + 0.5f - 32.0f) / 30.0f, v = (y + 0.5f - 32.0f) / 30.0f;
        const float disc = std::clamp((1.0f - std::hypot(u, v)) * 30.0f + 0.5f, 0.0f, 1.0f);
        const float edge = std::sqrt(std::max(0.0f, 1.0f - v * v));
        const float lit = waxing ? smooth01((u - k * edge) * 20.0f + 0.5f) : smooth01((-k * edge - u) * 20.0f + 0.5f);
        // A few soft grey seas on the lit face.
        const float seas = 0.1f * smooth01(1.0f - std::hypot(u + 0.25f, v + 0.2f) / 0.35f) +
                           0.08f * smooth01(1.0f - std::hypot(u - 0.2f, v - 0.25f) / 0.3f);
        const float shade = 1.0f - seas;
        row[x * 4 + 0] = (Uint8)(250 * shade);
        row[x * 4 + 1] = (Uint8)(244 * shade);
        row[x * 4 + 2] = (Uint8)(226 * shade);
        row[x * 4 + 3] = (Uint8)(disc * (0.1f + 0.9f * lit) * 255.0f);
      }
    }
    moon.reset(SDL_CreateTextureFromSurface(r, s.get()));
    if (moon) SDL_SetTextureScaleMode(moon.get(), SDL_SCALEMODE_LINEAR);
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
  // brush every hour and every weather.
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

  // A ridge line between `top` and `bottom`: smooth value noise, with folded ("ridged") octaves for mountains as
  // `peaky` goes to 1.
  static std::vector<float> makeRidge(std::mt19937 &rng, float top, float bottom, float peaky) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int n = (Screen::width + 2 * kMargin) / kStep + 1;
    std::vector<float> h(n, 0.0f);
    const float periods[4] = {430.0f, 190.0f, 80.0f, 34.0f};
    const float amps[4] = {1.0f, 0.45f, 0.08f + 0.14f * peaky, 0.02f + 0.07f * peaky};
    for (int o = 0; o < 4; ++o) {
      std::vector<float> lattice((std::size_t)(n * kStep / periods[o]) + 3);
      for (float &v : lattice)
        v = U(rng);
      for (int i = 0; i < n; ++i) {
        const float u = i * kStep / periods[o];
        const int k = (int)u;
        const float f = smooth01(u - k);
        float v = lattice[k] + (lattice[k + 1] - lattice[k]) * f;
        if (o < 2) v += (1.0f - std::abs(2.0f * v - 1.0f) - v) * peaky;
        h[i] += v * amps[o];
      }
    }
    const auto [lo, hi] = std::minmax_element(h.begin(), h.end());
    const float mn = *lo, span = std::max(0.001f, *hi - *lo);
    for (float &v : h)
      v = bottom - (bottom - top) * (v - mn) / span;
    return h;
  }

  static float RidgeY(const std::vector<float> &ys, float x) {
    const float u = std::clamp((x + kMargin) / kStep, 0.0f, (float)ys.size() - 1.001f);
    const int i = (int)u;
    return ys[i] + (ys[i + 1] - ys[i]) * (u - (float)i);
  }

  // Houses with pitched roofs and a church with a spire, as triangles, and the windows that light up at night.
  void buildTown(std::mt19937 &rng) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    auto rect = [&](float x0, float y0, float x1, float y1) {
      walls.insert(walls.end(), {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}});
    };
    auto roof = [&](float x0, float x1, float y, float h) {
      roofs.insert(roofs.end(), {{x0, y}, {x1, y}, {(x0 + x1) / 2, y - h}});
    };
    const float base = Layout::horizonY + 1.0f;
    for (float x = kTownX0; x < kTownX1;) {
      const float w = 9.0f + U(rng) * 9.0f, h = 6.0f + U(rng) * 6.0f;
      if (std::abs(x - 742.0f) < 16.0f) { // the church
        rect(x, base - 13.0f, x + 24.0f, base);
        roof(x - 1.0f, x + 25.0f, base - 13.0f, 6.0f);
        rect(x + 17.0f, base - 30.0f, x + 24.0f, base - 12.0f);
        roof(x + 16.0f, x + 25.0f, base - 30.0f, 16.0f);
        windows.push_back({x + 20.5f, base - 24.0f, 200.0f, U(rng)});
        x += 30.0f;
        continue;
      }
      rect(x, base - h, x + w, base);
      roof(x - 1.0f, x + w + 1.0f, base - h, 3.0f + w * 0.3f);
      for (float wx = x + 3.0f; wx < x + w - 2.0f; wx += 5.0f)
        if (U(rng) < 0.6f) windows.push_back({wx, base - h * (0.35f + 0.3f * U(rng)), 60.0f + U(rng) * 240.0f, U(rng)});
      x += w + 1.0f + U(rng) * 6.0f;
    }
    // A few farms along the rest of the shore.
    for (int i = 0; i < 6; ++i) {
      const float x = 60.0f + U(rng) * 540.0f;
      windows.push_back({x, Layout::horizonY - 3.0f - U(rng) * 6.0f, 90.0f + U(rng) * 300.0f, U(rng)});
    }
  }

  // Pines in the two front corners, tall at the edges and shorter towards the middle, clear of the text.
  void buildPines(std::mt19937 &rng) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    struct Spot {
      float x, h;
    };
    const Spot left[] = {{-26, 420}, {34, 340}, {-2, 250}, {92, 268}, {142, 196}, {188, 132}, {60, 170}};
    const Spot right[] = {{1052, 440}, {990, 350}, {1026, 262}, {934, 280}, {884, 200}, {842, 138}, {960, 172}};
    auto make = [&](float x, float h) {
      Pine p;
      p.x = x + (U(rng) - 0.5f) * 10.0f;
      p.height = h * (0.94f + 0.12f * U(rng));
      p.y = Screen::height + 14.0f + U(rng) * 16.0f;
      p.depth = std::clamp((440.0f - h) / 320.0f, 0.0f, 1.0f); // shorter trees stand further back, a little paler
      p.freq = 0.35f + U(rng) * 0.3f;
      p.phase = U(rng) * 6.28f;
      const float H = p.height, halfBase = H * (0.17f + 0.04f * U(rng));
      auto tri = [&](SDL_FPoint a, SDL_FPoint b, SDL_FPoint c) { p.pts.insert(p.pts.end(), {a, b, c}); };
      // trunk
      tri({-H * 0.018f, 0}, {H * 0.018f, 0}, {H * 0.012f, -H * 0.3f});
      tri({-H * 0.018f, 0}, {H * 0.012f, -H * 0.3f}, {-H * 0.012f, -H * 0.3f});
      // tiers of drooping boughs, narrowing to the top
      const int tiers = 8 + (int)(U(rng) * 4);
      for (int i = 0; i < tiers; ++i) {
        const float t = (float)i / tiers;
        const float yb = -H * (0.1f + 0.82f * t), yt = yb - H * (0.2f - 0.08f * t);
        const float wl = halfBase * std::pow(1.0f - t, 0.85f) * (0.8f + 0.4f * U(rng)) + 3.0f;
        const float wr = halfBase * std::pow(1.0f - t, 0.85f) * (0.8f + 0.4f * U(rng)) + 3.0f;
        const float droop = H * 0.025f * (1.0f - t);
        tri({-wl, yb + droop}, {0, yt}, {wr, yb + droop});
        tri({-wl * 0.7f, yb + droop * 0.6f}, {-wl * 0.35f, yb - H * 0.03f}, {0, yb + H * 0.01f}); // ragged underside
        tri({wr * 0.7f, yb + droop * 0.6f}, {wr * 0.35f, yb - H * 0.03f}, {0, yb + H * 0.01f});
      }
      tri({-H * 0.012f, -H * 0.9f}, {0, -H}, {H * 0.012f, -H * 0.9f}); // the leader
      pines.push_back(std::move(p));
    };
    // Back to front: the shorter, further trees first.
    std::vector<Spot> all(std::begin(left), std::end(left));
    all.insert(all.end(), std::begin(right), std::end(right));
    std::sort(all.begin(), all.end(), [](const Spot &a, const Spot &b) { return a.h < b.h; });
    for (const Spot &s : all)
      make(s.x, s.h);
  }

  void buildReeds(std::mt19937 &rng) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    for (float x = -kMargin; x < Screen::width + kMargin; x += 3.0f + U(rng) * 9.0f) {
      const float side = std::abs(x - Screen::width * 0.5f) / (Screen::width * 0.5f); // thicker at the sides
      if (U(rng) > 0.25f + 0.75f * side) continue;
      reeds.push_back({x, 8.0f + U(rng) * 16.0f * (0.5f + side), (U(rng) - 0.5f) * 0.5f, 1.4f + U(rng) * 1.4f,
                       0.8f + U(rng) * 0.8f, U(rng) * 6.28f});
    }
  }

  // ------------------------------------------------------------ drawing --

  // The body of a layer with a vertical gradient from its ridge line down to `bottomY`, plus a one-pixel feather
  // along the ridge that stands in for anti-aliasing.
  void Ridge(const std::vector<float> &ys, float dx, Col top, Col bottom, float bottomY) {
    const SDL_FColor ct = fc(top), cb = fc(bottom), clear = fc(top, 0.0f);
    const int first = (int)b.v.size();
    for (std::size_t i = 0; i < ys.size(); ++i) {
      const float x = -kMargin + (float)i * kStep + dx, y = ys[i];
      b.add(x, y, ct);
      b.add(x, bottomY, cb);
      b.add(x, y - 1.2f, clear);
      if (i > 0) {
        const int k = first + 3 * (int)i;
        b.quad(k - 3, k, k + 1, k - 2);
        b.quad(k - 1, k + 2, k, k - 3);
      }
    }
  }

  // Little pines standing on a ridge line.
  void Trees(const std::vector<FarPine> &trees, const std::vector<float> &ridge, float dx, Col c) {
    const SDL_FColor col = fc(c);
    for (const FarPine &p : trees) {
      const float x = p.x + dx, y = RidgeY(ridge, p.x) + 1.5f;
      const int a = b.add(x - p.w, y, col), e = b.add(x + p.w, y, col);
      b.tri(a, b.add(x, y - p.h, col), e);
    }
  }

  void Shapes(const std::vector<SDL_FPoint> &tris, float dx, Col c) {
    const SDL_FColor col = fc(c);
    for (const SDL_FPoint &pt : tris)
      b.add(pt.x + dx, pt.y, col);
    for (int i = (int)b.v.size() - (int)tris.size(); i + 2 < (int)b.v.size(); i += 3)
      b.tri(i, i + 1, i + 2);
  }

  // A ridge mirrored in the water about `axisY`, squashed by `squash`, fading out with depth.
  void Reflection(const std::vector<float> &ys, float dx, float axisY, float squash, Col c, float a0, float a1) {
    const int first = (int)b.v.size();
    for (std::size_t i = 0; i < ys.size(); ++i) {
      const float x = -kMargin + (float)i * kStep + dx;
      const float depth = std::max(0.0f, axisY - ys[i]) * squash;
      b.add(x, axisY, fc(c, a0));
      b.add(x, axisY + depth, fc(c, a0 + (a1 - a0) * std::min(1.0f, depth / 60.0f)));
      if (i > 0) {
        const int k = first + 2 * (int)i;
        b.quad(k - 2, k, k + 1, k - 1);
      }
    }
  }

  void Fog(SDL_Renderer *r, double secs, float y, float vis, Col col, float speed) {
    if (vis < 0.01f) return;
    constexpr float w = 760.0f, period = w * 1.6f;
    const float off = (float)std::fmod(secs * speed, (double)period) + (speed < 0 ? period : 0.0f);
    for (float x = off - period; x < Screen::width + period; x += period * 0.5f)
      b.sprite(x, y, w, 120.0f, fc(col, 0.6f * vis));
    b.flush(r, glow.get());
  }

  // Lit windows in the town and the farms: each switches on and off now and then.
  bool WindowOn(const Window &w, double secs) const {
    const double slot = std::floor(secs / w.period + w.seed * 7.0);
    const float h = (float)std::fmod(std::sin(slot * 12.9898 + w.seed * 78.233) * 43758.5453, 1.0);
    return std::abs(h) < 0.82f;
  }

  void DrawWindows(SDL_Renderer *r, double secs, float vis, float dx) {
    if (vis < 0.01f) return;
    for (const Window &w : windows)
      if (WindowOn(w, secs)) b.sprite(w.x + dx, w.y, 12.0f, 12.0f, fc({255, 180, 100}, 0.4f * vis));
    b.flush(r, glow.get(), SDL_BLENDMODE_ADD);
    for (const Window &w : windows)
      if (WindowOn(w, secs)) b.sprite(w.x + dx, w.y, 3.0f, 3.0f, fc({255, 224, 160}, vis));
    b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
  }

  void DrawLake(SDL_Renderer *r, double secs, const SceneState &s, const Look &l, ScreenPos sunAt, ScreenPos moonAt,
                float dxFar, float dxMid, float dxNear, float dxShore, float wind) {
    const float W = Screen::width, top = Layout::horizonY, bottom = Layout::shoreBottom + 4.0f;
    // The water mirrors the sky: the horizon colour at the far shore, higher sky nearer.
    const Col waterTop = waterColour(l);
    const Col waterLow = scale(mix(mix(l.skyMid, l.skyTop, 0.5f), l.near, 0.3f), 0.72f);
    b.spriteV(W * 0.5f, (top + bottom) * 0.5f, W, bottom - top, fc(waterTop), fc(waterLow));
    b.flush(r, nullptr);
    // Mountains and the far shore, upside down; a stirred-up lake (wind, rain) blurs them away.
    const float calm = std::clamp(1.0f - 0.05f * wind - 0.5f * s.rainIntensity, 0.3f, 1.0f);
    Reflection(farRidge, dxFar, top, 0.3f, mix(l.far, waterTop, 0.35f), 0.75f * calm, 0.0f);
    Reflection(midRidge, dxMid, top, 0.3f, mix(l.mid, waterTop, 0.3f), 0.8f * calm, 0.05f * calm);
    Reflection(nearRidge, dxNear, top, 0.3f, mix(l.near, waterTop, 0.25f), 0.85f * calm, 0.1f * calm);
    Reflection(forestRidge, dxShore, top, 1.6f, scale(l.forest, 0.9f), 0.9f * calm, 0.2f * calm);
    b.flush(r, nullptr);

    // A glitter path under the sun or the moon when it is low enough to catch.
    auto path = [&](ScreenPos at, float vis, Col c, float width) {
      if (vis < 0.02f || at.y > top + 10.0f) return;
      const float lowness = smooth01((top - at.y) > 330.0f ? 0.0f : 1.0f - (top - at.y) / 330.0f);
      if (lowness <= 0.0f) return;
      for (int i = 0; i < 18; ++i) {
        const float t = (i + 0.5f) / 18.0f, y = top + 2.0f + t * (bottom - top - 6.0f);
        const float wob = (float)std::sin(secs * (1.3 + 0.21 * i) + i * 1.7);
        const float w = width * (0.35f + 0.9f * t) * (0.7f + 0.3f * wob);
        const float a = vis * lowness * (0.55f - 0.35f * t) * (0.6f + 0.4f * (float)std::sin(secs * 2.1 + i * 2.3));
        b.sprite(at.x + wob * 3.0f * t, y, w, 2.2f + 2.0f * t, fc(c, a));
      }
      b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
    };
    path(sunAt, l.sunVis, l.sun, 90.0f);
    path(moonAt, l.moonVis, {230, 232, 255}, 40.0f);
    // Lit windows smear down into the water.
    if (l.lightsVis > 0.01f) {
      for (const Window &w : windows) {
        if (!WindowOn(w, secs)) continue;
        const float x = w.x + dxShore + 1.2f * (float)std::sin(secs * 1.7 + w.seed * 20.0);
        b.segment(x, top + 3.0f, x, top + 14.0f + w.seed * 8.0f, 2.0f, fc({255, 200, 130}, 0.35f * l.lightsVis * calm),
                  fc({255, 200, 130}, 0.0f));
      }
      b.flush(r, nullptr, SDL_BLENDMODE_ADD);
    }
    // Shimmer: slow light ripples catching the sky.
    const float light = std::clamp(relativeLuminance(l.skyLow) * 1.6f + 0.15f, 0.0f, 1.0f);
    const Col sheen = mix(l.skyLow, Col{255, 255, 255}, 0.5f);
    for (const Glint &g : shimmer) {
      const float y = top + 4.0f + g.y * (bottom - top - 8.0f);
      const float x = (float)std::fmod(g.x * (W + 120.0) + secs * g.speed * (1.0f + 0.2f * wind), W + 120.0) - 60.0f;
      const float a = light * 0.22f * (0.5f + 0.5f * (float)std::sin(secs * 0.9 + g.phase));
      b.sprite(x, y, g.len * (0.6f + g.y), 1.6f + g.y * 1.2f, fc(sheen, a));
    }
    b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
    // Rings where the rain lands.
    const int n = (int)(s.rainIntensity * kRipples);
    for (int i = 0; i < n; ++i) {
      const Ripple &rp = ripples[i];
      const float t = (float)std::fmod(secs * rp.rate + rp.phase, 1.0);
      const double cycle = std::floor(secs * rp.rate + rp.phase);
      const float jx = (float)std::fmod(std::abs(std::sin(cycle * 91.7 + i * 3.1)) * 1000.0, 1.0);
      const float y = top + 6.0f + rp.y * (bottom - top - 10.0f), x = std::fmod(rp.x + jx, 1.0f) * W;
      const float size = 3.0f + t * (8.0f + 10.0f * rp.y);
      b.sprite(x, y, size * 2.0f, size * 0.5f, fc(sheen, 0.35f * (1.0f - t)));
    }
    b.flush(r, dot.get(), SDL_BLENDMODE_ADD);
  }

  void DrawReeds(SDL_Renderer *r, double secs, const Look &l, float dx, float sway, float windX) {
    const Col c = mix(l.ground, l.fore, 0.5f);
    for (const Reed &rd : reeds) {
      const float x = rd.x + dx, y = RidgeY(shoreRidge, rd.x) + 3.0f;
      const float bend =
          rd.lean * 0.3f + windX * sway * 4.0f + sway * 5.0f * (float)std::sin(secs * rd.freq + rd.phase);
      const int a = b.add(x - rd.width, y, fc(c)), bb = b.add(x + rd.width, y, fc(c));
      b.tri(a, bb, b.add(x + bend * rd.len, y - rd.len, fc(c)));
    }
    b.flush(r, nullptr);
  }

  void DrawPines(SDL_Renderer *r, double secs, const Look &l, float dx, float sway, float windX) {
    for (const Pine &p : pines) {
      const SDL_FColor c = fc(mix(l.fore, mix(l.ground, l.haze, 0.15f), 0.35f * p.depth));
      // The crown bends downwind and rocks; the bend grows with the square of the height.
      const float rock =
          sway * (float)(std::sin(secs * p.freq + p.phase) + 0.35 * std::sin(secs * p.freq * 2.3 + p.phase));
      const float lean = windX * sway * 0.6f;
      const int first = (int)b.v.size();
      for (const SDL_FPoint &pt : p.pts) {
        const float h = -pt.y / p.height;
        b.add(p.x + dx + pt.x + (lean + rock) * p.height * h * h, p.y + pt.y, c);
      }
      for (int i = first; i < (int)b.v.size(); i += 3)
        b.tri(i, i + 1, i + 2);
    }
    b.flush(r, nullptr);
  }

  void DrawRain(SDL_Renderer *r, double secs, const SceneState &s, float windX, float wind) {
    const int n = (int)(s.rainIntensity * kDrops);
    if (n <= 0) return;
    const float W = Screen::width, span = Screen::height + 80.0f;
    const float slant = -windX * std::clamp(0.04f + wind * 0.035f, 0.0f, 0.6f);
    const float norm = std::sqrt(1.0f + slant * slant);
    const Col col = s.sceneBrightness < 0.2f ? Col{186, 206, 240} : Col{236, 242, 255};
    const float strength = 0.55f + 0.45f * s.rainIntensity;
    for (int i = 0; i < n; ++i) {
      const Drop &d = drops[i];
      const float y = (float)std::fmod(d.y0 * span + d.speed * secs, (double)span) - 40.0f;
      const float x = std::fmod(d.x0 * (W + 400.0f) + slant * y + 800.0f, W + 400.0f) - 200.0f;
      const float len = d.len * (0.8f + 0.4f * s.rainIntensity);
      const float a = (d.nearDrop ? 0.45f : 0.28f) * strength;
      b.segment(x - slant / norm * len, y - len / norm, x, y, d.nearDrop ? 1.7f : 1.1f, fc(col, 0.0f), fc(col, a));
    }
    b.flush(r, nullptr);
  }

  void DrawSnow(SDL_Renderer *r, double secs, const SceneState &s, float windX, float wind) {
    const int n = (int)(s.snowIntensity * kFlakes);
    if (n <= 0) return;
    const float W = Screen::width, span = Screen::height + 30.0f;
    for (int i = 0; i < n; ++i) {
      const Flake &f = flakes[i];
      const float y = (float)std::fmod(f.y0 * span + f.speed * secs, (double)span) - 15.0f;
      const float x = std::fmod(f.x0 * (W + 200.0f) + f.amp * (float)std::sin(secs * f.freq + f.phase) -
                                    windX * y * wind * 0.03f + 400.0f,
                                W + 200.0f) -
                      100.0f;
      b.sprite(x, y, f.size * 2.0f, f.size * 2.0f, fc({255, 255, 255}, 0.9f));
    }
    b.flush(r, dot.get());
  }

  // Now and then, on a clear day, a few birds cross the sky.
  void DrawBirds(SDL_Renderer *r, double secs, float dt, const SceneState &s, const Look &l) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    if (secs >= nextFlock) {
      nextFlock = secs + 120.0 + U(events) * 240.0;
      const bool fair = s.sunElevation > 4.0f && s.cloudiness < 0.7f && s.rainIntensity < 0.05f &&
                        s.snowIntensity < 0.05f && s.visibility > 0.6f;
      if (fair && birds.empty()) {
        flockDir = U(events) < 0.5f ? -1.0f : 1.0f;
        flockSpeed = 28.0f + U(events) * 16.0f;
        const float y0 = 70.0f + U(events) * 60.0f;
        const int count = 3 + (int)(U(events) * 5);
        for (int i = 0; i < count; ++i)
          birds.push_back({(flockDir > 0 ? -30.0f : Screen::width + 30.0f) - flockDir * (i * 16.0f + U(events) * 12.0f),
                           y0 + (i % 2 ? 1.0f : -1.0f) * i * 5.0f + U(events) * 6.0f, 3.2f + U(events) * 1.2f,
                           U(events) * 6.28f, 4.0f + U(events) * 2.5f});
      }
    }
    if (birds.empty()) return;
    const SDL_FColor c = fc(scale(mix(l.skyTop, l.far, 0.4f), 0.35f), 0.85f);
    bool anyOn = false;
    for (Bird &bd : birds) {
      bd.x += flockDir * flockSpeed * dt;
      bd.y += 1.5f * (float)std::sin(secs * 0.7 + bd.phase) * dt;
      anyOn = anyOn || (bd.x > -60.0f && bd.x < Screen::width + 60.0f);
      const float f = (float)std::sin(secs * bd.flap * 2.0 + bd.phase);
      const float wy = bd.size * (0.15f + 0.45f * f);
      b.segment(bd.x - bd.size, bd.y - wy, bd.x, bd.y, 1.3f, c, c);
      b.segment(bd.x, bd.y, bd.x + bd.size, bd.y - wy, 1.3f, c, c);
    }
    b.flush(r, nullptr);
    if (!anyOn) birds.clear();
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

  // Returns how bright the flash is, so the land can catch it too.
  float DrawLightning(SDL_Renderer *r, double secs, const SceneState &s, const Look &l) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    if (s.storm > 0.5f && secs >= nextStrike) {
      if (nextStrike > 0) {
        strikeAt = secs;
        strikeSeed = events();
      }
      nextStrike = secs + 6.0 + U(events) * 14.0;
    }
    const float t = (float)(secs - strikeAt);
    const float f = flashAt(t) * s.storm;
    if (f < 0.01f) return 0.0f;

    // The sky lights up behind the mountains, which stay dark silhouettes.
    const Col sky = mix(l.cloudLit, {235, 225, 255}, 0.6f);
    b.spriteV(Screen::width * 0.5f, Layout::horizonY * 0.5f, Screen::width, Layout::horizonY, fc(sky, 0.3f * f),
              fc(sky, 0.12f * f));
    b.flush(r, nullptr, SDL_BLENDMODE_ADD);
    if (t > 0.5f) return f;

    // A jagged bolt with one branch, reproducible from its seed for the whole strike, off to one side of the clock.
    std::mt19937 bolt(strikeSeed);
    const bool right = U(bolt) < 0.6f;
    float x = right ? 870.0f + U(bolt) * 110.0f : 40.0f + U(bolt) * 110.0f, y = -10.0f;
    const float yEnd = Layout::farTop + 20.0f + U(bolt) * 50.0f;
    const int steps = 11, branchAt = 3 + (int)(U(bolt) * 4);
    const SDL_FColor core = fc({245, 240, 255}, f), halo = fc({190, 170, 255}, 0.3f * f);
    for (int i = 0; i < steps; ++i) {
      const float nx = x + (U(bolt) - 0.5f) * 40.0f, ny = y + (yEnd + 10.0f) / steps;
      b.segment(x, y, nx, ny, 8.0f, halo, halo);
      b.segment(x, y, nx, ny, 2.4f, core, core);
      if (i == branchAt) {
        float bx = nx, by = ny;
        const float dir = right ? -1.0f : 1.0f;
        for (int j = 0; j < 4; ++j) {
          const float ex = bx + dir * (8.0f + U(bolt) * 18.0f), ey = by + 16.0f + U(bolt) * 10.0f;
          b.segment(bx, by, ex, ey, 1.4f, fc({245, 240, 255}, 0.7f * f), fc({245, 240, 255}, 0.7f * f));
          bx = ex, by = ey;
        }
      }
      x = nx, y = ny;
    }
    b.flush(r, nullptr, SDL_BLENDMODE_ADD);
    return f;
  }
};
