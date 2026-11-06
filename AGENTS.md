# Hapbeat for Arduino — context for AI coding agents

Single self-contained reference so an AI coding agent can use this library
correctly from one file. Include name: `Hapbeat.h`. Repo: `hapbeat-arduino`.

- last-verified-against: 0.2.0
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
- **Synthesized sine** — `hb.playSineSource(...)` registers one or more sine
  sources on the MCU; `hb.tick()` mixes and streams them. Frequency, gain, pan,
  loop, and duration are live parameters. **No kit, no WAV, no Studio** — the
  fastest first success.

Sine streams go out as Stream Gen 2 to devices on firmware 0.5.0 and later and
as Stream Gen 1 to firmware 0.4.x and earlier; the library finds out which per
device (below).

Event id format is `<kit-name>.<file-name>` (e.g. `sample-kit.sine_100hz`); the
id must exist in the kit deployed to the device.

## Install

- **Arduino IDE**: Library Manager → search "Hapbeat", or *Sketch → Include
  Library → Add .ZIP Library* from a repo ZIP.
- **PlatformIO**: `lib_deps = hapbeat/arduino@^1.0.0`

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
| `play(eventId, gain = 1.0, target = "", pan = 0.0)` | fire a kit event. `gain` is the app gain; PLAY carries `clamp(gain, 0, 1)` (NaN / infinity as 0). The Kit manifest is not read, so the Kit's `clip_gain` is not applied (counts as 1; contracts kit-format.md "強さの式（正本）"). Linear left/right balance |
| `stop(eventId, target = "")` / `stopAll(target = "")` | stop |
| `ping()` | keep-alive / RTT probe |
| `setGroup(group)` | group id sent in CONNECT_STATUS (level-1 default 0) |
| `discover(timeoutMs = 1500)` | broadcast a PING and collect **every** PONG (blocks the full timeout); true if ≥1 replied |
| `poll()` / `deviceCount()` / `deviceIp()` | drain pending PONGs (optional; sends do it too) / live device count / first device |
| `setDeviceIp(ip)` | pin a device by IP (never expires); `0.0.0.0` clears pinned entries |
| `setDeviceTimeout(ms)` / `setBroadcastOnly(on)` | liveness window (default 15 s) / force command broadcast; streams still need a full PONG endpoint |
| `playSineSource(freqHz, intensity, durationMs = 0, target = "", pan = 0, loop = false)` | register a non-blocking source and return a move-only value `Playback` handle; keep the owning `Hapbeat` alive while using it |
| `playSine(freqHz, intensity, durationMs, target = "")` | blocking convenience wrapper through the same stream hub |
| `tick()` | consume PONGs / ERRORs, send the unicast lease PINGs, reconcile endpoints, mix sources, and pace stream packets |
| `streamClass(ip)` / `streamEndpoints(out, max)` | per resolved device: `HapbeatStreamClass` (`Gen1` / `Gen2` / `Unknown`) and `HapbeatLeaseState` (`Valid` / `Unavailable` / `Superseded`, `None` unless Gen2); `out` is a caller array of `HapbeatStreamEndpointInfo` |
| `reacquireStreamOwnership()` | fresh client incarnation + lease PING: take a device back after `Superseded` |
| `setWarningHandler(void (*)(const char*))` | one-line warnings (unreadable PONG tail, superseded lease, device ERROR), once per device; the library never prints |
| `end()` | tell the device this app is leaving (clears the OLED app name) |

`playSineSource` rejects non-finite frequency, intensity, and pan values with
an invalid handle.

`target`: `""` = every resolved stream endpoint (and every command device).
Addresses are always
`player_<N>/<position>/group_<M>` (defaults `player_1` / `group_1`) and matching
is **positional** — segment *i* vs segment *i*, `*` covers one **whole**
segment, a short target front-matches. So `"player_1/pos_neck"` and
`"*/pos_neck"` work, but a group must be written **`"*/*/group_2"`**
(`"group_2"` alone is compared against the player slot and never matches), and
`"pos_*"` is **not** a wildcard (compared literally — write `"player_1/*"`).

