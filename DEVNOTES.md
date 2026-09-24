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

# Animated background

`scene.h` draws an animated landscape behind the text: a sky gradient with a soft glow, the sun on an arc from
sunrise to sunset, the moon and twinkling stars (and the odd shooting star) at night, drifting clouds, far
mountains, mid hills with twinkling town lights, near hills, and leaves swaying in the bottom corners. The hill
layers sway sideways very slowly, each by an amount proportional to its nearness (parallax). A fixed "painted
canvas" of brush strokes lies over everything but the text.

Its colours come from one of eighteen palettes ("worlds"): six times of day (night, blue hour, dawn, day, golden
hour, dusk, keyed to the real sunrise and sunset from the forecast, 07:00 and 19:00 until the first fetch
succeeds) for each of three kinds of weather (clear, grey, storm), picked from the WMO weather code. Within a kind
the palettes blend smoothly through the day. The particles — cloud cover, rain, snow, fog and lightning — ease in
and out over a few seconds when the forecast changes, and a change of kind fades the palette over 12 s.

Readability: the date and time sit on the sky, and the weather strip and rain chart on the near hills, which are
dark in every world, so that text is always light. Each palette is designed for contrast on its own (ink ≥ 7:1, dim
ink and small labels ≥ 4.5:1, accents ≥ 3:1, against the sky, the glow, the clouds and, for the feet of the
digits, the mountains), and colours only blend between palettes of the same polarity. A flip between a light and a
dark sky (sunrise, sunset, a storm rolling in) is a 2.5 s fade. Debug builds walk every minute of the day for every
kind of weather at startup and log any moment below those targets.

Everything is drawn on the GPU as about twenty batched `SDL_RenderGeometry` calls over a few small sprites baked at
startup (soft dot, glow, sun disc, crescent, three clouds, the canvas). The app iterates at 30 Hz.

Debug-build helpers:

```sh
APP_FAKE_TIME=18:30 ./build/debug/digital_clock_v3                  # pretend it is 18:30 today
APP_FAKE_WEATHER=95 ./build/debug/digital_clock_v3                  # pretend a WMO weather code (0 clear, 3 overcast,
                                                                    # 45 fog, 63 rain, 75 snow, 95 thunderstorm)
APP_SHOT=shot.png APP_SHOT_FRAME=40 ./build/debug/digital_clock_v3  # save a frame and exit
```

Screenshots work headless with `SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software`.
