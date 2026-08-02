// Verify_Unicast — release check for unicast sending. Flash it and listen.
//
// Streams a CONTINUOUS 100 Hz sine and alternates the send path every 15 s:
//
//   [B] BROADCAST — expected to stutter every 100-300 ms. A Wi-Fi AP buffers
//       group-addressed frames until its next DTIM beacon whenever ANY client
//       on it is power-saving, and broadcast frames get no MAC ACK/retry.
//   [A] UNICAST   — expected to be smooth.
//
// A continuous low sine is the harshest case for the transport: every late or
// lost packet is audible.
//
// No display library needed — progress goes to Serial at 115200. Works on any
// ESP32/ESP8266 board with Wi-Fi.
//
// With more than one Hapbeat powered on, watch that BOTH keep buzzing during
// the [A] phase: that is the multi-device unicast path (v0.2.0 replaced the
// old single-_deviceIp table, which only ever reached the first device found).

#include <WiFi.h>       // ESP8266: <ESP8266WiFi.h>
#include <Hapbeat.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

const float TONE_HZ = 100.0f;
const float INTENSITY = 0.6f;   // raise if you can barely feel it
const uint32_t PHASE_MS = 15000;

Hapbeat hb;
bool broadcastPhase = true;     // start with the "before" condition
uint32_t phaseStartMs = 0;
uint32_t lastPingMs = 0;

void startPhase() {
  hb.endSine();
  hb.setBroadcastOnly(broadcastPhase);
  Serial.printf("\n[%s] devices=%u  (%s)\n",
                broadcastPhase ? "B] BROADCAST" : "A] UNICAST  ",
                hb.deviceCount(),
                broadcastPhase ? "expect stutter" : "expect smooth");
  hb.beginSine(TONE_HZ, INTENSITY);
  phaseStartMs = millis();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nHapbeat unicast release check");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // streaming: keep the radio awake
  while (WiFi.status() != WL_CONNECTED) delay(200);
  Serial.printf("wifi ok, ip=%s\n", WiFi.localIP().toString().c_str());

  hb.begin(7700, "VerifyUni");

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

  // Devices age out of the table after 15 s without a PONG, and only a PING
  // makes them reply. Without this, unicast would silently fall back to
  // broadcast partway through and the A/B would compare nothing.
  if (millis() - lastPingMs >= 3000) {
    lastPingMs = millis();
    hb.ping();
  }

  if (millis() - phaseStartMs >= PHASE_MS) {
    broadcastPhase = !broadcastPhase;
    startPhase();
  }
  delay(2);  // yield to Wi-Fi
}