## How streaming works (so you don't reinvent it)

- Each full PONG `(IP, UDP port, address)` is an exact stream endpoint. Stream
  packets are never broadcast; a pinned IP without a PONG address cannot start
  a stream. Each endpoint has one session and its own cursor.
- Sources are generated on the MCU and mixed per endpoint as 16 kHz stereo
  PCM16 after source gain and linear pan. One canonical `STREAM_BEGIN` uses
  16 kHz / stereo / PCM16 / total 0 / gain 1 and the full endpoint address.
- The sender keeps ~160 ms of audio buffered ahead in the device's ~256 ms ring
  (the lead buffer absorbs Wi-Fi jitter). `tick()` self-paces and is bounded per
  call — call it every `loop()` iteration; never block `loop()` > ~160 ms.
- Commands may fall back to broadcast while no destination is known. Stream
  handles become `Deferred(NoResolvedEndpoint)` until a full endpoint resolves;
  handles do not retain their owner, so keep `Hapbeat` alive while using them;
  the owner must outlive each handle and no handle method may be called after
  owner destruction;
  `setBroadcastOnly(true)` never changes stream routing. Empty sessions linger
  310 ms before `STREAM_END`.
- Stream format per device (contracts `stream-session.md`): `tick()` sends a
  16-byte extended PING by unicast to each device a source targets (never
  broadcast; `ping()` / `discover()` stay 8 bytes). Only the direct reply
  decides: an `HBS2` tail → **Stream Gen 2** (header version 2, a 16-byte
  `device_boot_id / lease_ticket / generation` envelope, the device-issued
  lease, BEGIN repeated, next generation right after END, restart after a 3 s
  `tick()` gap); no tail → **Stream Gen 1** (version 1, no envelope, 310 ms
  END-to-BEGIN guard for that device only, during which the source stays
  `Active`). Leases are refreshed every 5 s and for 15 s after the last session.
- `deferredReason()`, strongest first: `Superseded` (another application
  streamed later; `reacquireStreamOwnership()` or the device handing it back
  clears it), `LeaseUnavailable` (Gen 2 device without a usable lease),
  `UnknownClass` (no reply to the lease PING yet), `NoResolvedEndpoint`.
- `stopAll()` ends every stream session at once; a source started right after
  begins a new session. ERROR `STREAM_STOPPED` (another sender's STOP_ALL)
  takes that session's sources off that device for good; a new Playback joins
  it as usual. A finite source is `Stopped` once it has played to the end on
  some device and no session still carries it.
- Not implemented: no action on the device ERRORs `STREAM_TIMED_OUT`,
  `STREAM_TARGET_MISMATCH` (warning only); CAPS is not read
  (no newer-major warning, pan still applied on one-motor devices); no
  endpoint-changed callback; no gain/pan smoothing, limiter or re-splice;
  boards other than ESP32 / ESP8266 use a weak `random()` client incarnation.

`Hapbeat` is not thread-safe. Synchronize callers that share one instance;
independent instances have independent stream state and do not require shared
locking.

## Wire format / interop

- Byte-for-byte compatible with `hapbeat-contracts` message-format and the other
  SDKs (Python `protocol.py`, Unity `HapbeatProtocol.cs`). See
  `src/HapbeatProtocol.h` (framework-agnostic, allocation-free packet builders)
  and `docs/wire-format.md`.
- Magic `0x4842` ("HB"), 8-byte header, little-endian. Header version 1 for
  every command except Stream Gen 2 BEGIN / DATA / END (version 2). UDP, no ACK
  (haptics policy: "better dropped than late").

## Gotchas

- The MCU and the Hapbeat must be on the **same Wi-Fi/LAN** (or the Hapbeat as a
  SoftAP the MCU joins).
- Command mode needs a **kit deployed to the device** (Studio). If `play(...)`
  does nothing, deploy the kit first — or use `playSine` (no kit) to verify.
- Single-button boards (M5 ATOM): map the one button to the kit-free sine so a
  first test needs no kit.
- Keep the radio awake during streaming: `WiFi.setSleep(false)`.
