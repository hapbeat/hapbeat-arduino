// Multi-board functional test for the Hapbeat Arduino library.
//
// Works on M5 boards with a touch screen (CoreS3, Core2 — tap the on-screen
// buttons) AND boards with physical buttons (Core / Fire / Atom / StickC — use
// BtnA/B/C). CoreS3 has no physical buttons, so this draws three tappable
// on-screen buttons:
//   A -> fire a kit event        (command mode, HAPBEAT_EVENT)
//   B -> sine 80 Hz, one-shot     (tap = ~500 ms burst)
//   C -> sine 160 Hz, hold        (vibrates while held; release to stop)
//
// The same actions can be driven from the PC over Serial (115200, one
// character per command, each answered with one line):
//   a = fire HAPBEAT_EVENT   b = 80 Hz one-shot (~500 ms)   c = toggle 160 Hz hold
//   s = stop all             p = status (devices, stream class, lease, playbacks)
// Library warnings are printed as "WARN: <message>", and a status line is
// printed every 2 s while a sine Playback is not Stopped.
//
// Fill in secrets.h (copy from secrets.h.example) before flashing.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASS, HAPBEAT_EVENT

// Set to 1 to force command packets to broadcast. Streaming still requires a
// full PONG endpoint, so discovery always runs and stream packets stay unicast.
#ifndef FORCE_BROADCAST
#define FORCE_BROADCAST 0
#endif

Hapbeat hb;
bool g_wifi_ok = false;

struct UiButton {
  int x, y, w, h;
  uint16_t color;
  const char* label;
};
UiButton g_btns[3];

static const int HEADER_H = 64;
static const uint32_t STATUS_INTERVAL_MS = 2000;

void layoutButtons() {
  const int W = M5.Display.width();
  const int H = M5.Display.height();
  const int gap = 6;
  const int bh = (H - HEADER_H - gap * 4) / 3;
  const uint16_t colors[3] = {TFT_NAVY, TFT_DARKGREEN, TFT_MAROON};
  const char* labels[3] = {"A: play", "B: 80Hz 1shot", "C: 160Hz HOLD"};
  for (int i = 0; i < 3; ++i) {
    g_btns[i] = {gap, HEADER_H + gap + i * (bh + gap), W - 2 * gap, bh, colors[i], labels[i]};
  }
}

