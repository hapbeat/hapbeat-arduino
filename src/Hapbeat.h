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
  // Hapbeat Studio). gain is 0..1. target "" = all devices; e.g.
  // "player_1/pos_neck" or "*/pos_neck".
  //
  // Matching is POSITIONAL: segment i of the target is compared with segment i
  // of the device address, "*" stands for one whole segment (not a substring:
  // "pos_*" is compared literally), and a short target front-matches. Device
  // addresses are always "player_<N>/<position>/group_<M>", so to select a
  // group alone you must fill the earlier slots: "*/*/group_2". "group_2" on
  // its own is compared against the player slot and never matches.
  //
  // pan is the left/right balance: -1 = left only, 0 = centre, +1 = right only
  // (clamped to that range). It is a balance, not true panning — centre leaves
  // both sides at full gain, and a device with a single actuator ignores it.
  void play(const char* eventId, float gain = 1.0f, const char* target = "",
            float pan = 0.0f);
  void stop(const char* eventId, const char* target = "");
  void stopAll(const char* target = "");
  void ping();

  // Discover Hapbeats on the LAN (broadcast PING -> PONG) and remember their
  // IPs. Commands and streaming then unicast to them, which is both smoother
  // and lower-latency than UDP broadcast: broadcast frames are unacked and
  // low-rate, and an AP holds them until its next DTIM beacon (100-300 ms)
  // whenever any client on it is power-saving. Call once after begin().
  //
  // Blocks for the whole timeout (it collects every reply, not just the first)
  // and returns true if at least one device replied. Devices drop out of the
  // table if they go quiet for longer than setDeviceTimeout(), so call ping()
  // every few seconds in loop() to keep unicast alive — once the table is
  // empty, sends fall back to broadcast.
  bool discover(uint32_t timeoutMs = 1500);

  // Consume any PONGs that have arrived, without blocking. Sends do this
  // themselves (rate-limited), so calling it is optional; do call it from
  // loop() if you want the device table to track changes promptly.
  void poll();

  // Devices heard from within the liveness window (plus any pinned by
  // setDeviceIp). 0 = sends fall back to broadcast.
  uint8_t deviceCount();
  // First known device, or 0.0.0.0 if none. With several devices, prefer
  // deviceCount(); this exists for single-device sketches.
  IPAddress deviceIp();
  // Pin a device by IP, skipping discovery. Pinned entries never expire.
  // Pass 0.0.0.0 to clear them.
  void setDeviceIp(IPAddress ip);
  // Copy the current unicast destinations into `out` (up to `maxOut`), and
  // return how many were written. Useful when a device is power-cycled: it
  // shows whether it came back on the same IP or left a stale entry behind.
  uint8_t deviceIps(IPAddress* out, uint8_t maxOut);

  // How long a device stays a unicast destination after its last PONG
  // (default 15 s = three 5 s pings). Pinned entries ignore this.
  void setDeviceTimeout(uint32_t ms) { _deviceTimeoutMs = ms; }

  // Force broadcast for everything, as before unicast routing existed.
  // Worth it when many devices must fire in lockstep: one broadcast reaches
  // them all at once, whereas N unicasts go out one after another (~3-8 ms
  // spread over 20 devices). Below ~10 devices unicast is the better default
  // — it is acked, retried, and sent at the full link rate.
  void setBroadcastOnly(bool on) { _broadcastOnly = on; }

  // Stream a synthesized sine wave (PCM16, generated on the fly — no stored
  // audio). Blocks for ~durationMs while streaming. One-shot convenience wrapper
  // around beginSine()/pumpSine()/endSine().
  //   freqHz:    tone frequency (Hz)
  //   intensity: 0..1, sent as the stream gain (device-side mix level)
  void playSine(float freqHz, float intensity, uint32_t durationMs,
                const char* target = "");

  // Continuous (open-ended) synthesized-sine stream — for hold-to-play / live
  // control. Call beginSine() once, then pumpSine() frequently (every loop())
  // to keep the device ring filled, and endSine() to stop.
  //
  // beginSine() blocks for a few tens of ms while it fills the device's buffer
  // (see its comment); pumpSine()/endSine() do not block. Calling beginSine()
  // again while a tone is playing re-arms it — do NOT endSine() first just to
  // restart, since a STREAM_END sent over a different route can overtake the
  // new STREAM_BEGIN and kill the stream that just started.
  void beginSine(float freqHz, float intensity, const char* target = "");
  void pumpSine();
  void endSine();
  bool sineActive() const { return _sineActive; }

  // Tell the device this app is leaving (clears the OLED app name).
  void end();

  // Group id sent in CONNECT_STATUS (level-1 default 0).
  void setGroup(uint8_t group) { _group = group; }

 private:
  // Known devices. Fixed size — no dynamic allocation on an MCU. 8 covers the
  // sketch-driven setups this library targets; beyond that, broadcast (see
  // setBroadcastOnly) is the better fit anyway.
  static const uint8_t MAX_DEVICES = 8;
  struct KnownDevice {
    IPAddress ip{(uint32_t)0};  // 0.0.0.0 = free slot
    uint32_t lastPongMs = 0;
    bool pinned = false;  // from setDeviceIp(): never expires
    uint8_t fails = 0;    // consecutive send failures (see kMaxSendFails)
  };
  // Consecutive failed unicasts before a destination is dropped. A device that
  // was power-cycled onto a different IP leaves a dead entry behind, and every
  // send to it fails while ARP retries -- which starves the TX pool the LIVE
  // device's audio needs. Dropping it restores the stream (and PONGs re-add the
  // device under its new IP within one ping). Reset on any success, so a
  // transient TX-pool hiccup never evicts a healthy device.
  static const uint8_t kMaxSendFails = 5;

  WiFiUDP _udp;
  uint16_t _port = 7700;
  uint16_t _seq = 0;
  uint8_t _group = 0;
  bool _ready = false;
  KnownDevice _devices[MAX_DEVICES];
  uint32_t _deviceTimeoutMs = 15000;  // 3 pings at the usual 5 s interval
  uint32_t _lastPollMs = 0;
  bool _broadcastOnly = false;
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
  bool sendRouted(const uint8_t* buf, size_t len);                  // unicast if known, else broadcast
  void connectStatus(bool connected);

  // Device table
  void pollPongs();                     // drain pending PONGs (non-blocking)
  void pollPongsThrottled();            // pollPongs() at most every 100 ms
  void rememberDevice(IPAddress ip, bool pinned = false);
  bool isAlive(const KnownDevice& d) const;
  uint8_t aliveCount() const;           // no polling (unlike deviceCount())
  uint32_t sineBuffered() const;        // samples the device should still hold
};

#endif  // HAPBEAT_H
