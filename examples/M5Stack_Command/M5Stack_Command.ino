// M5Stack_Command — press a button, fire a Hapbeat kit event.
//
//   A : sample-kit.sine_100hz at gain 0.6
//   B : sample-kit.sine_50hz  at gain 0.3
//   C : stop everything
//
// The simplest integration: the M5Stack sends a ~30-byte PLAY trigger over
// Wi-Fi and the haptic waveform lives in the kit on the device (deploy it with
// Hapbeat Studio). Nothing is stored on the MCU.
//
// The screen shows which button does what, the Wi-Fi it joined, and every
// Hapbeat it is currently sending to.
//
// Requires: M5Unified. Set your Wi-Fi credentials below, and use event ids that
// exist in your deployed kit.

#include <M5Unified.h>
#include <WiFi.h>
#include <Hapbeat.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// Must exist in the kit deployed to your Hapbeat via Hapbeat Studio.
const char* EVENT_A = "sample-kit.sine_100hz";
const char* EVENT_B = "sample-kit.sine_50hz";

// Devices drop out of the destination table this long after their last PONG,
// and only a PING makes them answer — so ping well inside that window or
// commands silently fall back to broadcast. See hb.setDeviceTimeout().
const uint32_t PING_INTERVAL_MS = 3000;

Hapbeat hb;
uint32_t lastPingMs = 0;
uint8_t shownDevices = 0xFF;  // forces the first draw

void drawButtons() {
  // The three hardware buttons sit under the screen at roughly 1/6, 1/2 and
  // 5/6 of its width, so the labels are placed to line up with them.
  const int h = M5.Display.height(), w = M5.Display.width();
  const int boxW = w / 3 - 8, boxH = 44, y = h - boxH - 4;
  const char* labels[3] = {"A 100Hz", "B 50Hz", "C stop"};
  for (int i = 0; i < 3; ++i) {
    const int x = 4 + i * (w / 3);
    M5.Display.fillRoundRect(x, y, boxW, boxH, 6, TFT_NAVY);
    M5.Display.drawRoundRect(x, y, boxW, boxH, 6, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
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
  M5.Display.println("Hapbeat kit");

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
    M5.Display.println("no device found - commands still go out,");
    M5.Display.setCursor(6, 94);
    M5.Display.println("just not unicast");
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
}

void setup() {
  M5.begin();
  M5.Display.setTextSize(2);
  M5.Display.println("Hapbeat kit\n\nWiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  hb.begin(7700, "M5Stack");  // app name shows on the Hapbeat OLED

  M5.Display.println("finding\nHapbeat...");
  // Register every device that answers, so commands unicast to all of them.
  // Broadcast still works as a fallback, but an AP holds broadcast frames until
  // its next DTIM beacon (100-300 ms) whenever any client on it is
  // power-saving, which shows up as a late buzz.
  hb.discover(1500);
  drawScreen();
}

void loop() {
  M5.update();
  hb.poll();  // consume PONGs so the destination table stays current

  if (M5.BtnA.wasPressed()) hb.play(EVENT_A, 0.6f);
  if (M5.BtnB.wasPressed()) hb.play(EVENT_B, 0.3f);
  if (M5.BtnC.wasPressed()) hb.stopAll();

  if (millis() - lastPingMs >= PING_INTERVAL_MS) {
    lastPingMs = millis();
    hb.ping();  // keeps devices in the table (and therefore unicast alive)
  }

  if (hb.deviceCount() != shownDevices) drawScreen();

  delay(2);
}
