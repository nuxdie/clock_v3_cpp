# Background Image API

https://peapix.com/bing/feed?country=us

would return JSON, that looks like:

```json
[
  {
    "title": "Leshan Giant Buddha, Sichuan, China",
    "copyright": "\\u00A9 www.anotherdayattheoffice.org/Getty Image",
    "fullUrl": "https://img.peapix.com/3da559556ac64726a87fb1c879b59f46_1920.jpg",
    "thumbUrl": "https://img.peapix.com/3da559556ac64726a87fb1c879b59f46_640.jpg",
    "imageUrl": "https://img.peapix.com/3da559556ac64726a87fb1c879b59f46.jpg",
    "pageUrl": "https://peapix.com/bing/54032",
    "date": "2025-11-22"
  },
  ...
  {
    "title": "A traboule in Lyon, France",
    "copyright": "\\u00A9 TPopova/Getty Image",
    "fullUrl": "https://img.peapix.com/f3952eca8c40478585af02b480fc9547_1920.jpg",
    "thumbUrl": "https://img.peapix.com/f3952eca8c40478585af02b480fc9547_640.jpg",
    "imageUrl": "https://img.peapix.com/f3952eca8c40478585af02b480fc9547.jpg",
    "pageUrl": "https://peapix.com/bing/53960",
    "date": "2025-11-16"
  }
]
```

There's a mock server for this API in `./bing-feed-local-mock-server`. Use it for development.

# Weather API

https://api.open-meteo.com/v1/forecast?latitude=52.3738&longitude=4.8910&current_weather=true&windspeed_unit=ms&timezone=auto

would return JSON, that looks like:

```json
{
  "latitude": 52.366,
  "longitude": 4.901,
  "generationtime_ms": 2.517104148864746,
  "utc_offset_seconds": 3600,
  "timezone": "Europe/Amsterdam",
  "timezone_abbreviation": "GMT+1",
  "elevation": 17.0,
  "current_weather_units": {
    "time": "iso8601",
    "interval": "seconds",
    "temperature": "°C",
    "windspeed": "m/s",
    "winddirection": "°",
    "is_day": "",
    "weathercode": "wmo code"
  },
  "current_weather": {
    "time": "2025-12-06T03:30",
    "interval": 900,
    "temperature": 4.9,
    "windspeed": 7.80,
    "winddirection": 159,
    "is_day": 0,
    "weathercode": 53
  }
}
```

# Clothing advice via Groq API

Set `GROQ_API_KEY` env variable.

POST https://api.groq.com/openai/v1/chat/completions with json payload:

```json
{
    "model": "gpt-oss-120b",
    "max_tokens": 300,
    "temperature": 0.7,
    "messages": [
      {
        "role": "system",
        "content": "You are a helpful assistant providing concise clothing advice."
      },
      {
        "role": "user",
        "content": "I live in Amsterdam. Today is <day> <month>, the time is <time> and the weather is: <weather_description>. What should I wear? Please answer in one short sentence, in russian. Only say what clothes I should wear, there's no need to mention city, current weather or time and date. Basically, just continue the phrase: You should wear..., without saying the 'you should wear' part."
      }
    ]
}
```

and Auth Bearer <auth_token> header. It would return a JSON object with the following structure:

```json
{
    "id": "chatcmpl-...",
    "object": "chat.completion",
    "created": 1677652288,
    "model": "gpt-oss-120b",
    "choices": [
      {
        "index": 0,
        "message": {
          "role": "assistant",
          "content": "You should wear a light jacket and jeans."
        },
        "finish_reason": "stop"
      }
    ],
    "usage": {
      "prompt_tokens": 13,
      "completion_tokens": 12,
      "total_tokens": 25
    }
}
```

# clangd LSP Integration

To get your editor pick up on dependencies headers, compile your project once in debug mode.
Then run `ln -s build/debug/compile_commands.json` in the root of your project and restart clangd.

# Living background

One landscape, drawn in depth layers, whose look is computed each frame from a handful of numbers rather than picked
from a set of backgrounds:

```text
time + place ──► sun and moon (astro.h) ─┐
                                         ├─► SceneState ──► Look ──► Scene (scene.h) draws the layers
forecast ─────► clouds, rain, fog, wind ─┘   (scene_state.h)  └────► TextTheme: readable ink over it
```

