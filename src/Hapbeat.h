// Hapbeat.h — Arduino transport for driving Hapbeat haptic devices.
//
// Works on ESP32-series, ESP8266, and Arduino-compatible Wi-Fi boards (anything
// providing WiFiUDP). Connect Wi-Fi in your sketch first, then call begin().
// Streaming uses a fixed-capacity, endpoint-scoped mixer. It does not allocate
// on the heap and never broadcasts stream packets. Each device gets the stream
// format its firmware speaks: Stream Gen 2 (session stream, firmware 0.5.0+)
// under a device-issued lease, or Stream Gen 1 (firmware 0.4.x and older).
// tick() asks the targeted devices by unicast PING which one they speak.
// A Hapbeat instance is not thread-safe; synchronize callers that share one
// instance. Independent Hapbeat instances have independent stream state.
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

// Why a Playback is Deferred (sdk-multi-stream.md section 3). When matching
// endpoints differ, the first of Superseded, LeaseUnavailable, UnknownClass and
// NoResolvedEndpoint is reported.
enum class HapbeatDeferredReason : uint8_t {
  None,
  NoResolvedEndpoint,  // no PONG-resolved endpoint matches the target
  UnknownClass,        // no matched reply to this library's lease PING yet
  LeaseUnavailable,    // Stream Gen 2 device without a usable lease
  Superseded,          // another application streamed to the device later
};

// Stream format of one device endpoint, from matched replies to this
// library's own extended PING (stream-session.md Receiver classification).
enum class HapbeatStreamClass : uint8_t {
  Unknown,  // no matched reply yet: streams to it defer
  Gen1,     // firmware 0.4.x and older: Stream Gen 1 + 300 ms END guard
  Gen2,     // firmware 0.5.0 and newer: Stream Gen 2 session stream
};

// Stream Gen 2 lease of one endpoint. None for Unknown and Gen1 endpoints.
enum class HapbeatLeaseState : uint8_t {
  None,
  Valid,
  Unavailable,
  Superseded,  // cleared by the device handing the lease back, or by
               // Hapbeat::reacquireStreamOwnership()
};

struct HapbeatStreamEndpointInfo {
  char address[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};
  IPAddress ip{(uint32_t)0};
  uint16_t port = 0;
  HapbeatStreamClass streamClass = HapbeatStreamClass::Unknown;
  HapbeatLeaseState leaseState = HapbeatLeaseState::None;
};

// Handle for one logical synthesized-sine source. It is a move-only value
// carrying generation tokens, so source-slot recycling cannot retarget an
// older handle. It does not retain its Hapbeat owner: the owner must outlive
// the handle, and no handle method may be called after owner destruction.
class HapbeatPlayback {
 public:
  HapbeatPlayback() = default;
  HapbeatPlayback(const HapbeatPlayback&) = delete;
  HapbeatPlayback& operator=(const HapbeatPlayback&) = delete;
  HapbeatPlayback(HapbeatPlayback&& other) noexcept;
  HapbeatPlayback& operator=(HapbeatPlayback&& other) noexcept;
  uint16_t id() const { return _id; }
  bool valid() const;
  HapbeatPlaybackStatus status() const;
  HapbeatDeferredReason deferredReason() const;
  float gain() const;
  float pan() const;
  bool loop() const;

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
  float _gain = 1.0f;
  float _pan = 0.0f;
  bool _loop = false;

  void reset(Hapbeat* hub, uint8_t slot, uint16_t id, uint64_t generation,
             float gain, float pan, bool loop);
  void invalidate();
};

class Hapbeat {
 public:
  using Playback = HapbeatPlayback;
  using PlaybackStatus = HapbeatPlaybackStatus;
  using DeferredReason = HapbeatDeferredReason;

  Hapbeat() = default;
  Hapbeat(const Hapbeat&) = delete;
  Hapbeat& operator=(const Hapbeat&) = delete;
  Hapbeat(Hapbeat&&) = delete;
  Hapbeat& operator=(Hapbeat&&) = delete;

