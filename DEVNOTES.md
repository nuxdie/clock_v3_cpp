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

In debug builds you can also paint a local photo without any network access:

```sh
CLOCK_TEST_IMAGE=/path/to/photo.jpg ./build/debug/digital_clock_v3
```

# Painted background

Every downloaded photo is turned into a painting once, on a low-priority worker thread (`Painter::paint`):
cover-crop to the screen size, a blurry underpainting, three layers of brush strokes that follow the edges of the
photo (Hertzmann-style painterly rendering), then colour grading: more saturation, the brightest 10% capped so
the text always stands out, a soft vignette and a faint canvas grain. The two strongest hues of the result
become the colours of the hours and minutes. Until the first photo arrives (or with no network), a generated
abstract canvas is painted instead.

All of this is CPU work done once per picture (~0.3 s on a desktop, a few seconds on a Raspberry Pi 3). Rendering
itself is one full-screen texture plus the text textures, and a frame is only drawn when something changed
(minute tick, weather update, new painting fading in), so the GPU and CPU stay idle almost all the time.

Text has a dark outline baked into its texture, so it is readable on any part of any picture.

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
