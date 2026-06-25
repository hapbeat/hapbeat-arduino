// M5Stack_SineStream — press a button, stream a synthesized sine wave.
//
// Instead of triggering a stored kit clip, this generates a sine tone on the
// M5Stack and streams it as PCM16 to the Hapbeat device. Nothing is stored on
// the MCU — the tone is synthesized live, so frequency and intensity are free
// expressive parameters.
//
//   hb.playSine(frequencyHz, intensity /*0..1*/, durationMs)
//
// Requires: M5Unified library + Wi-Fi credentials below.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

Hapbeat hb;

void setup() {
  M5.begin();
  M5.Display.setTextSize(2);
  M5.Display.println("Hapbeat sine\nconnecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // streaming: keep the radio awake to avoid choppiness
  while (WiFi.status() != WL_CONNECTED) delay(200);

  hb.begin(7700, "M5Sine");

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("Hapbeat sine\nA: 160Hz strong\nB: 400Hz soft");
}

void loop() {
  M5.update();
  // playSine(frequencyHz, intensity 0..1, durationMs) — pure synthesis, no WAV.
  // BtnA = 160 Hz: the no-kit-needed default, and the one button single-button
  // boards (M5 ATOM etc.) have — so it works as a first test on any board.
  if (M5.BtnA.wasPressed()) hb.playSine(160.0f, 0.7f, 400);  // low, strong
  if (M5.BtnB.wasPressed()) hb.playSine(400.0f, 0.4f, 300);  // high, soft
}
