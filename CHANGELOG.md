# Changelog

All notable changes to the **Hapbeat** Arduino library (`hapbeat-arduino`).

## 0.1.0 — first public release

Level-1 Arduino / ESP32 / M5Stack library to drive Hapbeat haptic devices over
Wi-Fi UDP. The library has **zero dependencies** (the examples use M5Unified).

### Command (fire) mode
- `play(eventId, gain, target)` / `stop` / `stopAll` / `ping` / `setGroup`.
- Fires a kit event by id (`<kit-name>.<file-name>`); the waveform lives in the
  kit deployed to the device via Hapbeat Studio. The MCU sends a ~30-byte
  trigger and stores nothing.

### Synthesized sine (no kit, no WAV)
- `playSine(freqHz, intensity, durationMs)` — one-shot.
- `beginSine` / `pumpSine` / `endSine` / `sineActive` — continuous, open-ended
  stream for hold-to-play and live frequency/intensity control.
- 16 kHz mono PCM16 generated on the MCU; a ~160 ms sender lead buffer absorbs
  Wi-Fi jitter against the device's ~256 ms ring.
- Reliability: `STREAM_BEGIN`/`STREAM_END` are resent (a lost BEGIN would mute
  the whole stream); the prime/catch-up burst is bounded per `pumpSine()` call.

### Discovery & addressing
- `discover()` / `deviceIp()` / `setDeviceIp()` — broadcast PING → PONG learns
  the device IP so streaming can **unicast** (Wi-Fi MAC ACK + retry = far
  smoother than broadcast). Falls back to broadcast when no device is found.
- `target` addressing: `""` (broadcast), `"player_1/chest"`, `"*/chest"`,
  `"group_<N>"`.

### Interop
- Wire format byte-compatible with `hapbeat-contracts` and the Python / Unity /
  JS SDKs (see `docs/wire-format.md`).

### Not yet (future work)
- ESP-NOW transport (router-free), stored-WAV / clip streaming, an EventMap
  tuning layer, and richer synthesis.
