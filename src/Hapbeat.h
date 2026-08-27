// Hapbeat.h — Arduino transport for driving Hapbeat haptic devices.
//
// Works on ESP32-series, ESP8266, and Arduino-compatible Wi-Fi boards (anything
// providing WiFiUDP). Connect Wi-Fi in your sketch first, then call begin().
// Streaming uses a fixed-capacity, endpoint-scoped mixer. It does not allocate
// on the heap and never broadcasts stream packets.
#ifndef HAPBEAT_H
#define HAPBEAT_H

#include <Arduino.h>
#include <WiFiUdp.h>

#include "HapbeatProtocol.h"

class Hapbeat;

enum class HapbeatPlaybackStatus : uint8_t {
  Deferred,
  Active,
  Stopped,
};

enum class HapbeatDeferredReason : uint8_t {
  None,
  NoResolvedEndpoint,
};

// Handle for one logical synthesized-sine source. It lives in a fixed registry
// entry and carries a generation token, so source-slot recycling cannot
// retarget an older handle.
class HapbeatPlayback {
 public:
  HapbeatPlayback() = default;
  uint16_t id() const { return _id; }
  bool valid() const {
    return _allocated && _hub != nullptr && _id != 0 && _generation != 0;
  }
  HapbeatPlaybackStatus status() const;
  HapbeatDeferredReason deferredReason() const;
  float gain() const { return _gain; }
  float pan() const { return _pan; }
  bool loop() const { return _loop; }

  void setGain(float value);
  void setPan(float value);
  void setLoop(bool value);

  // Stop only this source. Repeated calls are harmless.
  void stop();

  bool isActive() const { return status() == HapbeatPlaybackStatus::Active; }
  bool isStopped() const { return status() == HapbeatPlaybackStatus::Stopped; }

 private:
  friend class Hapbeat;
  Hapbeat* _hub = nullptr;
  uint8_t _slot = 0;
  uint16_t _id = 0;
  uint64_t _generation = 0;
  bool _allocated = false;
  float _gain = 1.0f;
  float _pan = 0.0f;
  bool _loop = false;

  void reset(Hapbeat* hub, uint8_t slot, uint16_t id, uint64_t generation,
             float gain, float pan, bool loop);
};

class Hapbeat {
 public:
  using Playback = HapbeatPlayback;
  using PlaybackStatus = HapbeatPlaybackStatus;
  using DeferredReason = HapbeatDeferredReason;

  // Open the UDP socket for sending. appName (<=16 chars) shows on the device
  // OLED while connected. Call after Wi-Fi is connected.
  bool begin(uint16_t port = 7700, const char* appName = "");

  // Fire a kit event by id (must exist in the kit deployed to the device via
  // Hapbeat Studio). gain is 0..1. target "" = all devices; pan is the
  // left/right balance (-1 = left only, 0 = centre, +1 = right only).
  void play(const char* eventId, float gain = 1.0f, const char* target = "",
            float pan = 0.0f);
  void stop(const char* eventId, const char* target = "");
  void stopAll(const char* target = "");
  void ping();

  // Discover Hapbeats on the LAN (broadcast PING -> PONG) and remember their
  // IP, UDP port, and complete device address. Commands then unicast to them.
  bool discover(uint32_t timeoutMs = 1500);

  // Consume any PONGs that have arrived, without blocking. tick() also calls
  // this while streaming, so explicit polling is optional.
  void poll();

  uint8_t deviceCount();
  IPAddress deviceIp();
  void setDeviceIp(IPAddress ip);
  uint8_t deviceIps(IPAddress* out, uint8_t maxOut);

  // How long a device stays a command destination after its last PONG.
  void setDeviceTimeout(uint32_t ms) { _deviceTimeoutMs = ms; }

  // Force broadcast for command packets. Streaming always requires a resolved
  // PONG endpoint and ignores this setting; it never broadcasts.
  void setBroadcastOnly(bool on) { _broadcastOnly = on; }

  // Register a synthesized sine source and return its independently controllable
  // handle. durationMs == 0 means open-ended; loop repeats a finite source.
  // The source is generated as 16 kHz mono and mixed as canonical 16 kHz stereo
  // PCM16 by the endpoint session. Call tick() frequently from loop().
  Playback* playSineSource(float freqHz, float intensity,
                           uint32_t durationMs = 0,
                           const char* target = "", float pan = 0.0f,
                           bool loop = false);

  // Blocking convenience wrapper. It registers through the same source pool and
  // StreamHub state machine as playSineSource().
  void playSine(float freqHz, float intensity, uint32_t durationMs,
                const char* target = "");

  // Cooperative stream reconciliation, mixing, packet pacing, and PONG intake.
  void tick();

  // Tell the device this app is leaving (clears the OLED app name).
  void end();

  // Group id sent in CONNECT_STATUS (level-1 default 0).
  void setGroup(uint8_t group) { _group = group; }

 private:
  friend class HapbeatPlayback;
  static const uint8_t MAX_DEVICES = 8;
  static const uint8_t MAX_STREAM_SOURCES = 8;
  static const uint8_t MAX_STREAM_SESSIONS = 8;
  static const uint8_t MAX_END_RECORDS = 8;
  static const uint8_t MAX_PLAYBACK_HANDLES = 32;
  static const uint16_t STREAM_SAMPLE_RATE = 16000;
  static const uint8_t STREAM_CHANNELS = 2;
  static const uint16_t STREAM_CHUNK_FRAMES = 256;
  static const uint32_t STREAM_SEND_AHEAD_FRAMES = 2560;  // 160 ms
  static const uint32_t SESSION_LINGER_MS = 310;
  static const uint8_t MAX_CHUNKS_PER_TICK = 8;

