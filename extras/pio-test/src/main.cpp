// Multi-board functional test for the Hapbeat Arduino library.
//
// Works on M5 boards with a touch screen (CoreS3, Core2 — tap the on-screen
// buttons) AND boards with physical buttons (Core / Fire / Atom / StickC — use
// BtnA/B/C). CoreS3 has no physical buttons, so this draws three tappable
// on-screen buttons:
//   A -> fire a kit event   (command mode, HAPBEAT_EVENT)
//   B -> stream a 150 Hz sine
//   C -> stream a 400 Hz sine
//
// Fill in secrets.h (copy from secrets.h.example) before flashing.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASS, HAPBEAT_EVENT

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
  const char* labels[3] = {"A: play", "B: sine 80Hz", "C: sine 160Hz"};
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

void doAction(int i) {
  drawButton(i, true);  // visual feedback
  if (g_wifi_ok) {
    if (i == 0)
      hb.play(HAPBEAT_EVENT, 0.7f);
    else if (i == 1)
      hb.playSine(80.0f, 0.8f, 500);
    else
      hb.playSine(160.0f, 0.8f, 500);
  }
  delay(90);
  drawButton(i, false);
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
    hb.discover(1500);  // find the Hapbeat -> unicast streaming (much smoother)
  }
  drawScreen();
}

void loop() {
  M5.update();

  // Physical buttons (Core / Core2 / Fire / Atom / StickC).
  if (M5.BtnA.wasPressed()) doAction(0);
  if (M5.BtnB.wasPressed()) doAction(1);
  if (M5.BtnC.wasPressed()) doAction(2);

  // Touch screen (CoreS3 / Core2). On non-touch boards this never fires.
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    for (int i = 0; i < 3; ++i) {
      if (inButton(g_btns[i], t.x, t.y)) {
        doAction(i);
        break;
      }
    }
  }
}