  // Open the UDP socket for sending. appName (<=16 chars) shows on the device
  // OLED while connected. Call after Wi-Fi is connected.
  bool begin(uint16_t port = 7700, const char* appName = "");

  // Fire a kit event by id (must exist in the kit deployed to the device via
  // Hapbeat Studio). gain is the app gain; the PLAY carries clamp(gain, 0, 1)
  // (NaN / infinity as 0). This library does not read the Kit manifest, so the
  // Kit's clip_gain is not applied (it counts as 1). target "" = all devices;
  // pan is the left/right balance (-1 = left only, 0 = centre, +1 = right only).
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
  // value handle. durationMs == 0 means open-ended; loop repeats a finite source.
  // The source is generated as 16 kHz mono and mixed as canonical 16 kHz stereo
  // PCM16 by the endpoint session. Non-finite frequency, intensity, or pan is
  // rejected with an invalid handle. Call tick() frequently from loop().
  Playback playSineSource(float freqHz, float intensity,
                          uint32_t durationMs = 0,
                          const char* target = "", float pan = 0.0f,
                          bool loop = false);

  // Blocking convenience wrapper. It registers through the same source pool and
  // StreamHub state machine as playSineSource().
  void playSine(float freqHz, float intensity, uint32_t durationMs,
                const char* target = "");

  // Cooperative stream reconciliation, mixing, packet pacing, PONG / ERROR
  // intake, and the unicast lease PINGs to the devices the sources target.
  void tick();

  // Stream class of the first known device with this IP (Unknown if none).
  HapbeatStreamClass streamClass(IPAddress ip) const;

  // Copy the live PONG-resolved endpoints (at most maxOut) into out and
  // return how many were written.
  uint8_t streamEndpoints(HapbeatStreamEndpointInfo* out, uint8_t maxOut) const;

  // Take the stream back from another application that superseded this one:
  // a fresh client incarnation and an immediate lease PING to every target.
  void reacquireStreamOwnership();

  // Receive one-line warnings (unreadable PONG tails, superseded leases,
  // device ERRORs). Each is reported once per device. The library never
  // prints; pass nullptr to stop.
  void setWarningHandler(void (*handler)(const char* message)) {
    _warningHandler = handler;
  }

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
  static const uint16_t STREAM_SAMPLE_RATE = 16000;
  static const uint8_t STREAM_CHANNELS = 2;
  static const uint16_t STREAM_CHUNK_FRAMES = 256;
  static const uint32_t STREAM_SEND_AHEAD_FRAMES = 2560;  // 160 ms
  static const uint32_t SESSION_LINGER_MS = 310;
  static const uint8_t MAX_CHUNKS_PER_TICK = 8;
  static const uint8_t MAX_PENDING_PINGS = 8;
  static const uint32_t PENDING_PING_MAX_AGE_MS = 15000;
  // Lease TTL / 3 for a held lease; the faster retry while the class is
  // unknown or no lease is usable (stream-session.md Discovery lease).
  static const uint32_t LEASE_REFRESH_MS = 5000;
  static const uint32_t LEASE_RETRY_MS = 500;
  // Lease PINGs continue for one TTL after an endpoint's last session.
  static const uint32_t LEASE_TTL_MS = 15000;
  // A new session needs a lease refreshed this recently (the TTL less a margin
  // for the PING's travel time), else it waits for the next lease reply.
  static const uint32_t LEASE_FRESH_MS = 14000;
  // Stream Gen 2 BEGIN repetition (Sender requirements 2): before each of the
  // first three DATA blocks, then at most 500 ms apart (450 leaves room for
  // tick jitter).
  static const uint8_t BEGIN_REPEAT_COUNT = 3;
  static const uint32_t BEGIN_REPEAT_MS = 450;
  // A Gen 2 session that could not send DATA this long restarts under the
  // next generation (Sender requirements 3; the device times out at 5 s).
  static const uint32_t OWN_GAP_RESTART_MS = 3000;
  static const size_t WARNING_BUFFER_SIZE = 160;

  enum class EndpointMode : uint8_t {
    Unknown,
    Gen1,
    Gen2,  // a usable lease
    LeaseUnavailable,
    Superseded,
  };

