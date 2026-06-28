# Hapbeat for Arduino / ESP32 / M5Stack

Drive [Hapbeat](https://hapbeat.com) haptic devices from a microcontroller over
Wi-Fi UDP. Press a button → a Hapbeat buzzes. Works on **ESP32-series, M5Stack,
ESP8266, and Arduino-compatible Wi-Fi boards**.

> **📚 Docs**: <https://devtools.hapbeat.com/docs/sdk-integration/arduino-sdk/getting-started/>

Two ways to make haptics, no stored audio required:

- **Command (fire) mode** — trigger a clip that lives in the kit on the device
  (`hb.play("sample-kit.sine_100hz")`). The MCU stores nothing; it just sends a ~30-byte trigger.
- **Synthesized sine streaming** — generate a sine on the MCU and stream it
  (`hb.playSine(freqHz, intensity, durationMs)`). Frequency and intensity are
  live parameters — expressive without any WAV files.

This is the **level-1** library (initial draft). Verify on your own ESP32 before
relying on it; ESP-NOW transport and richer synthesis are future work.

## Install

- **Arduino IDE**: Library Manager → search **"Hapbeat"** → Install. (Or *Sketch
  → Include Library → Add .ZIP Library* from a repo ZIP.)
- **PlatformIO**: add to `lib_deps`:
  ```
  lib_deps = hapbeat/arduino@^0.1.0
  ```

The library itself has **no dependencies**. The examples use **M5Unified**.

## Quick start

```cpp
#include <WiFi.h>        // ESP8266: <ESP8266WiFi.h>
#include <Hapbeat.h>

Hapbeat hb;

void setup() {
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(200);
  hb.begin(7700, "MyDevice");          // app name shows on the Hapbeat OLED
}

void loop() {
  // ... on some input:
  hb.play("sample-kit.sine_100hz", 0.6f);         // fire a kit event (id must be in the kit)
  // or synthesize:
  hb.playSine(150.0f, 0.7f, 400);      // 150 Hz, intensity 0.7, 400 ms
}
```

`"sample-kit.sine_100hz"` must be an event id present in the **kit deployed to the device**
via [Hapbeat Studio](https://devtools.hapbeat.com).

## Examples

| Example | What it does |
|---|---|
| `M5Stack_Command` | Btn A/B → fire kit events (`play`) |
| `M5Stack_SineStream` | Btn A/B → stream a synthesized sine (`playSine`) |

(Single-button boards like M5 ATOM only have `BtnA` — use that one.)

## API

| Method | Purpose |
|---|---|
| `begin(port = 7700, appName = "")` | open the UDP socket; announce app name |
| `play(eventId, gain = 1.0, target = "")` | fire a kit event (gain 0..1) |
| `stop(eventId, target = "")` / `stopAll(target = "")` | stop |
| `playSine(freqHz, intensity, durationMs, target = "")` | stream a synthesized sine |
| `ping()` / `end()` | keep-alive probe / announce disconnect |

`target`: `""` = broadcast to all; `"player_1/chest"`, `"*/chest"` to address devices.

## Network

The MCU and the Hapbeat must be on the same Wi-Fi/LAN (or the Hapbeat acting as
a SoftAP that the MCU joins). The library sends UDP broadcast on port 7700.

## How it works at the byte level

See [docs/wire-format.md](docs/wire-format.md) — the exact PLAY packet bytes, so
you can replicate it from any language/MCU without this library.

## License

MIT © Hapbeat