void drawButton(int i, bool active) {
  const UiButton& b = g_btns[i];
  M5.Display.fillRoundRect(b.x, b.y, b.w, b.h, 8, active ? TFT_WHITE : b.color);
  M5.Display.setTextColor(active ? TFT_BLACK : TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(b.x + 10, b.y + b.h / 2 - 8);
  M5.Display.print(b.label);
}

void drawScreen() {
  M5.Display.fillScreen(TFT_BLACK);
  if (g_wifi_ok) {
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(6, 6);
    if ((uint32_t)hb.deviceIp() != 0)
      M5.Display.printf("uni %s", hb.deviceIp().toString().c_str());
    else
      M5.Display.print("no endpoint");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(6, 36);
    M5.Display.printf("A=%s", HAPBEAT_EVENT);
  } else {
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(6, 6);
    M5.Display.print("WiFi FAILED");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(6, 40);
    M5.Display.print("edit secrets.h");
  }
  layoutButtons();
  for (int i = 0; i < 3; ++i) drawButton(i, false);
}

int g_holdBtn = -1;  // index of the button currently driving a hold stream (C)
Hapbeat::Playback g_holdPlayback;
Hapbeat::Playback g_oneshotPlayback;  // B: 80 Hz, ~500 ms
uint32_t g_lastStatusMs = 0;

// ---- Serial status --------------------------------------------------------

const char* className(HapbeatStreamClass value) {
  switch (value) {
    case HapbeatStreamClass::Gen1: return "Gen1";
    case HapbeatStreamClass::Gen2: return "Gen2";
    default: return "Unknown";
  }
}

const char* leaseName(HapbeatLeaseState value) {
  switch (value) {
    case HapbeatLeaseState::Valid: return "valid";
    case HapbeatLeaseState::Unavailable: return "unavailable";
    case HapbeatLeaseState::Superseded: return "superseded";
    default: return "-";
  }
}

const char* statusName(HapbeatPlaybackStatus value) {
  switch (value) {
    case HapbeatPlaybackStatus::Active: return "Active";
    case HapbeatPlaybackStatus::Deferred: return "Deferred";
    default: return "Stopped";
  }
}

const char* reasonName(HapbeatDeferredReason value) {
  switch (value) {
    case HapbeatDeferredReason::NoResolvedEndpoint: return "NoResolvedEndpoint";
    case HapbeatDeferredReason::UnknownClass: return "UnknownClass";
    case HapbeatDeferredReason::LeaseUnavailable: return "LeaseUnavailable";
    case HapbeatDeferredReason::Superseded: return "Superseded";
    default: return "None";
  }
}

void printPlayback(const char* name, const Hapbeat::Playback& playback) {
  Serial.printf(" %s=%s/%s", name, statusName(playback.status()),
                reasonName(playback.deferredReason()));
}

// One line: every resolved device (IP, address, stream class, lease) and the
// state of the two sine Playbacks.
HapbeatStreamEndpointInfo g_endpoints[8];
void printStatus() {
  const uint8_t count = hb.streamEndpoints(g_endpoints, 8);
  Serial.printf("STATUS t=%lu devices=%u", (unsigned long)millis(), (unsigned)count);
  for (uint8_t i = 0; i < count; ++i) {
    Serial.printf(" [%s %s class=%s lease=%s]",
                  g_endpoints[i].ip.toString().c_str(), g_endpoints[i].address,
                  className(g_endpoints[i].streamClass),
                  leaseName(g_endpoints[i].leaseState));
  }
  printPlayback("hold", g_holdPlayback);
  printPlayback("oneshot", g_oneshotPlayback);
  Serial.println();
}

void printWarning(const char* message) {
  Serial.print("WARN: ");
  Serial.println(message);
}

// ---- Actions (buttons and Serial) ------------------------------------------

void playEvent() {
  if (g_wifi_ok) hb.play(HAPBEAT_EVENT, 0.7f);
}

void playOneshot() {
  if (g_wifi_ok)
    g_oneshotPlayback = hb.playSineSource(80.0f, 0.8f, 500, "", 0.0f, false);
}

// A / B: one-shot (fires once per tap). B registers a 500 ms source that
// tick() streams from loop().
void fireOneshot(int i) {
  drawButton(i, true);
  if (i == 0)
    playEvent();
  else  // i == 1
    playOneshot();
  delay(90);
  drawButton(i, false);
}

// C: hold-to-play — register a continuous sine on press, tick it while held.
void startHold(int i) {
  if (g_holdBtn >= 0) return;
  g_holdBtn = i;
  drawButton(i, true);
  if (g_wifi_ok)
    g_holdPlayback = hb.playSineSource(160.0f, 0.8f, 0, "", 0.0f, true);
}

void stopHold() {
  if (g_holdBtn < 0) return;
  if (g_wifi_ok && g_holdPlayback.valid()) g_holdPlayback.stop();
  drawButton(g_holdBtn, false);
  g_holdBtn = -1;
}

void handleSerial() {
  while (Serial.available() > 0) {
    const int command = Serial.read();
    if (command == '\r' || command == '\n' || command == ' ') continue;
    if (!g_wifi_ok && command != 'p') {
      Serial.printf("ERR %c: no Wi-Fi\n", (char)command);
      continue;
    }
    switch (command) {
      case 'a':
        playEvent();
        Serial.printf("OK a play %s\n", HAPBEAT_EVENT);
        break;
      case 'b':
        playOneshot();
        Serial.printf("OK b sine 80Hz 500ms id=%u %s/%s\n",
                      (unsigned)g_oneshotPlayback.id(),
                      statusName(g_oneshotPlayback.status()),
                      reasonName(g_oneshotPlayback.deferredReason()));
        break;
      case 'c':
        if (g_holdBtn >= 0) {
          stopHold();
          Serial.println("OK c hold off");
        } else {
          startHold(2);
          Serial.printf("OK c hold on id=%u %s/%s\n",
                        (unsigned)g_holdPlayback.id(),
                        statusName(g_holdPlayback.status()),
                        reasonName(g_holdPlayback.deferredReason()));
        }
        break;
      case 's':
        hb.stopAll();
        if (g_holdBtn >= 0) stopHold();
        Serial.println("OK s stopAll");
        break;
      case 'p':
        printStatus();
        break;
      default:
        Serial.printf("ERR unknown command '%c' (a b c s p)\n", (char)command);
        break;
    }
  }
}

bool inButton(const UiButton& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(6, 6);
  M5.Display.print("Hapbeat\nWiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // keep the radio awake -> low-jitter UDP streaming
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  g_wifi_ok = (WiFi.status() == WL_CONNECTED);

  if (g_wifi_ok) {
    hb.begin(7700, "M5Test");
    hb.setWarningHandler(printWarning);
    hb.setBroadcastOnly(FORCE_BROADCAST);
    // Find the Hapbeat endpoint required by streaming. Retry a few times: the
    // device may not have answered the first broadcast PING yet.
    for (int i = 0; i < 3 && (uint32_t)hb.deviceIp() == 0; ++i) {
      hb.discover(800);
      if ((uint32_t)hb.deviceIp() == 0) delay(150);
    }
  }
  drawScreen();
  Serial.printf("READY wifi=%s ip=%s (a b c s p)\n", g_wifi_ok ? "ok" : "failed",
                WiFi.localIP().toString().c_str());
}

void loop() {
  M5.update();

#if HB_SINGLE_BUTTON
  // Single-button boards (M5 ATOM / AtomS3 / StampS3): the lone button drives
  // the kit-free 160 Hz sine (HOLD), so a first test needs no kit deployed.
  if (M5.BtnA.wasPressed()) startHold(2);
  if (M5.BtnA.wasReleased()) stopHold();
#else
  // Multi-button boards: A=play (command) / B=80Hz 1shot / C=160Hz HOLD.
  if (M5.BtnA.wasPressed()) fireOneshot(0);
  if (M5.BtnB.wasPressed()) fireOneshot(1);
  if (M5.BtnC.wasPressed()) startHold(2);
  if (M5.BtnC.wasReleased()) stopHold();

  // Touch screen (CoreS3 / Core2). On non-touch boards this never fires.
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (inButton(g_btns[0], t.x, t.y)) fireOneshot(0);
    else if (inButton(g_btns[1], t.x, t.y)) fireOneshot(1);
    else if (inButton(g_btns[2], t.x, t.y)) startHold(2);
  }
  if (t.wasReleased() && g_holdBtn == 2) stopHold();
#endif

  handleSerial();

  // Service the hold source and all other endpoint-scoped streams.
  if (g_wifi_ok) hb.tick();

  // The library has no session start/end callback, so while a sine Playback
  // is not Stopped the state is printed periodically instead.
  const bool streaming = !g_holdPlayback.isStopped() || !g_oneshotPlayback.isStopped();
  if (streaming && millis() - g_lastStatusMs >= STATUS_INTERVAL_MS) {
    g_lastStatusMs = millis();
    printStatus();
  }
}