  struct KnownDevice {
    IPAddress ip{(uint32_t)0};
    uint16_t port = 7700;
    uint32_t lastPongMs = 0;
    bool pinned = false;
    uint8_t fails = 0;
    bool used = false;
    char address[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};
  };

  struct StreamEndpoint {
    IPAddress ip{(uint32_t)0};
    uint16_t port = 7700;
    char address[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};
  };

  struct SineSource {
    bool used = false;
    uint16_t id = 0;
    uint64_t generation = 0;
    uint32_t phaseStep = 0;
    int16_t amplitude = 0;
    uint32_t totalFrames = 0;
    char target[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};
    HapbeatPlaybackStatus status = HapbeatPlaybackStatus::Stopped;
    HapbeatDeferredReason reason = HapbeatDeferredReason::None;
    float gain = 1.0f;
    float pan = 0.0f;
    bool loop = false;
  };

  struct StreamSession {
    bool used = false;
    StreamEndpoint endpoint;
    bool beginSent = false;
    bool attached[MAX_STREAM_SOURCES] = {false};
    bool completed[MAX_STREAM_SOURCES] = {false};
    uint32_t cursors[MAX_STREAM_SOURCES] = {0};
    uint32_t byteOffset = 0;
    uint64_t framesSent = 0;
    uint32_t startedMs = 0;
    uint32_t emptySinceMs = 0;
    bool empty = false;
  };

  struct EndRecord {
    bool used = false;
    StreamEndpoint endpoint;
    uint32_t endedMs = 0;
  };

  WiFiUDP _udp;
  uint16_t _port = 7700;
  uint16_t _seq = 0;
  uint16_t _nextPlaybackId = 0;
  uint64_t _nextPlaybackGeneration = 0;
  uint8_t _group = 0;
  bool _ready = false;
  KnownDevice _devices[MAX_DEVICES];
  uint32_t _deviceTimeoutMs = 15000;
  uint32_t _lastPollMs = 0;
  bool _broadcastOnly = false;
  char _appName[hapbeat::MAX_APP_NAME_LEN + 1] = {0};

  SineSource _sources[MAX_STREAM_SOURCES];
  StreamSession _sessions[MAX_STREAM_SESSIONS];
  EndRecord _endRecords[MAX_END_RECORDS];
  HapbeatPlayback _playbacks[MAX_PLAYBACK_HANDLES];

  uint16_t nextSeq() { return ++_seq; }
  uint16_t nextPlaybackId() {
    ++_nextPlaybackId;
    if (_nextPlaybackId == 0) ++_nextPlaybackId;
    return _nextPlaybackId;
  }
  uint64_t nextPlaybackGeneration() {
    ++_nextPlaybackGeneration;
    if (_nextPlaybackGeneration == 0) ++_nextPlaybackGeneration;
    return _nextPlaybackGeneration;
  }

  bool sendPacket(const uint8_t* buf, size_t len);
  bool sendTo(const uint8_t* buf, size_t len, IPAddress ip,
              uint16_t port = 0);
  bool sendRouted(const uint8_t* buf, size_t len);
  bool sendStreamTo(const uint8_t* buf, size_t len,
                    const StreamEndpoint& endpoint);
  void connectStatus(bool connected);

  void pollPongs();
  void pollPongsThrottled();
  void rememberDevice(IPAddress ip, uint16_t port, const char* address,
                      bool pinned = false);
  bool isAlive(const KnownDevice& d) const;
  uint8_t aliveCount() const;

  void reconcileStreams(uint32_t now);
  void pumpStreams(uint32_t now);
  bool sendStreamBegin(StreamSession& session);
  void stopSource(uint8_t slot, uint16_t id, uint64_t generation);
  void setSourceGain(uint8_t slot, uint16_t id, uint64_t generation,
                     float value);
  void setSourcePan(uint8_t slot, uint16_t id, uint64_t generation,
                    float value);
  void setSourceLoop(uint8_t slot, uint16_t id, uint64_t generation,
                     bool value);
  HapbeatPlaybackStatus playbackStatus(uint8_t slot, uint16_t id,
                                       uint64_t generation) const;
  HapbeatDeferredReason playbackReason(uint8_t slot, uint16_t id,
                                        uint64_t generation) const;
  void clearAllSources(bool sendEnds);

  uint8_t collectStreamEndpoints(StreamEndpoint* out, uint8_t maxOut) const;
  bool sameEndpoint(const StreamEndpoint& a, const StreamEndpoint& b) const;
  bool sameIpPort(const StreamEndpoint& a, const StreamEndpoint& b) const;
  int8_t findSession(const StreamEndpoint& endpoint) const;
  bool sourceMatches(const SineSource& source,
                    const StreamEndpoint& endpoint) const;
  bool endpointCoolingDown(const StreamEndpoint& endpoint, uint32_t now) const;
  void recordEnd(const StreamEndpoint& endpoint, uint32_t now);
  void removeSession(uint8_t slot);
  bool sourceCompletedEverywhere(uint8_t sourceSlot,
                                 const StreamEndpoint* endpoints,
                                 uint8_t endpointCount) const;
  void completeFinishedSources(const StreamEndpoint* endpoints,
                               uint8_t endpointCount);
  void mixSession(StreamSession& session, uint16_t frames,
                  uint8_t* packet, size_t packetCapacity,
                  uint8_t& finishedMask);
  static float clampFloat(float value, float minimum, float maximum);
  static int16_t saturatePcm16(float value);
  static bool copyAddress(char* out, size_t capacity, const char* value);
};

#endif  // HAPBEAT_H
