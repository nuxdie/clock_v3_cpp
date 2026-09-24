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

# Sky colours

The background follows the real sunrise and sunset from the forecast (`daily=sunrise,sunset`; 07:00 and 19:00
until the first fetch succeeds): night → blue hour → dawn → day → golden hour → dusk → night. Each palette in
`namespace Sky` is designed for contrast on its own (ink ≥ 7:1, dim ink and small labels ≥ 4.5:1, accent and
rain ≥ 3:1), and colours only blend between palettes of the same polarity. The single dark ↔ light flip at
sunrise and sunset is a 2.5 s crossfade. Debug builds walk every minute of the day at startup and log any
moment below those contrast targets.

The sky (gradient, glow and a fixed "painted canvas" of brush strokes) is composed on the CPU once a minute at
half resolution and drawn as one opaque texture, so a frame is just that quad plus the text. The app iterates at
20 Hz, which is enough for the colon pulse.

Debug-build helpers:

```sh
APP_FAKE_TIME=18:30 ./build/debug/digital_clock_v3             # pretend it is 18:30 today
APP_SHOT=shot.png APP_SHOT_FRAME=40 ./build/debug/digital_clock_v3  # save a frame and exit
```