- `astro.h` places the sun and the moon (elevation, azimuth, phase) for `Config::latitude`/`longitude` with the
  low-precision Astronomical Almanac formulas. Sunrise, sunset and twilight follow from the sun's elevation for any
  date, with no network needed.
- `SceneState` holds the time fields (sun and moon position, morning or evening) and the weather fields (cloudiness,
  rain, snow, storm, visibility, wind speed and direction, and slower wetness and snow cover). `applySky` and
  `applyWeather` (from the WMO code and the 15-minute rain forecast) set a target; `easeScene` eases the weather
  fields towards it, so a new forecast rolls in over a few seconds, the ground dries over a quarter of an hour and
  snow settles over ten minutes.
- `lookFor` turns the state into colours. The sky, land and cloud colours are keyframed by the sun's elevation
  (night −18°, dawn/dusk −10° and −4°, sunrise/sunset 0°, 6°, 15°, day 30°), with a morning set (pink, peach, cool
  blue) and an evening set (gold, coral, violet) blended by which side of the meridian the sun is on. Nothing
  snaps: every colour is a continuous function of the time. Weather then grades those colours: cloud greys and dims
  them, a storm bruises them violet, haze washes out the distance first (far mountains, then the middle range, the
  hills, the shore), snow whitens the land in proportion to the light, and wet ground darkens.
- `Scene::Draw` renders, back to front: sky gradient, horizon glow at the sun's azimuth, stars and shooting stars,
  sun, moon (baked for its phase), birds on fair days, the cloud deck and drifting clouds, lightning, distant
  mountains, low cloud in heavy rain, mist, a middle range, forested hills, the far shore with its forest and town
  (lit windows at night that switch on and off), the lake (mirrored sky and mountains, the sun's or moon's glitter
  path, shimmer, rings where rain lands), the near shore and reeds, foreground pines, rain and snow, and a
  painted-canvas texture. The layers sway sideways with a slow camera breath scaled by their nearness (0.05× clouds,
  0.10× far mountains, 0.20× hills, 0.30× far shore, 0.45× near shore, 1× foreground pines). Wind sets the cloud
  drift (direction and speed), the rain's slant, how much the lake blurs its reflections, and how hard the pines and
  reeds sway — barely at a breeze, clearly in a gale.

Readability: the date, the time and the condition sit on the sky; the weather row, the advice and the rain timeline
on the land, which is kept dark at every hour. `textThemeFor` samples what lies behind each piece of text (sky at
several heights, the horizon glow, clouds at full strength, the mountains behind the feet of the digits, the hills,
the ground, mist) and picks light or dark ink for the sky text (with hysteresis; a flip fades over 2 s). Where the
scene alone falls short of the targets (5:1 for the digits, 4.5:1 for small text, 6:1 for the land text, 2.5:1 for
the big decorative colon), it works out the least backing that reaches them: a soft shadow (or, behind dark ink, a glow) around the
glyphs, plus a soft panel for anything the shadow cannot supply. So the text is readable by construction in any
weather; debug builds walk every two minutes of the day in every kind of weather at startup, check it, and log
how much backing the palettes needed. The accent (colon, rain bars) takes its hue from the scene — peach at sunrise,
blue by day, coral at sunset, violet at night, cool blue in rain, electric purple in a storm — and is lightened or
deepened within that hue until it reads.

Everything is drawn on the GPU as a few dozen batched `SDL_RenderGeometry` calls over a few small sprites baked at
startup (soft dot, glow, sun disc, moon, three clouds, the canvas). The app iterates at 30 Hz.

Debug-build helpers:

```sh
APP_FAKE_TIME=18:30 ./build/debug/digital_clock_v3                  # pretend it is 18:30 today
APP_FAKE_TIME="2026-12-21 16:10" ./build/debug/digital_clock_v3     # ...or any moment (season, moon phase)
APP_FAKE_WEATHER=95 ./build/debug/digital_clock_v3                  # pretend a WMO weather code (0 clear, 2 partly
                                                                    # cloudy, 3 overcast, 45 fog, 63 rain, 65 heavy
                                                                    # rain, 75 snow, 95 thunderstorm)
APP_FAKE_WIND=16,270 ./build/debug/digital_clock_v3                 # with APP_FAKE_WEATHER: wind m/s[,from degrees]
APP_SHOT=shot.png APP_SHOT_FRAME=40 ./build/debug/digital_clock_v3  # save a frame and exit
```

Screenshots work headless with `SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software`.