  struct KnownDevice {
    IPAddress ip{(uint32_t)0};
    uint16_t port = 7700;
    uint64_t lastPongMs = 0;
    bool pinned = false;
    uint8_t fails = 0;
    bool used = false;
    char address[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};

    // Stream class and lease, changed only by matched lease replies and the
    // device's ERRORs about this library's sessions.
    HapbeatStreamClass streamClass = HapbeatStreamClass::Unknown;
    bool leasePinged = false;
    uint64_t lastLeasePingMs = 0;
    int64_t lastAcceptedTimestampUs = 0;
    uint64_t leaseIncarnation = 0;  // incarnation of the accepted reply
    uint64_t bootId = 0;
    uint32_t ticket = 0;
    bool leaseValid = false;
    bool superseded = false;
    // When the last matched lease reply was accepted. The device lets an
    // unused lease expire one TTL after its last extended PING.
    uint64_t leaseRefreshedMs = 0;
    uint32_t lastGeneration = 0;  // last generation under (bootId, ticket)
    bool hadSession = false;
    uint64_t lastSessionMs = 0;
    // Envelope of the Gen 2 session last ended here (for its ERRORs).
    uint64_t endedBootId = 0;
    uint32_t endedTicket = 0;
    uint32_t endedGeneration = 0;
    bool ignoredWarned = false;
    uint16_t errorWarned = 0;  // bit per ERROR code already reported
  };

  struct PendingPing {
    bool used = false;
    uint16_t seq = 0;
    int64_t timestampUs = 0;
    IPAddress ip{(uint32_t)0};
    uint16_t port = 0;
    uint64_t incarnation = 0;
    uint64_t sentMs = 0;
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
    uint64_t totalFrames = 0;
    char target[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};
    HapbeatPlaybackStatus status = HapbeatPlaybackStatus::Stopped;
    HapbeatDeferredReason reason = HapbeatDeferredReason::None;
    float gain = 1.0f;
    float pan = 0.0f;
    bool loop = false;
    // Device-table slots where this finite source played to the end. Kept on
    // the source, not the session: a session that lingers out and is removed
    // must not make the source play there again (sdk-multi-stream section 6.10).
    uint8_t completedDevices = 0;
    // Device-table slots whose session STOP_ALL ended (ERROR STREAM_STOPPED):
    // this Playback never rejoins them (sdk-multi-stream section 6.13).
    uint8_t stoppedDevices = 0;
    bool played = false;   // played to the end on at least one endpoint
    bool leftByStop = false;  // removed from an endpoint by STREAM_STOPPED
    bool waiting = false;  // a matched endpoint holds or awaits its cursor
  };

  struct StreamSession {
    bool used = false;
    StreamEndpoint endpoint;
    bool beginSent = false;
    bool attached[MAX_STREAM_SOURCES] = {false};
    bool completed[MAX_STREAM_SOURCES] = {false};
    uint64_t cursors[MAX_STREAM_SOURCES] = {0};
    uint32_t byteOffset = 0;
    uint64_t framesSent = 0;
    uint64_t startedMs = 0;
    uint64_t emptySinceMs = 0;
    bool empty = false;
    // Stream Gen 2 identity. generation 0 = suspended: the session was ended
    // because its lease became unavailable and keeps the cursors for the
    // next lease of the same boot (Sender requirements 8).
    bool gen2 = false;
    uint64_t bootId = 0;
    uint32_t ticket = 0;
    uint32_t generation = 0;
    uint64_t suspendedMs = 0;
    uint8_t dataBlocksSent = 0;  // saturates at 255
    uint64_t lastBeginMs = 0;
    uint64_t lastDataMs = 0;
  };

  struct EndRecord {
    bool used = false;
    StreamEndpoint endpoint;
    uint64_t endedMs = 0;
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
  uint64_t _lastPollMs = 0;
  mutable uint32_t _lastMillisRaw = 0;
  mutable uint64_t _extendedMillis = 0;
  mutable bool _clockInitialized = false;
  bool _broadcastOnly = false;
  char _appName[hapbeat::MAX_APP_NAME_LEN + 1] = {0};

