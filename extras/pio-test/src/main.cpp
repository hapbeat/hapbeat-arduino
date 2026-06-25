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
// Fill in secrets.h (copy from secrets.h.example) before flashing.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASS, HAPBEAT_EVENT

// Set to 1 to force the UDP-broadcast path (skip discovery) so you can A/B it
// against unicast on the same hardware/network. 0 = discover + unicast (smoother).
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
      M5.Display.print("broadcast (no dev)");
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

// A / B: one-shot (fires once per tap; B blocks ~500 ms while streaming).
void fireOneshot(int i) {
  drawButton(i, true);
  if (g_wifi_ok) {
    if (i == 0)
      hb.play(HAPBEAT_EVENT, 0.7f);
    else  // i == 1
      hb.playSine(80.0f, 0.8f, 500);
  }
  delay(90);
  drawButton(i, false);
}

// C: hold-to-play — start a continuous sine on press, pump it while held.
void startHold(int i) {
  if (g_holdBtn >= 0) return;
  g_holdBtn = i;
  drawButton(i, true);
  if (g_wifi_ok) hb.beginSine(160.0f, 0.8f);
}

void stopHold() {
  if (g_holdBtn < 0) return;
  if (g_wifi_ok) hb.endSine();
  drawButton(g_holdBtn, false);
  g_holdBtn = -1;
}

bool inButton(const UiButton& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void setup() {
  auto cfg = M5.config();
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
#if !FORCE_BROADCAST
    // Find the Hapbeat -> unicast streaming (Wi-Fi MAC-ACK + retry = much
    // smoother). Retry a few times: the device may not have answered the first
    // broadcast PING yet. Falls back to broadcast if nothing replies.
    for (int i = 0; i < 3 && (uint32_t)hb.deviceIp() == 0; ++i) {
      hb.discover(800);
      if ((uint32_t)hb.deviceIp() == 0) delay(150);
    }
#endif
  }
  drawScreen();
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

  // Keep the hold stream's ring topped up while a hold is active.
  if (hb.sineActive()) hb.pumpSine();
}
