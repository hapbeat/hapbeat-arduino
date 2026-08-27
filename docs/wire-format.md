# Wire format cheat sheet

What to send to make a Hapbeat buzz, at the byte level — so you can replicate it
from any language/MCU even without this library. Full spec:
`hapbeat-contracts/specs/message-format.md`.

- Transport: **UDP** to **port 7700**, on the same Wi-Fi/LAN as the device.
  Commands may use the **broadcast address** `255.255.255.255` before discovery,
  but stream packets must use the exact `(IP, UDP port, full PONG address)`
  endpoint. A pinned IP without that address is not a stream endpoint.
- **All multi-byte fields are little-endian.**

## Header (8 bytes, every packet)

| off | field | type | value |
|----|-------|------|-------|
| 0 | magic | u16 | `0x4842` ("HB") → bytes `42 48` |
| 2 | version | u8 | `0x01` |
| 3 | command | u8 | see below |
| 4 | seq | u16 | increments per packet |
| 6 | payload_length | u16 | bytes after the header |

Commands: `0x01` PLAY · `0x02` STOP · `0x03` STOP_ALL · `0x10` PING ·
`0x20` CONNECT_STATUS · `0x30/0x31/0x32` STREAM begin/data/end.

## PLAY (0x01) — fire a kit event

Payload: `event_id` (null-terminated UTF-8) + `target` (null-terminated; `""` =
all devices) + `target_time` (i64 µs, `0` = now) + `gain` (f32, 0..1) + `pan`
(f32, `-1` = left only … `0` = centre … `+1` = right only).

**Worked example** — play event `sample-kit.sine_100hz` on all devices at gain `0.6`:

```
header : 42 48 01 01 01 00 27 00
         └magic┘ V  C  └seq┘ └len=39┘
payload: 73 61 6d 70 6c 65 2d 6b 69 74 2e 73 69 6e 65 5f 31 30 30 68 7a 00
                                            "sample-kit.sine_100hz\0"
         00                                  target "" (\0)
         00 00 00 00 00 00 00 00             target_time = 0 (i64)
         9a 99 19 3f                         gain = 0.6 (f32 LE)
         00 00 00 00                         pan  = 0.0 (f32 LE, centre)
```

47 bytes total → UDP to `255.255.255.255:7700`. That single packet makes the
device play its `sample-kit.sine_100hz` clip — deploy the `sample-kit` in Hapbeat
Studio (it appears automatically when you pick a Kit folder).

## STOP (0x02) / STOP_ALL (0x03)

- STOP payload: `event_id`\0 + `target`\0.
- STOP_ALL payload: `target`\0.

## CONNECT_STATUS (0x20) — optional keep-alive (OLED app name)

Payload: `connected` (u8 1/0) + `group` (u8) + `app_name`\0 + `device_name`\0.

## Sine / PCM streaming (0x30 → 0x31… → 0x32)

For synthesized haptics (e.g. a sine of a chosen frequency/intensity). The
device mixes streamed PCM alongside clips into a ~256 ms ring buffer.

1. **STREAM_BEGIN (0x30)** — `sample_rate` (u16, exactly `16000`) + `channels`
   (u8, exactly `2`) + `format` (u8, `0` = PCM16) + `total_samples` (u32,
   exactly `0`) + `gain` (f32, exactly `1.0`) + the full PONG device address
   (`target`\0). **Send this first** — the device only accepts STREAM_DATA after
   a matching BEGIN. Each exact endpoint has its own session.
2. **STREAM_DATA (0x31)** — `offset` (u32, running byte offset) + raw
   little-endian PCM16 samples. Keep each packet ≤ 1472 bytes and feed at
   ~real-time (16000 samples/sec) so the 256 ms ring neither overflows nor
   underruns.
3. **STREAM_END (0x32)** — no payload. The buffer drains naturally.

This library's `playSineSource(freqHz, intensity, durationMs, target, pan, loop)`
generates a source on the MCU, applies source gain and linear pan, then mixes
all matching sources into endpoint-scoped PCM16 stereo @ 16 kHz. The BEGIN gain
is always `1.0`; source intensity is applied in the mixed samples. `tick()` is
cooperative and bounded, and sends no stream packet until a full endpoint is
resolved.
