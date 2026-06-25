# pio-test — multi-board build/flash harness

PlatformIO harness for verifying the Hapbeat library on real M5 hardware. Lives
in `extras/` (ignored by the Arduino IDE). M5Unified makes one sketch
(`src/main.cpp`) run on every M5 model — only the `board` (chip family) changes
per environment. CoreS3 (no physical buttons) uses on-screen touch buttons;
other boards use BtnA/B/C.

## 1. Set up Wi-Fi (one time)

```bash
cp src/secrets.h.example src/secrets.h     # then edit src/secrets.h
```

`src/secrets.h` is **gitignored** (your credentials are never committed).
`src/secrets.h.example` is the tracked template. Fill in:

```c
#define WIFI_SSID "..."        // 2.4 GHz
#define WIFI_PASS "..."
#define HAPBEAT_EVENT "sample-kit.sine_100hz"   // Button A event (deploy sample-kit in Studio)
```

## 2. Build / flash

```bash
pio run -e cores3                          # build for M5Stack CoreS3
pio run -e cores3 -t upload --upload-port COMx   # flash it
```

Other environments: `atoms3`, `stamps3`, `core`, `core2`, `fire`, `atom`, `stickc`
(e.g. `pio run -e core2 -t upload`).

## 3. Test on the device

- **B / C** → synthesized sine (no kit needed) — fastest "does it buzz" check.
- **A** → plays `HAPBEAT_EVENT` (deploy the `sample-kit` to your Hapbeat in Studio first).

The Hapbeat must be on the same 2.4 GHz Wi-Fi/LAN as the board.