  SineSource _sources[MAX_STREAM_SOURCES];
  StreamSession _sessions[MAX_STREAM_SESSIONS];
  EndRecord _endRecords[MAX_END_RECORDS];

  uint64_t _incarnation = 0;  // client incarnation, nonzero after begin()
  int64_t _lastPingTimestampUs = 0;
  PendingPing _pendingPings[MAX_PENDING_PINGS];
  // Class members, not stack: the ESP8266 loop stack is 4 KiB.
  uint8_t _rxBuffer[hapbeat::MAX_PACKET_SIZE];
  char _warningBuffer[WARNING_BUFFER_SIZE] = {0};
  void (*_warningHandler)(const char* message) = nullptr;

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
  int8_t rememberDevice(IPAddress ip, uint16_t port, const char* address,
                        bool pinned = false);
  bool isAlive(const KnownDevice& d) const;
  uint8_t aliveCount() const;
  int8_t findDevice(IPAddress ip, uint16_t port) const;

  // Stream Gen 2 discovery lease (stream-session.md).
  void handlePong(IPAddress ip, uint16_t port, size_t length);
  void handleError(IPAddress ip, uint16_t port, size_t length);
  const PendingPing* matchPending(uint16_t seq, int64_t timestampUs,
                                  IPAddress ip, uint16_t port) const;
  int64_t nextPingTimestamp();
  void newIncarnation();
  void serviceLeasePings(uint64_t now);
  bool leasePingWanted(const KnownDevice& device, uint64_t now) const;
  void sendLeasePing(KnownDevice& device, uint64_t now);
  EndpointMode endpointMode(const StreamEndpoint& endpoint) const;
  bool leaseFresh(const StreamEndpoint& endpoint, uint64_t now) const;
  bool sessionCurrent(const StreamSession& session,
                      const StreamEndpoint& endpoint) const;
  HapbeatLeaseState leaseState(const KnownDevice& device) const;
  void warn(IPAddress ip, const char* format, ...);

  uint64_t extendedMillis() const;
  void reconcileStreams(uint64_t now);
  void pumpStreams(uint64_t now);
  bool openSession(uint8_t slot, const StreamEndpoint& endpoint,
                   EndpointMode mode, uint64_t now);
  bool allocateGeneration(StreamSession& session, uint64_t now);
  void restartSession(StreamSession& session, uint64_t now);
  void sendStreamEnd(StreamSession& session, uint64_t now);
  bool sendStreamBegin(StreamSession& session, uint64_t now);
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
  bool playbackValues(uint8_t slot, uint16_t id, uint64_t generation,
                      float& gain, float& pan, bool& loop) const;
  void clearAllSources(bool sendEnds);

  uint8_t collectStreamEndpoints(StreamEndpoint* out, uint8_t maxOut) const;
  bool sameEndpoint(const StreamEndpoint& a, const StreamEndpoint& b) const;
  bool sameIpPort(const StreamEndpoint& a, const StreamEndpoint& b) const;
  int8_t findSession(const StreamEndpoint& endpoint) const;
  bool sourceMatches(const SineSource& source,
                    const StreamEndpoint& endpoint) const;
  bool endpointCoolingDown(const StreamEndpoint& endpoint, uint64_t now) const;
  void recordEnd(const StreamEndpoint& endpoint, uint64_t now);
  void removeSession(uint8_t slot);
  bool sourceFinished(uint8_t sourceSlot) const;
  void forgetDeviceCompletion(uint8_t deviceSlot, bool alsoStopped);
  void stopSessionSources(uint8_t sessionSlot, uint8_t deviceSlot);
  void completeFinishedSources();
  void mixSession(StreamSession& session, uint16_t frames,
                  uint8_t* packet, size_t packetCapacity,
                  uint8_t& finishedMask);
  static float clampFloat(float value, float minimum, float maximum);
  static int16_t saturatePcm16(float value);
  static bool copyAddress(char* out, size_t capacity, const char* value);
};

#endif  // HAPBEAT_H
