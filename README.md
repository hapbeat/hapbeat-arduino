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
  lib_deps = hapbeat/arduino@^0.2.0
  ```

The library itself has **no dependencies**. The `M5Stack_*` examples use
**M5Unified**; `Verify_Unicast` needs nothing beyond Wi-Fi + Serial.

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
| `M5Stack_Command` | A/B → fire kit events, C → stop. Unicasts to discovered devices; the screen shows the button map and what it is sending to |
| `M5Stack_SineStream` | A 40 Hz / B 100 Hz for a fixed time, C 100 Hz while held. No kit needed |
| `Verify_Unicast` | continuous 100 Hz sine, auto-alternating broadcast ⇄ unicast every 5 s (release check — no display library needed) |

(Single-button boards like M5 ATOM only have `BtnA` — use that one.)

## API

| Method | Purpose |
|---|---|
| `begin(port = 7700, appName = "")` | open the UDP socket; announce app name |
| `play(eventId, gain = 1.0, target = "")` | fire a kit event (gain 0..1) |
| `stop(eventId, target = "")` / `stopAll(target = "")` | stop |
| `playSine(freqHz, intensity, durationMs, target = "")` | stream a synthesized sine |
| `ping()` / `end()` | keep-alive probe / announce disconnect |
| `discover(timeoutMs = 1500)` | broadcast a PING and collect every reply |
| `poll()` / `deviceCount()` / `deviceIp()` | device table upkeep and inspection |
| `setDeviceIp(ip)` / `setDeviceTimeout(ms)` / `setBroadcastOnly(on)` | routing controls |

### Addressing

`target`: `""` = every device. Matching is **positional** — segment *i* of the
target is compared with segment *i* of the device address, `*` stands for one
**whole** segment (`pos_*` is compared literally, so use `player_1/*`), and a
short target front-matches.

Device addresses are always `player_<N>/<position>/group_<M>`, defaulting to
`player_1` / `group_1`. So `"player_1/pos_neck"` and `"*/pos_neck"` work as you
would expect, but to select a **group** you must fill the earlier slots:
`"*/*/group_2"`. Plain `"group_2"` is compared against the *player* slot and
never matches.

## Network

The MCU and the Hapbeat must be on the same Wi-Fi/LAN (or the Hapbeat acting as
a SoftAP that the MCU joins). The library sends on UDP port 7700.

Commands and streams are **unicast** to every device that has answered a PING,
and broadcast only while no device is known. Wi-Fi APs hold broadcast frames
until their next DTIM beacon (100–300 ms) whenever any client on the AP is
power-saving — that shows up as a late buzz and as stuttering streams — and
broadcast frames are also unacked and sent at the lowest rate. Unicast avoids
both.

```cpp
hb.begin(7700, "MyDevice");
hb.discover();            // one PING, collects every reply (blocks ~1.5 s)
// ... and in loop(), every few seconds:
hb.ping();                // keeps the device table (and therefore unicast) alive
```

Devices drop out of the table after `setDeviceTimeout()` (15 s default) without
a PONG, at which point sends fall back to broadcast. Call `setBroadcastOnly(true)`
to always broadcast — worth it when many devices must fire in lockstep, since
one broadcast reaches them all at once while N unicasts go out one after another.

## How it works at the byte level

See [docs/wire-format.md](docs/wire-format.md) — the exact PLAY packet bytes, so
you can replicate it from any language/MCU without this library.

## License

MIT © Hapbeat
