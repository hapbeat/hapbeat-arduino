// Verify_Unicast — release check for unicast sending. Flash it and listen.
//
// Streams a CONTINUOUS 100 Hz sine and alternates the send path every 5 s:
//
//   [B] BROADCAST — expected to stutter every 100-300 ms. A Wi-Fi AP buffers
//       group-addressed frames until its next DTIM beacon whenever ANY client
//       on it is power-saving, and broadcast frames get no MAC ACK/retry.
//   [A] UNICAST   — expected to be smooth.
//
// A continuous low sine is the harshest case for the transport: every late or
// lost packet is audible.
//
// The current phase, device count and countdown go to the M5 screen when
// M5Unified is installed, and always to Serial at 115200 — so this runs on any
// ESP32/ESP8266 with Wi-Fi, with or without a display library.
//
// With more than one Hapbeat powered on, watch that BOTH keep buzzing during
// the [A] phase: that is the multi-device unicast path (v0.2.0 replaced the
// old single-_deviceIp table, which only ever reached the first device found).

#include <WiFi.h>       // ESP8266: <ESP8266WiFi.h>
#include <Hapbeat.h>

// Optional: screen output on M5 boards. Nothing else in this sketch depends on
// it, so boards without the library still build and report over Serial.
#if __has_include(<M5Unified.h>)
  #include <M5Unified.h>
  #define HB_HAS_SCREEN 1
#endif

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

const float TONE_HZ = 100.0f;
const float INTENSITY = 0.6f;   // raise if you can barely feel it
const uint32_t PHASE_MS = 5000;   // long enough to hear, short enough to compare

Hapbeat hb;
bool broadcastPhase = true;     // start with the "before" condition
uint32_t phaseStartMs = 0;
uint32_t lastPingMs = 0;
uint32_t lastScreenMs = 0;

#ifdef HB_HAS_SCREEN
// Phase banner. Colour carries the expectation at a glance so you can keep your
// eyes on the device you are holding: red = the condition that should stutter.
void drawPhase(uint8_t devices, const IPAddress* ips) {
  const uint16_t bg = broadcastPhase ? TFT_MAROON : TFT_DARKGREEN;
  M5.Display.fillScreen(bg);
  M5.Display.setTextColor(TFT_WHITE, bg);

  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 10);
  M5.Display.println(broadcastPhase ? "BROADCAST" : "UNICAST");

  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 50);
  M5.Display.println(broadcastPhase ? "expect stutter" : "expect smooth");

  M5.Display.setCursor(8, 80);
  M5.Display.printf("devices: %u\n", devices);

  // The destination list is the thing to look at after a power cycle: a device
  // that came back on a different IP shows up as an extra (dead) line here.
  int y = 104;
  for (uint8_t i = 0; i < devices && i < 3; ++i, y += 20) {
    M5.Display.setCursor(8, y);
    M5.Display.printf("-> %s", ips[i].toString().c_str());
  }
  if (devices == 0) {
    M5.Display.setCursor(8, y);
    M5.Display.print("none known ->");
    y += 20;
    M5.Display.setCursor(8, y);
    M5.Display.print("broadcasting");
    y += 20;
  }
  M5.Display.setCursor(8, y);
  M5.Display.printf("%d Hz  gain %.1f", (int)TONE_HZ, INTENSITY);

  // Static part of the countdown line; drawCountdown() fills in the digits.
  M5.Display.setCursor(8, M5.Display.height() - 24);
  M5.Display.print("switching in    s");
}

// Only the digits are repainted each second; drawPhase() paints the label once.
// Every SPI write is time the loop is not calling pumpSine(), and a full-line
// redraw measurably increased dropped sends on hardware.
void drawCountdown() {
  const uint16_t bg = broadcastPhase ? TFT_MAROON : TFT_DARKGREEN;
  const uint32_t left = (PHASE_MS - (millis() - phaseStartMs)) / 1000;
  M5.Display.setTextColor(TFT_WHITE, bg);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8 + 13 * 12, M5.Display.height() - 24);  // after "switching in "
  M5.Display.printf("%2lu", (unsigned long)left);
}
#endif

void startPhase() {
  hb.endSine();
  hb.setBroadcastOnly(broadcastPhase);
  IPAddress ips[4];
  const uint8_t devices = hb.deviceIps(ips, 4);
  Serial.printf("\n[%s] devices=%u  (%s)\n",
                broadcastPhase ? "B] BROADCAST" : "A] UNICAST  ", devices,
                broadcastPhase ? "expect stutter" : "expect smooth");
  // Printing the actual destinations is what tells you, after a power cycle,
  // whether the device came back on the same IP or left a stale entry behind.
  for (uint8_t i = 0; i < devices; ++i)
    Serial.printf("    -> %s\n", ips[i].toString().c_str());
#ifdef HB_HAS_SCREEN
  drawPhase(devices, ips);
#endif
  hb.beginSine(TONE_HZ, INTENSITY);
  phaseStartMs = millis();
}

void setup() {
#ifdef HB_HAS_SCREEN
  M5.begin();
  M5.Display.setTextSize(2);
  M5.Display.println("Hapbeat\nunicast check\n\nWiFi...");
#endif
  Serial.begin(115200);
  delay(300);
  Serial.println("\nHapbeat unicast release check");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // streaming: keep the radio awake
  while (WiFi.status() != WL_CONNECTED) delay(200);
  Serial.printf("wifi ok, ip=%s\n", WiFi.localIP().toString().c_str());

  hb.begin(7700, "VerifyUni");

  // Free heap is worth watching: UDP sends fail with ENOMEM when it runs low,
  // and a display library costs tens of KB.
  Serial.printf("free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());

  // Collects EVERY device that replies, not just the first (blocks ~1.5 s).
  const bool found = hb.discover(1500);
  Serial.printf("discover: %s (%u device(s))\n",
                found ? "ok" : "NOTHING FOUND -- will broadcast",
                hb.deviceCount());
  if (!found) {
    Serial.println("  check: same Wi-Fi as the Hapbeat? AP client isolation off?");
  }
  startPhase();
}

void loop() {
  hb.pumpSine();   // keeps the device ring fed; self-paces
  hb.poll();       // consume PONGs even during the broadcast-only phase

  // Devices age out of the table after 15 s without a PONG, and only a PING
  // makes them reply. Without this, unicast would silently fall back to
  // broadcast partway through and the A/B would compare nothing.
  if (millis() - lastPingMs >= 3000) {
    lastPingMs = millis();
    hb.ping();
  }

#ifdef HB_HAS_SCREEN
  // Once a second, and only the countdown line — a full redraw mid-stream would
  // block the SPI bus long enough to starve the device ring.
  if (millis() - lastScreenMs >= 1000) {
    lastScreenMs = millis();
    drawCountdown();
  }
#endif

  if (millis() - phaseStartMs >= PHASE_MS) {
    broadcastPhase = !broadcastPhase;
    startPhase();
  }
  delay(2);  // yield to Wi-Fi
}
