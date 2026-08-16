# Replit run notes

## Start the server

```bash
npm install
npm start
```

The HTTP and WebSocket server listens on port `5000`. The WebSocket endpoints
are `/ws/gemini` and `/ws/esp32`.

## Gemini configuration

The server reads `GEMINI_API_KEY` through `GEMINI_API_KEY10` from Replit
Secrets. The default Live model is `models/gemini-3.1-flash-live-preview`; set
`GEMINI_LIVE_MODEL` only if you need to switch to another model supported by
your Gemini project.

## Browser voice tester

Open `/app` in the Replit preview, click **Start microphone**, speak, then click
**Stop & Send**. The dashboard shows the mic level and logs the complete
`START_STREAM` / `END_STREAM` sequence. Gemini's PCM voice response is played in
the browser, so this can verify the server and Gemini flow before flashing the
ESP32 firmware. The bridge queues audio and the end-of-stream marker while
Gemini is still completing its session setup, so short first utterances are not
lost.

## ESP32 connection stability

The ESP32 firmware uses `WebSocketsClient`'s built-in reconnect loop. Its
15-second JSON keepalive is handled by the server as a local pong and is not
forwarded to Gemini Live. After changing `firmware.ino`, flash the sketch to
the ESP32 and monitor the serial log for `[WS] Connected ✓` and
`[WS] Server hello/keepalive ack`, AI audio frames, and `[AUDIO] PLAYBACK_END`.
The server splits Gemini's larger PCM responses into 4096-byte binary frames,
and the firmware also splits oversized frames before writing them to I2S so
audio is not silently dropped. Keep `WS_HOST` in `firmware.ino` aligned with
the current Replit preview host before flashing.