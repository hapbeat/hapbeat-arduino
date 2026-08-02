# Hapbeat for Arduino — context for AI coding agents

Single self-contained reference so an AI coding agent can use this library
correctly from one file. Include name: `Hapbeat.h`. Repo: `hapbeat-arduino`.

- last-verified-against: 0.1.0
- Source of truth is the code: public API in `src/Hapbeat.h`, the wire format in
  `src/HapbeatProtocol.h` / `.cpp`. If this file disagrees with the code, the
  code wins.
- Canonical docs: https://devtools.hapbeat.com/docs/sdk-integration/arduino-sdk/

## What it is

A small, dependency-free C++ library to drive Hapbeat haptic devices over
Wi-Fi UDP from an ESP32 / M5Stack / ESP8266 / Arduino-compatible Wi-Fi board.
For makers, physical computing, research, and teaching. No cloud, no broker — it
sends UDP on the LAN (port 7700). The examples use M5Unified; the library itself
has **zero dependencies**.

This is the **level-1** library: command (fire) mode + synthesized-sine
streaming. Stored-WAV/clip streaming, an EventMap, and ESP-NOW transport are
**not** here yet (future work).

## Core model: two ways to make haptics

- **Command (fire) mode** — `hb.play("kit-name.clip-name")` sends a ~30-byte
  trigger; the haptic waveform lives in the **kit deployed to the device** via
  Hapbeat Studio. The MCU stores nothing. Needs a kit on the device.
- **Synthesized sine** — `hb.playSine(freqHz, intensity, durationMs)` generates
  a PCM16 sine on the MCU and streams it. Frequency + intensity are live
  parameters. **No kit, no WAV, no Studio** — the fastest first success.

Event id format is `<kit-name>.<file-name>` (e.g. `sample-kit.sine_100hz`); the
id must exist in the kit deployed to the device.

## Install

- **Arduino IDE**: Library Manager → search "Hapbeat", or *Sketch → Include
  Library → Add .ZIP Library* from a repo ZIP.
- **PlatformIO**: `lib_deps = hapbeat/arduino@^0.1.0`

## Quick start

```cpp
#include <WiFi.h>        // ESP8266: <ESP8266WiFi.h>
#include <Hapbeat.h>

Hapbeat hb;

void setup() {
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(200);
  WiFi.setSleep(false);                 // streaming: avoids choppiness
  hb.begin(7700, "MyDevice");           // app name shows on the Hapbeat OLED
}

void loop() {
  // Easiest first test — no kit needed, just a powered Hapbeat on the same LAN:
  hb.playSine(160.0f, 0.7f, 400);       // 160 Hz, intensity 0.7, 400 ms

  // Or fire a kit event (the id must exist in the device's deployed kit):
  hb.play("sample-kit.sine_100hz", 0.6f);
}
```

## Public API (src/Hapbeat.h)

| Method | Purpose |
|---|---|
| `begin(port = 7700, appName = "")` | open the UDP socket; `appName` (≤16 chars) shows on the Hapbeat OLED while connected |
| `play(eventId, gain = 1.0, target = "")` | fire a kit event (gain 0..1) |
| `stop(eventId, target = "")` / `stopAll(target = "")` | stop |
| `ping()` | keep-alive / RTT probe |
| `setGroup(group)` | group id sent in CONNECT_STATUS (level-1 default 0) |
| `discover(timeoutMs = 1500)` | broadcast a PING and collect **every** PONG (blocks the full timeout); true if ≥1 replied |
| `poll()` / `deviceCount()` / `deviceIp()` | drain pending PONGs (optional; sends do it too) / live device count / first device |
| `setDeviceIp(ip)` | pin a device by IP (never expires); `0.0.0.0` clears pinned entries |
| `setDeviceTimeout(ms)` / `setBroadcastOnly(on)` | liveness window (default 15 s) / force broadcast |
| `playSine(freqHz, intensity, durationMs, target = "")` | one-shot synthesized sine (blocks ~durationMs) |
| `beginSine(freqHz, intensity, target = "")` / `pumpSine()` / `endSine()` | continuous (open-ended) sine for hold-to-play / live control; call `pumpSine()` often from `loop()` |
| `sineActive()` | is a continuous sine stream running |
| `end()` | tell the device this app is leaving (clears the OLED app name) |

`target`: `""` = every device. Addresses are always
`player_<N>/<position>/group_<M>` (defaults `player_1` / `group_1`) and matching
is **positional** — segment *i* vs segment *i*, `*` covers one **whole**
segment, a short target front-matches. So `"player_1/pos_neck"` and
`"*/pos_neck"` work, but a group must be written **`"*/*/group_2"`**
(`"group_2"` alone is compared against the player slot and never matches), and
`"pos_*"` is **not** a wildcard (compared literally — write `"player_1/*"`).

## How streaming works (so you don't reinvent it)

- 16 kHz mono PCM16, generated on the MCU. One `STREAM_BEGIN`, continuous
  `STREAM_DATA` (running offset), `STREAM_END` on stop.
- The sender keeps ~160 ms of audio buffered ahead in the device's ~256 ms ring
  (the lead buffer absorbs Wi-Fi jitter). `pumpSine()` self-paces and is bounded
  per call — call it every `loop()` iteration; never block `loop()` > ~160 ms.
- **Unicast by default**: commands and stream packets go to every device in the
  table (populated by `discover()` / any PONG), and broadcast only while the
  table is empty. Broadcast has no MAC ACK/retry (lossy → choppy) and is held
  by the AP until its next DTIM beacon (100–300 ms) whenever any client is
  power-saving. Call `ping()` every few seconds from `loop()` so devices don't
  age out of the table (15 s default). `setBroadcastOnly(true)` reverts to
  broadcast — better for firing many devices in lockstep, since N unicasts go
  out one after another. Never both at once (a device would fire twice).

## Wire format / interop

- Byte-for-byte compatible with `hapbeat-contracts` message-format and the other
  SDKs (Python `protocol.py`, Unity `HapbeatProtocol.cs`). See
  `src/HapbeatProtocol.h` (framework-agnostic, allocation-free packet builders)
  and `docs/wire-format.md`.
- Magic `0x4842` ("HB"), version 1, 8-byte header, little-endian. UDP, no ACK
  (haptics policy: "better dropped than late").

## Gotchas

- The MCU and the Hapbeat must be on the **same Wi-Fi/LAN** (or the Hapbeat as a
  SoftAP the MCU joins).
- Command mode needs a **kit deployed to the device** (Studio). If `play(...)`
  does nothing, deploy the kit first — or use `playSine` (no kit) to verify.
- Single-button boards (M5 ATOM): map the one button to the kit-free sine so a
  first test needs no kit.
- Keep the radio awake during streaming: `WiFi.setSleep(false)`.
