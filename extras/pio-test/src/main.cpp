// Multi-board functional test for the Hapbeat Arduino library.
//
// M5Unified makes this one sketch run on every M5 model. Buttons (touch on
// CoreS3, physical on Core/Core2, single BtnA on Atom/StickC):
//   A -> fire a kit event   (command mode)
//   B -> stream a 150 Hz sine (strong)
//   C -> stream a 400 Hz sine (soft)   [boards with a 3rd button]
//
// Fill in secrets.h (copy from secrets.h.example) before flashing.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASS, HAPBEAT_EVENT

Hapbeat hb;

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setTextSize(2);
  M5.Display.println("Hapbeat test");
  M5.Display.println("WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  if (WiFi.status() != WL_CONNECTED) {
    M5.Display.println("WiFi FAILED\nedit secrets.h");
    return;
  }

  hb.begin(7700, "M5Test");  // app name shows on the Hapbeat OLED
  M5.Display.printf("Hapbeat ready\nIP %s\n\nA: play\nB: sine 150Hz\nC: sine 400Hz\n",
                    WiFi.localIP().toString().c_str());
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) hb.play(HAPBEAT_EVENT, 0.7f);
  if (M5.BtnB.wasPressed()) hb.playSine(150.0f, 0.7f, 400);
  if (M5.BtnC.wasPressed()) hb.playSine(400.0f, 0.4f, 300);
}
