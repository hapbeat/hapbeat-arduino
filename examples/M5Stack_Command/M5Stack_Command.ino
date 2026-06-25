// M5Stack_Command — press a button, fire a Hapbeat kit event.
//
// The simplest integration: Button A / B send a PLAY command over Wi-Fi to a
// Hapbeat device on the same LAN. The haptic waveform lives in the kit on the
// device (deploy it with Hapbeat Studio) — the M5Stack just sends the trigger.
//
// Requires: M5Unified library. Set your Wi-Fi credentials below, and use event
// ids that exist in your deployed kit.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

Hapbeat hb;

void setup() {
  M5.begin();
  M5.Display.setTextSize(2);
  M5.Display.println("Hapbeat\nconnecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  hb.begin(7700, "M5Stack");  // app name shows on the Hapbeat OLED

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("Hapbeat ready\nA: impact\nB: click");
}

void loop() {
  M5.update();
  // Event ids must exist in the kit deployed to your Hapbeat device.
  if (M5.BtnA.wasPressed()) hb.play("sample-kit.sine_100hz", 0.6f);
  if (M5.BtnB.wasPressed()) hb.play("sample-kit.sine_50hz", 0.3f);
}
