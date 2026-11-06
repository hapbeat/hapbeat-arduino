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
| 2 | version | u8 | `0x01`; `0x02` for Stream Gen 2 BEGIN / DATA / END |
| 3 | command | u8 | see below |
| 4 | seq | u16 | increments per packet |
| 6 | payload_length | u16 | bytes after the header |

Commands: `0x01` PLAY · `0x02` STOP · `0x03` STOP_ALL · `0x10` PING ·
`0x20` CONNECT_STATUS · `0x30/0x31/0x32` STREAM begin/data/end. From the
device: `0x11` PONG · `0xFF` ERROR.

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

## PING (0x10) / PONG (0x11) — discovery and the stream lease

- **Discovery PING**: payload `timestamp` (i64 µs, never 0). Broadcast or
  unicast. It never allocates a stream lease.
- **Extended PING** (Stream Gen 2 lease request): `timestamp` (i64) +
  `client_incarnation` (u64, nonzero, fresh per `begin()` /
  `reacquireStreamOwnership()`) — 16 bytes. **Unicast only.** Example (seq 257,
  timestamp 1700000000000000, incarnation `0x1122334455667788`):
  `42 48 01 10 01 01 10 00  00 40 1e 18 24 0a 06 00  88 77 66 55 44 33 22 11`.
- **PONG** payload: `timestamp` (the PING's), `server_time` (i64),
  `device_name`\0, `address`\0, `firmware_version`\0, `volume_level` (u8),
  `volume_wiper` (u8), then — only in a direct reply to an extended PING from
  firmware 0.5.0+ — a 32-byte tail:

| tail off | field | type |
|----|-------|------|
| 0 | marker | `48 42 53 32` ("HBS2") |
| 4 | extension_version | u8 = 1 |
| 5 | flags | u8: bit 0 lease_valid, bit 1 superseded, bit 2 reissued |
| 6 | reserved | u16 |
| 8 | echoed_client_incarnation | u64 |
| 16 | device_boot_id | u64 |
| 24 | lease_ticket | u32 |
| 28 | high_water_ticket | u32 |

Only a reply whose seq, echoed timestamp, source (IP, port) and echoed
incarnation match an outstanding extended PING classifies the device:
nothing after `volume_wiper` → **Stream Gen 1** (firmware 0.4.x and older); at
least 32 bytes starting with `HBS2` and extension_version 1 → **Stream Gen 2**
(read the first 32 bytes, ignore reserved, other flag bits and anything
after); anything else → ignored. An unsolicited PONG (timestamp 0) never
changes the class or the lease.

## Sine / PCM streaming (0x30 → 0x31… → 0x32)

For synthesized haptics (e.g. a sine of a chosen frequency/intensity). The
device mixes streamed PCM alongside clips into a ~256 ms ring buffer. Every
stream packet goes by exact unicast to one `(IP, UDP port, full PONG address)`
endpoint, in the format its class needs.

### Stream Gen 2 (firmware 0.5.0 and later)

Header version `0x02`. Every payload starts with a 16-byte envelope:
`device_boot_id` (u64) + `lease_ticket` (u32) from the device's PONG tail +
`generation` (u32, starts at 1 per lease, +1 per session, never reused).

1. **STREAM_BEGIN** — envelope + `sample_rate` (u16, `16000`) + `channels` (u8,
   `2`) + `format` (u8, `0` = PCM16) + `total_samples` (u32, `0`) + `gain`
   (f32, `1.0`) + the full device address (`target`\0, at most 63 chars). Sent
   before each of the first three DATA and then at least every 500 ms (the
   same tuple; the device ignores an equal BEGIN).
2. **STREAM_DATA** — envelope + `offset` (u32, from 0 per session) + PCM16
   samples (≥ 1 byte; ≤ 1472 bytes per packet).
3. **STREAM_END** — the envelope only. The next session (generation + 1) may
   begin at once.

Fixture (`hapbeat-contracts/fixtures/stream-session-v2.json`; boot
`0x0102030405060708`, ticket 7, generation 42, seq 4660):

```
BEGIN 42 48 02 30 34 12 1e 00  08 07 06 05 04 03 02 01 07 00 00 00 2a 00 00 00
      80 3e 02 00 00 00 00 00 00 00 80 3f 2a 00          ("*\0" target)
DATA  42 48 02 31 34 12 18 00  <envelope>  00 00 00 00  10 00 f0 ff
END   42 48 02 32 34 12 10 00  <envelope>
```

The device answers rejected stream packets with ERROR (`0xFF`): `error_code`
(u16) + `message`\0 + `ref_command` (u8) + `ref_seq` (u16) + `detail_length`
(u8) + `detail` (the rejected envelope for codes 0x0011–0x0017). This library
acts on `STREAM_NOT_STARTED` (BEGIN again), `STREAM_SUPERSEDED` (defer),
`STREAM_LEASE_INVALID` (new lease) and `STREAM_GEN_UNSUPPORTED` (classify
again), and reports the rest through the warning handler.

### Stream Gen 1 (firmware 0.4.x and earlier)

Header version `0x01`, no envelope.

1. **STREAM_BEGIN (0x30)** — `sample_rate` (u16, exactly `16000`) + `channels`
   (u8, exactly `2`) + `format` (u8, `0` = PCM16) + `total_samples` (u32,
   exactly `0`) + `gain` (f32, exactly `1.0`) + the full PONG device address
   (`target`\0). **Send this first** — the device only accepts STREAM_DATA after
   a matching BEGIN. Each exact endpoint has its own session. Sent once.
2. **STREAM_DATA (0x31)** — `offset` (u32, running byte offset) + raw
   little-endian PCM16 samples. Keep each packet ≤ 1472 bytes and feed at
   ~real-time (16000 samples/sec) so the 256 ms ring neither overflows nor
   underruns.
3. **STREAM_END (0x32)** — no payload. The buffer drains naturally. The next
   BEGIN to that device waits at least 300 ms.

This library's `playSineSource(freqHz, intensity, durationMs, target, pan, loop)`
generates a source on the MCU, applies source gain and linear pan, then mixes
all matching sources into endpoint-scoped PCM16 stereo @ 16 kHz. The BEGIN gain
is always `1.0`; source intensity is applied in the mixed samples. `tick()` is
cooperative and bounded, and sends no stream packet until a full endpoint is
resolved and classified.
