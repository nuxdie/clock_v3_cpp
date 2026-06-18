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
#include <ctime>
#include <execution>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
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
constexpr int num_snowflakes = 666;
constexpr const char *AppName = "Digital Clock v3";
constexpr const char *AppVersion = "0.2.1";

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

std::string getCurrentTime() {
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  return std::format("{}:{:02}", tm.tm_hour, tm.tm_min);
}

std::string getCurrentDate() {
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  return std::format("{}, {} {} {} года", weekdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
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

    if (!TTF_Init()) {
      SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL_ttf: %s", SDL_GetError());
      return false;
    }
    fontNormal.reset(TTF_OpenFontIO(SDL_IOFromConstMem(BellotaText_Bold_ttf, BellotaText_Bold_ttf_len), true,
                                    Config::font_normal_size));
    fontBig.reset(TTF_OpenFontIO(SDL_IOFromConstMem(BellotaText_Bold_ttf, BellotaText_Bold_ttf_len), true,
                                 Config::font_big_size));
    fontSmall.reset(TTF_OpenFontIO(SDL_IOFromConstMem(BellotaText_Bold_ttf, BellotaText_Bold_ttf_len), true,
                                   Config::font_small_size));
    if (!fontNormal || !fontBig || !fontSmall) {
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

    // Start Data Threads
    bgLoaderThread = std::jthread(&Clock::FetchBackgroundImage, this);
    weatherLoaderThread = std::jthread(&Clock::FetchWeather, this);

    lastPerformanceCounter = SDL_GetPerformanceCounter();

    return true;
  }

  SDL_AppResult Iterate() {
    UpdateTiming();
    // snow.Update(deltaTime);
    UpdateTextures();
    Render();
    return SDL_APP_CONTINUE;
  }

private:
  WindowPtr window;
  RendererPtr renderer;
  FontPtr fontBig;
  FontPtr fontNormal;
  FontPtr fontSmall;

  // SnowSystem snow;

  // Background Image
  std::jthread bgLoaderThread;
  std::mutex bgImageLoaderMutex;
  std::string lastLoadedUrl;
  SurfacePtr pendingBgImage;
  TexturePtr bgTexture;

  // Weather Data
  std::jthread weatherLoaderThread;
  std::mutex weatherMutex;
  std::string weatherString;

  // Clothing Advice (LLM)
  std::mutex adviceMutex;
  std::string adviceString;

  Uint64 lastPerformanceCounter = 0;
  double fps = 0.0;
  double deltaTime = 0.0;

  struct TextLabel {
    std::string text;
    TexturePtr texture;
    SDL_FRect rect;
    // Store last wrap width to detect changes needed if window resizes (though fixed logical size simplifies this)
    int lastWrapWidth = 0;

    // Layout function to position the text label within the window
    using LayoutFunc = std::function<SDL_FRect(float w, float h)>;

    void update(SDL_Renderer *renderer, TTF_Font *font, std::string_view newText, SDL_Color color, LayoutFunc layout,
                int wrapWidth = 0) {
      if (text == newText && texture && wrapWidth == lastWrapWidth) return;
      if (newText.empty()) {
        text.clear();
        lastWrapWidth = wrapWidth;
        texture.reset();
        rect = {};
        return;
      }
      text = newText;
      lastWrapWidth = wrapWidth;

      SurfacePtr surf;
      if (wrapWidth > 0) {
        surf.reset(TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, color, wrapWidth));
      } else {
        surf.reset(TTF_RenderText_Blended(font, text.c_str(), 0, color));
      }

      if (surf) {
        texture.reset(SDL_CreateTextureFromSurface(renderer, surf.get()));
        rect = layout((float)surf->w, (float)surf->h);
      }
    }

    void draw(SDL_Renderer *renderer) const {
      if (!texture) return;
      // Shadow
      SDL_SetTextureColorMod(texture.get(), 0, 0, 0);
      SDL_SetTextureAlphaMod(texture.get(), 128);
      SDL_FRect shadow = rect;
      shadow.x += 1.0f;
      shadow.y += 1.0f;
      SDL_RenderTexture(renderer, texture.get(), nullptr, &shadow);
      // Text
      SDL_SetTextureColorMod(texture.get(), 255, 255, 255);
      SDL_SetTextureAlphaMod(texture.get(), 255);
      SDL_RenderTexture(renderer, texture.get(), nullptr, &rect);
    }
  };

  TextLabel timeLabel;
  TextLabel dateLabel;
  TextLabel weatherLabel;
  TextLabel adviceLabel;

  void FetchBackgroundImage(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
      try {
        cpr::Response response = cpr::Get(cpr::Url{"https://peapix.com/bing/feed?country=us"});
        if (response.status_code == 200) {
          auto response_json = json::parse(response.text);
          const auto &images = response_json.get<std::vector<BingImage>>();
          if (!images.empty()) {
            // TODO: instead of grabbing the first image, grab the image with today's date
            std::string imgUrl = images[0].fullUrl;
            if (imgUrl != lastLoadedUrl) {
              cpr::Response imgResp = cpr::Get(cpr::Url{imgUrl}, cpr::ReserveSize{2 * 1024 * 1024});
              if (imgResp.status_code == 200) {
                SDL_IOStream *io = SDL_IOFromConstMem(imgResp.text.data(), imgResp.text.size());
                SurfacePtr loadedSurf(IMG_Load_IO(io, true));
                if (loadedSurf) {
                  std::lock_guard lock(bgImageLoaderMutex);
                  pendingBgImage = std::move(loadedSurf);
                  lastLoadedUrl = imgUrl;
                }
              }
            }
          }
        }
      } catch (const std::exception &e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Background image fetch failed: %s", e.what());
      }
      std::mutex sleepMutex;             // We use a dummy mutex because the condition_variable's wait_for requires
      std::unique_lock lock(sleepMutex); // a lock to wait on
      std::condition_variable_any().wait_for(lock, stopToken, std::chrono::hours(4),
                                             [&stopToken] { return stopToken.stop_requested(); });
    }
  }

  void FetchWeather(std::stop_token stopToken) {
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
        cpr::Response response = cpr::Get(url, params, cpr::Timeout{60000});
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
                cpr::Timeout{60000});
            if (r.status_code == 200) {
              auto llmResp = json::parse(r.text).get<LlmResponse>();
              if (!llmResp.choices.empty()) {
                finalAdvice = llmResp.choices[0].message.content;
                if (finalAdvice.size() > 1 && finalAdvice.front() == '"' && finalAdvice.back() == '"') {
                  finalAdvice = finalAdvice.substr(1, finalAdvice.size() - 2);
                }
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

      std::mutex sleepMutex;
      std::unique_lock lock(sleepMutex);
      std::condition_variable_any().wait_for(lock, stopToken,
                                             std::chrono::minutes(5), // Fetch weather every 5 minutes
                                             [&stopToken] { return stopToken.stop_requested(); });
    }
  }

  void UpdateTiming() {
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 diff = now - lastPerformanceCounter;
    lastPerformanceCounter = now;
    deltaTime = (double)diff / (double)SDL_GetPerformanceFrequency();
    fps = 1.0 / deltaTime;

    SDL_Delay(16); // Wait for 16ms to maintain 60 FPS
  }

  void UpdateTextures() {
    SDL_Color white = {255, 255, 255, SDL_ALPHA_OPAQUE};
    { // Update Background Image
      std::lock_guard lock(bgImageLoaderMutex);
      if (pendingBgImage) {
        bgTexture.reset(SDL_CreateTextureFromSurface(renderer.get(), pendingBgImage.get()));
        pendingBgImage.reset();
      }
    }
    // Update Date
    dateLabel.update(renderer.get(), fontNormal.get(), getCurrentDate(), white,
                     [](float w, float h) { return SDL_FRect{(Config::screen_width - w) / 2.0f, 60.0f, w, h}; });
    // Update Time
    timeLabel.update(renderer.get(), fontBig.get(), getCurrentTime(), white, [](float w, float h) {
      return SDL_FRect{(Config::screen_width - w) / 2.0f, (Config::screen_height - h) / 2.0f - 20.0f, w, h};
    });

    // Update Weather
    std::string currentW;
    {
      std::lock_guard lock(weatherMutex);
      currentW = weatherString;
    }
    weatherLabel.update(renderer.get(), fontNormal.get(), currentW, white, [&](float w, float h) {
      float timeBottom = timeLabel.rect.y + timeLabel.rect.h;
      // If time texture isn't ready yet, guess a position, otherwise use relative
      float yPos = (timeBottom > 0) ? timeBottom - 80.0f : (Config::screen_height / 2.0f + 140.0f);
      return SDL_FRect{(Config::screen_width - w) / 2.0f, yPos, w, h};
    });

    std::string currentAdvice;
    {
      std::lock_guard lock(adviceMutex);
      currentAdvice = adviceString;
    }
    int wrapW = static_cast<int>(Config::screen_width * 0.95f);
    adviceLabel.update(
        renderer.get(), fontSmall.get(), currentAdvice, white,
        [&](float w, float h) {
          float weatherBottom = weatherLabel.rect.y + weatherLabel.rect.h;
          float yPos = weatherBottom + 10.0f; // 10px padding
          return SDL_FRect{(Config::screen_width - w) / 2.0f, yPos, w, h};
        },
        wrapW);
  }

  void Render() {
    SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer.get());

    if (bgTexture) {
      SDL_SetTextureColorMod(bgTexture.get(), 200, 200, 200);
      RenderTextureCover(bgTexture.get());
    }
    // snow.Draw(renderer.get());
    dateLabel.draw(renderer.get());
    timeLabel.draw(renderer.get());
    weatherLabel.draw(renderer.get());
    adviceLabel.draw(renderer.get());

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
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  auto *app = static_cast<Clock *>(appstate);
  return app->Iterate();
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  auto *app = static_cast<Clock *>(appstate);
  delete app;
  TTF_Quit();
}
