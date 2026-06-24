// Hapbeat.h — Arduino transport for driving Hapbeat haptic devices.
//
// Works on ESP32-series, ESP8266, and Arduino-compatible Wi-Fi boards (anything
// providing WiFiUDP). Connect Wi-Fi in your sketch first, then call begin().
//
// Level-1: command (fire) mode + a synthesized-sine streaming helper. Stored-WAV
// streaming is intentionally excluded — put waveforms in the kit on the device
// (command mode) or synthesize them (playSine).
#ifndef HAPBEAT_H
#define HAPBEAT_H

#include <Arduino.h>
#include <WiFiUdp.h>

#include "HapbeatProtocol.h"

class Hapbeat {
 public:
  // Open the UDP socket for sending. appName (<=16 chars) shows on the device
  // OLED while connected. Call after Wi-Fi is connected.
  bool begin(uint16_t port = 7700, const char* appName = "");

  // Fire a kit event by id (must exist in the kit deployed to the device via
  // Hapbeat Studio). gain is 0..1. target "" = broadcast to all devices;
  // e.g. "player_1/chest" or "*/chest".
  void play(const char* eventId, float gain = 1.0f, const char* target = "");
  void stop(const char* eventId, const char* target = "");
  void stopAll(const char* target = "");
  void ping();

  // Stream a synthesized sine wave (PCM16, generated on the fly — no stored
  // audio). Blocks for ~durationMs while streaming.
  //   freqHz:    tone frequency (Hz)
  //   intensity: 0..1, sent as the stream gain (device-side mix level)
  void playSine(float freqHz, float intensity, uint32_t durationMs,
                const char* target = "");

  // Tell the device this app is leaving (clears the OLED app name).
  void end();

  // Group id sent in CONNECT_STATUS (level-1 default 0).
  void setGroup(uint8_t group) { _group = group; }

 private:
  WiFiUDP _udp;
  uint16_t _port = 7700;
  uint16_t _seq = 0;
  uint8_t _group = 0;
  bool _ready = false;
  char _appName[hapbeat::MAX_APP_NAME_LEN + 1] = {0};

  uint16_t nextSeq() { return ++_seq; }
  void sendPacket(const uint8_t* buf, size_t len);
  void connectStatus(bool connected);
};

#endif  // HAPBEAT_H
