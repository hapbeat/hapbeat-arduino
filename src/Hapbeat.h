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

  // Discover a Hapbeat on the LAN (broadcast PING -> PONG) and remember its IP.
  // Streaming then unicasts to it, which is far smoother than UDP broadcast
  // (broadcast frames are unacked, low-rate, and lossy on Wi-Fi). Call once
  // after begin(). Returns true if a device replied within timeoutMs.
  bool discover(uint32_t timeoutMs = 1500);
  IPAddress deviceIp() const { return _deviceIp; }
  void setDeviceIp(IPAddress ip) { _deviceIp = ip; }

  // Stream a synthesized sine wave (PCM16, generated on the fly — no stored
  // audio). Blocks for ~durationMs while streaming. One-shot convenience wrapper
  // around beginSine()/pumpSine()/endSine().
  //   freqHz:    tone frequency (Hz)
  //   intensity: 0..1, sent as the stream gain (device-side mix level)
  void playSine(float freqHz, float intensity, uint32_t durationMs,
                const char* target = "");

  // Continuous (open-ended) synthesized-sine stream — for hold-to-play / live
  // control. Call beginSine() once, then pumpSine() frequently (every loop())
  // to keep the device ring filled, and endSine() to stop. Non-blocking.
  void beginSine(float freqHz, float intensity, const char* target = "");
  void pumpSine();
  void endSine();
  bool sineActive() const { return _sineActive; }

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
  IPAddress _deviceIp{(uint32_t)0};  // 0.0.0.0 = not discovered -> broadcast
  char _appName[hapbeat::MAX_APP_NAME_LEN + 1] = {0};

  // Continuous-sine streaming state (beginSine/pumpSine/endSine).
  bool _sineActive = false;
  float _sinePhase = 0.0f;
  float _sineDphase = 0.0f;
  int16_t _sineAmp = 0;
  uint16_t _sineRate = 16000;
  uint32_t _sineSent = 0;        // samples generated so far
  uint32_t _sineByteOffset = 0;  // STREAM_DATA offset (informational)
  uint32_t _sineStartMs = 0;     // pacing reference

  uint16_t nextSeq() { return ++_seq; }
  // Send helpers return false if the packet could not be enqueued (endPacket()
  // == 0, i.e. the lwIP/Wi-Fi TX path was full). Streaming uses this to keep the
  // open-loop buffer estimate honest instead of counting un-sent audio.
  bool sendPacket(const uint8_t* buf, size_t len);                  // broadcast
  bool sendTo(const uint8_t* buf, size_t len, IPAddress ip);        // unicast
  bool streamSend(const uint8_t* buf, size_t len);                  // unicast if discovered
  void connectStatus(bool connected);
};

#endif  // HAPBEAT_H
