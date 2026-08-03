// M5Stack_SineStream — stream a synthesized sine to Hapbeat, no kit needed.
//
//   A : 40 Hz  for a fixed duration
//   B : 100 Hz for a fixed duration
//   C : 100 Hz for as long as you hold it
//
// The tone is generated on the M5Stack and streamed as PCM16, so frequency,
// intensity and duration are all live parameters — nothing is stored on the MCU
// and no kit has to be deployed to the device.
//
// The screen shows which button does what, the Wi-Fi it joined, and every
// Hapbeat it is currently streaming to.
//
// Requires: M5Unified. Set your Wi-Fi credentials below.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

const float FREQ_A = 40.0f;    // low: felt more as a push than a buzz
const float FREQ_B = 100.0f;
const float FREQ_C = 100.0f;   // same tone as B, so you can compare timed vs held
const float INTENSITY = 0.6f;  // 0..1, sent as the stream gain
const uint32_t TIMED_MS = 400; // how long A / B play

// Devices drop out of the destination table this long after their last PONG,
// and only a PING makes them answer — so ping well inside that window or
// sending silently falls back to broadcast. See hb.setDeviceTimeout().
const uint32_t PING_INTERVAL_MS = 3000;

Hapbeat hb;

enum Playing { NONE, TIMED_A, TIMED_B, HELD_C };
Playing playing = NONE;
uint32_t stopAtMs = 0;
uint32_t lastPingMs = 0;
uint8_t shownDevices = 0xFF;   // forces the first draw
Playing shownPlaying = NONE;

// ── Screen ────────────────────────────────────────────────────────
// Redrawn only when something changes. Every SPI write is time the loop is not
// calling pumpSine(), and on hardware a per-frame redraw was enough to starve
// the device's ring buffer and make the stream stutter.
void drawButtons() {
  // The three hardware buttons sit under the screen at roughly 1/6, 1/2 and
  // 5/6 of its width, so the labels are placed to line up with them.
  const int h = M5.Display.height(), w = M5.Display.width();
  const int boxW = w / 3 - 8, boxH = 44, y = h - boxH - 4;
  const char* labels[3] = {"A 40Hz", "B 100Hz", "C hold"};
  const Playing states[3] = {TIMED_A, TIMED_B, HELD_C};
  for (int i = 0; i < 3; ++i) {
    const int x = 4 + i * (w / 3);
    const bool active = (playing == states[i]);
    M5.Display.fillRoundRect(x, y, boxW, boxH, 6, active ? TFT_ORANGE : TFT_NAVY);
    M5.Display.drawRoundRect(x, y, boxW, boxH, 6, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, active ? TFT_ORANGE : TFT_NAVY);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(x + 8, y + 14);
    M5.Display.print(labels[i]);
  }
}

void drawScreen() {
  IPAddress ips[4];
  const uint8_t devices = hb.deviceIps(ips, 4);

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(6, 6);
  M5.Display.println("Hapbeat sine");

  M5.Display.setTextSize(1);
  M5.Display.setCursor(6, 30);
  M5.Display.printf("WiFi %s  %ddBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
  M5.Display.setCursor(6, 42);
  M5.Display.printf("self %s\n", WiFi.localIP().toString().c_str());

  M5.Display.setTextSize(2);
  M5.Display.setCursor(6, 60);
  if (devices == 0) {
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.println("broadcast");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(6, 82);
    M5.Display.println("no device found - press RESET after");
    M5.Display.setCursor(6, 94);
    M5.Display.println("powering one on, or check the Wi-Fi");
  } else {
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.printf("unicast x%u\n", devices);
    M5.Display.setTextSize(1);
    int y = 82;
    for (uint8_t i = 0; i < devices; ++i, y += 12) {
      M5.Display.setCursor(6, y);
      M5.Display.printf("-> %s\n", ips[i].toString().c_str());
    }
  }
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  drawButtons();
  shownDevices = devices;
  shownPlaying = playing;
}

// ── Playback ──────────────────────────────────────────────────────
// Non-blocking on purpose: playSine() would block for its whole duration, which
// would freeze the UI and — more importantly — make "play while held" and
// "release to stop" impossible, since neither buttons nor pumpSine() get
// serviced meanwhile.
void startTone(Playing which, float freqHz, uint32_t durationMs) {
  hb.beginSine(freqHz, INTENSITY);   // re-arms if a tone is already running
  playing = which;
  stopAtMs = durationMs ? millis() + durationMs : 0;  // 0 = until released
}

void stopTone() {
  hb.endSine();
  playing = NONE;
  stopAtMs = 0;
}

void setup() {
  M5.begin();
  M5.Display.setTextSize(2);
  M5.Display.println("Hapbeat sine\n\nWiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // streaming: keep the radio awake, or it stutters
  while (WiFi.status() != WL_CONNECTED) delay(200);

  hb.begin(7700, "M5Sine");  // app name shows on the Hapbeat OLED

  M5.Display.println("finding\nHapbeat...");
  // Register every device that answers, so streaming unicasts to all of them
  // instead of broadcasting. Broadcast still works as a fallback, but an AP
  // holds broadcast frames until its next DTIM beacon (100-300 ms) whenever any
  // client on it is power-saving, which a continuous stream hears as dropouts.
  hb.discover(1500);
  drawScreen();
}

void loop() {
  M5.update();
  hb.pumpSine();  // keeps the device ring fed; self-paces, safe to call always
  hb.poll();      // consume PONGs so the destination table stays current

  if (M5.BtnA.wasPressed()) startTone(TIMED_A, FREQ_A, TIMED_MS);
  if (M5.BtnB.wasPressed()) startTone(TIMED_B, FREQ_B, TIMED_MS);
  if (M5.BtnC.wasPressed()) startTone(HELD_C, FREQ_C, 0);
  if (M5.BtnC.wasReleased() && playing == HELD_C) stopTone();

  if (stopAtMs && (int32_t)(millis() - stopAtMs) >= 0) stopTone();

  if (millis() - lastPingMs >= PING_INTERVAL_MS) {
    lastPingMs = millis();
    hb.ping();  // keeps devices in the table (and therefore unicast alive)
  }

  // Repaint only on a real change (device list or which button is lit).
  if (playing != shownPlaying) {
    drawButtons();
    shownPlaying = playing;
  }
  if (hb.deviceCount() != shownDevices) drawScreen();

  delay(2);  // yield to Wi-Fi
}
