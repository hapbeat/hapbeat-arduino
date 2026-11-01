# Hapbeat for Arduino / ESP32 / M5Stack

Drive [Hapbeat](https://hapbeat.com) haptic devices from a microcontroller over
Wi-Fi UDP. Press a button → a Hapbeat buzzes. Works on **ESP32-series, M5Stack,
ESP8266, and Arduino-compatible Wi-Fi boards**.

> **📚 Docs**: <https://devtools.hapbeat.com/docs/sdk-integration/arduino-sdk/getting-started/>

Two ways to make haptics, no stored audio required:

- **Command (fire) mode** — trigger a clip that lives in the kit on the device
  (`hb.play("sample-kit.sine_100hz")`). The MCU stores nothing; it just sends a ~30-byte trigger.
- **Synthesized sine streaming** — register one or more sine sources on the MCU
  (`hb.playSineSource(...)`) and service them with `hb.tick()`. Frequency,
  intensity, pan, and duration are live parameters — expressive without WAV
  files.

> **Device firmware 0.5.0 and later:** this library's sine streaming (`playSine`,
> and `beginSine` / `pumpSine` / `endSine` in the published v0.2.0) sends the
> older stream format, which firmware 0.5.0+ no longer accepts, so sine streams
> are **silent** on those devices. `play()` (command mode, firing a kit event)
> still works. On firmware 0.4.x and earlier, sine streams work too.

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
| `M5Stack_SineStream` | A 200 Hz / B 100 Hz for a fixed time, C 100 Hz while held. No kit needed |
| `Verify_Unicast` | continuous 100 Hz sine with command broadcast/unicast toggling; stream packets remain exact-unicast (release check — no display library needed) |

(Single-button boards like M5 ATOM only have `BtnA` — use that one.)

## API

| Method | Purpose |
|---|---|
| `begin(port = 7700, appName = "")` | open the UDP socket; announce app name |
| `play(eventId, gain = 1.0, target = "", pan = 0.0)` | fire a kit event. `gain` is the app gain; the PLAY carries `clamp(gain, 0, 1)` (NaN / infinity as 0). This library does not read the Kit manifest, so the Kit's `clip_gain` is not applied (it counts as 1). `pan` is the left/right balance, -1 = left only … 0 = centre … +1 = right only |
| `stop(eventId, target = "")` / `stopAll(target = "")` | stop |
| `playSineSource(freqHz, intensity, durationMs = 0, target = "", pan = 0, loop = false)` | register a non-blocking source and return a move-only value `Playback` handle; keep the owning `Hapbeat` alive while using it |
| `playSine(freqHz, intensity, durationMs, target = "")` | blocking convenience wrapper through the same stream hub |
| `tick()` | consume PONGs, reconcile endpoints, mix sources, and pace stream packets |
| `ping()` / `end()` | keep-alive probe / announce disconnect |
| `discover(timeoutMs = 1500)` | broadcast a PING and collect every reply |
| `poll()` / `deviceCount()` / `deviceIp()` | device table upkeep and inspection |
| `setDeviceIp(ip)` / `setDeviceTimeout(ms)` / `setBroadcastOnly(on)` | command routing controls; streams still require a full PONG endpoint |

`playSineSource` rejects non-finite frequency, intensity, and pan values with
an invalid handle.

### Addressing

`target`: `""` = every resolved endpoint for streams (and every device for
commands). Matching is **positional** — segment *i* of the target is compared
with segment *i* of the device address, `*` stands for one whole **segment**
(`pos_*` is compared literally, so use `player_1/*`), and a short target
front-matches.

Device addresses are always `player_<N>/<position>/group_<M>`, defaulting to
`player_1` / `group_1`. So `"player_1/pos_neck"` and `"*/pos_neck"` work as you
would expect, but to select a **group** you must fill the earlier slots:
`"*/*/group_2"`. Plain `"group_2"` is compared against the *player* slot and
never matches.

## Network

The MCU and the Hapbeat must be on the same Wi-Fi/LAN (or the Hapbeat acting as
a SoftAP that the MCU joins). The library sends on UDP port 7700.

Commands are unicast to every device that has answered a PING, with broadcast
fallback while no command destination is known. Stream packets are different:
they are never broadcast, including when `setBroadcastOnly(true)` is enabled or
only a pinned IP is configured. A stream session starts only after a full PONG
address resolves an exact `(IP, UDP port, address)` endpoint. This avoids AP
DTIM buffering and keeps multiple sources isolated per endpoint.

```cpp
hb.begin(7700, "MyDevice");
hb.discover();            // one PING, collects every reply (blocks ~1.5 s)
// ... and in loop(), every few seconds:
hb.ping();                // keeps the device table (and therefore unicast) alive
```

Devices drop out of the table after `setDeviceTimeout()` (15 s default) without
a PONG. Commands then fall back to broadcast; stream handles become
`Deferred(NoResolvedEndpoint)` until a full endpoint is available. Call
`setBroadcastOnly(true)` only when command packets should be broadcast.

Each `playSineSource` handle has independent status, gain, pan, loop, and
idempotent `stop()` control. Handles do not retain their owning `Hapbeat`; the
owner must outlive each handle, and no handle method may be called after owner
destruction. The hub mixes all matching sources per exact
endpoint as 16 kHz stereo PCM16, keeps a per-endpoint cursor, and uses a short
empty-session linger/cooldown around `STREAM_END`.

`Hapbeat` is not thread-safe. Synchronize callers that share one instance;
independent instances have independent stream state and do not require shared
locking.

## How it works at the byte level

See [docs/wire-format.md](docs/wire-format.md) — the exact PLAY packet bytes, so
you can replicate it from any language/MCU without this library.

## License

MIT © Hapbeat
