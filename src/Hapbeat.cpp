// Hapbeat.cpp — see Hapbeat.h.
#include "Hapbeat.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif
#endif

namespace {

constexpr float kPhaseToRadians = 6.28318530717958647692f / 4294967296.0f;
constexpr double kPhaseModulus = 4294967296.0;
constexpr uint32_t kMaxGeneration = 0xFFFFFFFFu;

// Source of client incarnations. A restarted MCU must not repeat the previous
// value: the device would return the old lease and reject generation 1 as
// already used. ESP32 / ESP8266 use their hardware RNG; other boards fall back
// to Arduino random() seeded from the clock, which is weak (README).
uint64_t randomU64() {
#if defined(ESP32)
  return ((uint64_t)esp_random() << 32) | (uint64_t)esp_random();
#elif defined(ESP8266)
  return ((uint64_t)ESP.random() << 32) | (uint64_t)ESP.random();
#else
  static bool seeded = false;
  if (!seeded) {
    randomSeed((unsigned long)micros());
    seeded = true;
  }
  uint64_t value = 0;
  for (int i = 0; i < 4; ++i)
    value = (value << 16) | (uint64_t)(random(0x10000L) & 0xFFFF);
  return value;
#endif
}

uint64_t readU64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

uint32_t readU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// One bit per ERROR code for the once-per-device report.
uint16_t errorBit(uint16_t code) {
  if (code >= 0x0001 && code <= 0x0003) return (uint16_t)(1u << (code - 1));
  if (code >= 0x0010 && code <= 0x0017) return (uint16_t)(1u << (3 + code - 0x10));
  return (uint16_t)(1u << 15);
}

HapbeatDeferredReason strongerReason(HapbeatDeferredReason current,
                                     HapbeatDeferredReason candidate) {
  // Superseded > LeaseUnavailable > UnknownClass > NoResolvedEndpoint, which
  // is the enum order (sdk-multi-stream.md section 3).
  return (uint8_t)candidate > (uint8_t)current ? candidate : current;
}

}  // namespace

HapbeatPlayback::HapbeatPlayback(HapbeatPlayback&& other) noexcept {
  _hub = other._hub;
  _slot = other._slot;
  _id = other._id;
  _generation = other._generation;
  _gain = other._gain;
  _pan = other._pan;
  _loop = other._loop;
  other.invalidate();
}

HapbeatPlayback& HapbeatPlayback::operator=(HapbeatPlayback&& other) noexcept {
  if (this == &other) return *this;
  _hub = other._hub;
  _slot = other._slot;
  _id = other._id;
  _generation = other._generation;
  _gain = other._gain;
  _pan = other._pan;
  _loop = other._loop;
  other.invalidate();
  return *this;
}

void HapbeatPlayback::invalidate() {
  _hub = nullptr;
  _slot = 0;
  _id = 0;
  _generation = 0;
  _gain = 1.0f;
  _pan = 0.0f;
  _loop = false;
}

void HapbeatPlayback::reset(Hapbeat* hub, uint8_t slot, uint16_t id,
                            uint64_t generation, float gain, float pan,
                            bool loop) {
  _hub = hub;
  _slot = slot;
  _id = id;
  _generation = generation;
  _gain = gain;
  _pan = pan;
  _loop = loop;
}

bool HapbeatPlayback::valid() const {
  return _hub != nullptr && _id != 0 && _generation != 0;
}

HapbeatPlaybackStatus HapbeatPlayback::status() const {
  if (!valid()) return HapbeatPlaybackStatus::Stopped;
  return _hub->playbackStatus(_slot, _id, _generation);
}

HapbeatDeferredReason HapbeatPlayback::deferredReason() const {
  if (!valid()) return HapbeatDeferredReason::None;
  return _hub->playbackReason(_slot, _id, _generation);
}

float HapbeatPlayback::gain() const {
  float gain = _gain;
  float pan = _pan;
  bool loop = _loop;
  if (valid() && _hub->playbackValues(_slot, _id, _generation, gain, pan, loop))
    return gain;
  return _gain;
}

float HapbeatPlayback::pan() const {
  float gain = _gain;
  float pan = _pan;
  bool loop = _loop;
  if (valid() && _hub->playbackValues(_slot, _id, _generation, gain, pan, loop))
    return pan;
  return _pan;
}

bool HapbeatPlayback::loop() const {
  float gain = _gain;
  float pan = _pan;
  bool loop = _loop;
  if (valid() && _hub->playbackValues(_slot, _id, _generation, gain, pan, loop))
    return loop;
  return _loop;
}

void HapbeatPlayback::setGain(float value) {
  if (!isfinite(value)) return;
  _gain = Hapbeat::clampFloat(value, 0.0f, 1.0f);
  if (valid()) _hub->setSourceGain(_slot, _id, _generation, _gain);
}

void HapbeatPlayback::setPan(float value) {
  if (!isfinite(value)) return;
  _pan = Hapbeat::clampFloat(value, -1.0f, 1.0f);
  if (valid()) _hub->setSourcePan(_slot, _id, _generation, _pan);
}

void HapbeatPlayback::setLoop(bool value) {
  _loop = value;
  if (valid()) _hub->setSourceLoop(_slot, _id, _generation, _loop);
}

void HapbeatPlayback::stop() {
  if (!valid() || status() == HapbeatPlaybackStatus::Stopped) return;
  _hub->stopSource(_slot, _id, _generation);
}

uint64_t Hapbeat::extendedMillis() const {
  const uint32_t raw = millis();
  if (!_clockInitialized) {
    _lastMillisRaw = raw;
    _extendedMillis = raw;
    _clockInitialized = true;
  } else {
    _extendedMillis += (uint32_t)(raw - _lastMillisRaw);
    _lastMillisRaw = raw;
  }
  return _extendedMillis;
}

bool Hapbeat::begin(uint16_t port, const char* appName) {
  _port = port;
  _appName[0] = '\0';
  if (appName) {
    strncpy(_appName, appName, hapbeat::MAX_APP_NAME_LEN);
    _appName[hapbeat::MAX_APP_NAME_LEN] = '\0';
  }
  _udp.begin(_port);  // bind for both sends and PONG discovery replies
  newIncarnation();  // every begin() is a new connection (stream-session.md)
  _ready = true;
  if (_appName[0]) connectStatus(true);
  return true;
}

bool Hapbeat::sendPacket(const uint8_t* buf, size_t len) {
  if (!_ready || len == 0) return false;
  _udp.beginPacket(IPAddress(255, 255, 255, 255), _port);
  _udp.write(buf, len);
  return _udp.endPacket() != 0;
}

bool Hapbeat::sendTo(const uint8_t* buf, size_t len, IPAddress ip,
                     uint16_t port) {
  if (!_ready || len == 0 || (uint32_t)ip == 0) return false;
  const uint16_t destinationPort = port ? port : _port;
  _udp.beginPacket(ip, destinationPort);
  _udp.write(buf, len);
  const bool sent = _udp.endPacket() != 0;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    KnownDevice& device = _devices[i];
    if (!device.used || device.ip != ip || device.port != destinationPort) continue;
    if (sent) {
      device.fails = 0;
    } else if (!device.pinned && ++device.fails >= 5) {
      device = KnownDevice();
    }
  }
  return sent;
}

// Commands may fall back to broadcast while no command destination is known.
// Streaming does not use this function: STREAM_DATA/END have no target field,
// so every stream packet is sent through sendStreamTo() to one exact endpoint.
bool Hapbeat::sendRouted(const uint8_t* buf, size_t len) {
  if (!_ready || len == 0) return false;
  pollPongsThrottled();
  if (_broadcastOnly) return sendPacket(buf, len);

  bool known = false;
  bool sent = false;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    if (!isAlive(_devices[i])) continue;
    known = true;
    if (sendTo(buf, len, _devices[i].ip, _devices[i].port)) sent = true;
  }
  return known ? sent : sendPacket(buf, len);
}

bool Hapbeat::sendStreamTo(const uint8_t* buf, size_t len,
                           const StreamEndpoint& endpoint) {
  // A pinned IP without a PONG address is intentionally not a stream endpoint.
  if (!endpoint.address[0] || (uint32_t)endpoint.ip == 0 ||
      (uint32_t)endpoint.ip == 0xffffffffu)
    return false;
  return sendTo(buf, len, endpoint.ip, endpoint.port);
}

bool Hapbeat::isAlive(const KnownDevice& device) const {
  if (!device.used || (uint32_t)device.ip == 0) return false;
  if (device.pinned) return true;
  return (extendedMillis() - device.lastPongMs) < _deviceTimeoutMs;
}

uint8_t Hapbeat::aliveCount() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i)
    if (isAlive(_devices[i])) ++count;
  return count;
}

int8_t Hapbeat::rememberDevice(IPAddress ip, uint16_t port, const char* address,
                               bool pinned) {
  if ((uint32_t)ip == 0) return -1;
  if (port == 0) port = _port;
  const bool hasAddress = address && address[0];
  const uint64_t now = extendedMillis();
  int8_t slot = -1;
  int8_t freeSlot = -1;
  int8_t oldest = -1;
  uint64_t oldestAge = 0;

  // The exact (IP, port, full address) tuple is the endpoint identity. A full
  // address may legitimately be reported by multiple IP routes, so address
  // alone must never select or overwrite an existing table entry.
  if (hasAddress) {
    for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
      KnownDevice& device = _devices[i];
      if (!device.used) continue;
      if (device.ip == ip && device.port == port &&
          strcmp(device.address, address) == 0) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
        KnownDevice& device = _devices[i];
        if (device.used && device.ip == ip && device.port == port) {
          slot = i;  // same route, address changed
          break;
        }
      }
    }
  } else {
    for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
      KnownDevice& device = _devices[i];
      if (device.used && device.ip == ip && device.port == port) {
        slot = i;
        break;
      }
    }
  }

  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    KnownDevice& device = _devices[i];
    if (slot < 0 && !device.used) freeSlot = i;
    if (slot < 0 && device.used && !isAlive(device)) freeSlot = i;
    if (slot < 0 && device.used && !device.pinned) {
      const uint64_t age = now - device.lastPongMs;
      if (oldest < 0 || age > oldestAge) {
        oldest = i;
        oldestAge = age;
      }
    }
  }

  const bool fresh = slot < 0;
  if (slot < 0) slot = freeSlot >= 0 ? freeSlot : oldest;
  if (slot < 0) return -1;
  KnownDevice& device = _devices[slot];
  // A free or evicted slot holds another device's class and lease.
  if (fresh) device = KnownDevice();
  device.used = true;
  device.ip = ip;
  device.port = port;
  device.lastPongMs = now;
  device.fails = 0;
  if (pinned) device.pinned = true;
  if (hasAddress) copyAddress(device.address, sizeof(device.address), address);
  return slot;
}

int8_t Hapbeat::findDevice(IPAddress ip, uint16_t port) const {
  for (uint8_t i = 0; i < MAX_DEVICES; ++i)
    if (_devices[i].used && _devices[i].ip == ip && _devices[i].port == port)
      return (int8_t)i;
  return -1;
}

void Hapbeat::pollPongs() {
  if (!_ready) return;
  for (uint8_t guard = 0; guard < MAX_DEVICES; ++guard) {
    const int packetSize = _udp.parsePacket();
    if (packetSize <= 0) return;
    const IPAddress remoteIp = _udp.remoteIP();
    const uint16_t sourcePort = _udp.remotePort();
    const int readSize = _udp.read(_rxBuffer, sizeof(_rxBuffer));
    _udp.flush();
    if (packetSize > (int)sizeof(_rxBuffer) ||
        readSize < (int)hapbeat::HEADER_SIZE)
      continue;
    const size_t length = (size_t)readSize;
    if (_rxBuffer[3] == hapbeat::CMD_PONG) {
      handlePong(remoteIp, sourcePort ? sourcePort : _port, length);
    } else if (_rxBuffer[3] == hapbeat::CMD_ERROR) {
      handleError(remoteIp, sourcePort, length);
    }
  }
}

const Hapbeat::PendingPing* Hapbeat::matchPending(uint16_t seq,
                                                  int64_t timestampUs,
                                                  IPAddress ip,
                                                  uint16_t port) const {
  if (timestampUs == 0) return nullptr;  // unsolicited identity PONG
  const uint64_t now = extendedMillis();
  for (uint8_t i = 0; i < MAX_PENDING_PINGS; ++i) {
    const PendingPing& pending = _pendingPings[i];
    if (pending.used && pending.seq == seq &&
        pending.timestampUs == timestampUs && pending.ip == ip &&
        pending.port == port && now - pending.sentMs < PENDING_PING_MAX_AGE_MS)
      return &pending;
  }
  return nullptr;
}

// Class and lease change only on a matched direct reply to this incarnation's
// extended PING (same seq, echoed timestamp, request destination, echoed
// incarnation) that is newer than the last accepted one (stream-session.md
// Discovery lease and Receiver classification).
void Hapbeat::handlePong(IPAddress ip, uint16_t port, size_t length) {
  hapbeat::PongInfo info;
  if (!hapbeat::parsePong(_rxBuffer, length, info)) return;
  const char* address = info.hasAddress ? info.address : "";
  const PendingPing* pending =
      matchPending(info.seq, info.timestampUs, ip, port);

  if (info.tail == hapbeat::PongTail::Ignored) {
    // Not a lease reply this version can read, and not proof of Stream Gen 1.
    const int8_t slot = rememberDevice(ip, port, address);
    if (slot >= 0 && !_devices[slot].ignoredWarned) {
      _devices[slot].ignoredWarned = true;
      warn(ip,
           "PONG stream tail not understood (newer firmware major?); streams "
           "to it stay deferred");
    }
    return;
  }

  if (info.tail == hapbeat::PongTail::Gen2) {
    // A lease tail that fails the correlation is dropped whole, address and
    // liveness included (a retired request or a previous incarnation).
    if (!pending || _incarnation == 0 ||
        info.echoedIncarnation != _incarnation ||
        pending->incarnation != info.echoedIncarnation)
      return;
    const int8_t existing = findDevice(ip, port);
    if (existing >= 0 &&
        info.timestampUs <= _devices[existing].lastAcceptedTimestampUs)
      return;  // older than an accepted reply: never roll the lease back
    const uint64_t incarnation = pending->incarnation;
    const int8_t slot = rememberDevice(ip, port, address);
    if (slot < 0) return;
    KnownDevice& device = _devices[slot];
    bool valid = info.leaseValid;
    bool superseded = info.superseded;
    // A superseded writer stays deferred until the device re-issues it a
    // ticket (lease_reissued) or the sketch reacquires ownership; a later
    // periodic reply is not an implicit takeover.
    const bool keepSuperseded =
        device.streamClass == HapbeatStreamClass::Gen2 && device.superseded &&
        device.leaseIncarnation == incarnation &&
        !(valid && !superseded && info.reissued);
    if (keepSuperseded) {
      valid = false;
      superseded = true;
    } else if (device.bootId != info.bootId || device.ticket != info.ticket) {
      device.bootId = info.bootId;
      device.ticket = info.ticket;
      device.lastGeneration = 0;
    }
    if (superseded && !device.superseded)
      warn(ip,
           "stream lease superseded by another application; streams to it "
           "stay deferred until it hands the device back or "
           "reacquireStreamOwnership() is called");
    device.streamClass = HapbeatStreamClass::Gen2;
    device.leaseValid = valid;
    device.superseded = superseded;
    device.leaseIncarnation = incarnation;
    device.lastAcceptedTimestampUs = info.timestampUs;
    return;
  }

  // Nothing after volume_wiper. Firmware before Stream Gen 2 cannot echo the
  // incarnation, so the request itself must be this incarnation's.
  const int8_t slot = rememberDevice(ip, port, address);
  if (slot < 0 || !pending || _incarnation == 0 ||
      pending->incarnation != _incarnation)
    return;
  KnownDevice& device = _devices[slot];
  if (info.timestampUs <= device.lastAcceptedTimestampUs) return;
  device.streamClass = HapbeatStreamClass::Gen1;
  device.leaseValid = false;
  device.superseded = false;
  device.bootId = 0;
  device.ticket = 0;
  device.lastGeneration = 0;
  device.leaseIncarnation = pending->incarnation;
  device.lastAcceptedTimestampUs = info.timestampUs;
}

// ERROR (message-format.md 0xFF): only from a known device on the Hapbeat
// port, and a stream code only when its detail is the envelope of this
// library's current or just-ended session there. Handled per endpoint.
void Hapbeat::handleError(IPAddress ip, uint16_t port, size_t length) {
  if (port != _port) return;
  const int8_t slot = findDevice(ip, port);
  if (slot < 0) return;
  hapbeat::ErrorInfo error;
  if (!hapbeat::parseError(_rxBuffer, length, error)) return;
  KnownDevice& device = _devices[slot];

  StreamSession* current = nullptr;
  if (error.code >= hapbeat::ERR_STREAM_LEASE_INVALID &&
      error.code <= hapbeat::ERR_STREAM_TARGET_MISMATCH) {
    if (!error.hasReference ||
        error.detailLength != hapbeat::STREAM_ENVELOPE_SIZE)
      return;
    const uint64_t bootId = readU64(error.detail);
    const uint32_t ticket = readU32(error.detail + 8);
    const uint32_t generation = readU32(error.detail + 12);
    for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
      StreamSession& session = _sessions[s];
      if (session.used && session.gen2 && session.generation != 0 &&
          session.endpoint.ip == ip && session.endpoint.port == port &&
          session.bootId == bootId && session.ticket == ticket &&
          session.generation == generation)
        current = &session;
    }
    const bool ended = device.endedGeneration != 0 &&
                       device.endedBootId == bootId &&
                       device.endedTicket == ticket &&
                       device.endedGeneration == generation;
    if (!current && !ended) return;
  }

  const uint16_t bit = errorBit(error.code);
  if (!(device.errorWarned & bit)) {
    device.errorWarned |= bit;
    warn(ip, "device ERROR %s (0x%04x): %s", hapbeat::errorName(error.code),
         (unsigned)error.code, error.message);
  }

  const uint64_t now = extendedMillis();
  switch (error.code) {
    case hapbeat::ERR_STREAM_GEN_UNSUPPORTED:
      // The device was updated past Stream Gen 1: classify it again.
      if (device.streamClass != HapbeatStreamClass::Gen1) break;
      device.streamClass = HapbeatStreamClass::Unknown;
      sendLeasePing(device, now);
      break;
    case hapbeat::ERR_STREAM_LEASE_INVALID:
      if (device.streamClass != HapbeatStreamClass::Gen2) break;
      device.leaseValid = false;
      sendLeasePing(device, now);
      break;
    case hapbeat::ERR_STREAM_SUPERSEDED:
      // Defer the device; no automatic takeover.
      if (device.streamClass != HapbeatStreamClass::Gen2) break;
      device.leaseValid = false;
      device.superseded = true;
      break;
    case hapbeat::ERR_STREAM_NOT_STARTED:
      if (current) sendStreamBegin(*current, now);
      break;
    default:
      // Reported only. STREAM_TIMED_OUT, STREAM_STOPPED and
      // STREAM_TARGET_MISMATCH are not acted on by this version (README).
      break;
  }
}

int64_t Hapbeat::nextPingTimestamp() {
  // Strictly increasing and never 0 (0 marks an unsolicited PONG).
  int64_t timestamp = (int64_t)extendedMillis() * 1000;
  if (timestamp <= _lastPingTimestampUs) timestamp = _lastPingTimestampUs + 1;
  _lastPingTimestampUs = timestamp;
  return timestamp;
}

void Hapbeat::newIncarnation() {
  uint64_t value = 0;
  while (value == 0 || value == _incarnation) value = randomU64();
  _incarnation = value;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) _devices[i].leasePinged = false;
}

bool Hapbeat::leasePingWanted(const KnownDevice& device, uint64_t now) const {
  if (!isAlive(device) || !device.address[0] ||
      (uint32_t)device.ip == 0xffffffffu)
    return false;
  if (device.hadSession && now - device.lastSessionMs < LEASE_TTL_MS)
    return true;
  for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
    if (_sources[i].used &&
        hapbeat::addressMatches(_sources[i].target, device.address))
      return true;
  return false;
}

// Extended PINGs go by unicast only, to the devices the sources target or
// that had a session within one lease TTL (stream-session.md Discovery lease).
void Hapbeat::serviceLeasePings(uint64_t now) {
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used) continue;
    const int8_t slot =
        findDevice(_sessions[s].endpoint.ip, _sessions[s].endpoint.port);
    if (slot < 0) continue;
    _devices[slot].hadSession = true;
    _devices[slot].lastSessionMs = now;
  }
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    KnownDevice& device = _devices[i];
    if (!leasePingWanted(device, now)) continue;
    const HapbeatLeaseState lease = leaseState(device);
    const bool retry =
        device.streamClass == HapbeatStreamClass::Unknown ||
        lease == HapbeatLeaseState::Unavailable;
    const uint32_t interval = retry ? LEASE_RETRY_MS : LEASE_REFRESH_MS;
    if (device.leasePinged && now - device.lastLeasePingMs < interval) continue;
    sendLeasePing(device, now);
  }
}

void Hapbeat::sendLeasePing(KnownDevice& device, uint64_t now) {
  uint8_t packet[hapbeat::HEADER_SIZE + 16];
  const uint16_t seq = nextSeq();
  const int64_t timestamp = nextPingTimestamp();
  const size_t length = hapbeat::buildPingExtended(packet, sizeof(packet), seq,
                                                   timestamp, _incarnation);
  device.leasePinged = true;
  device.lastLeasePingMs = now;
  if (length == 0) return;

  // Keep the request until it ages out; it is not consumed by its reply.
  int8_t slot = -1;
  for (uint8_t i = 0; i < MAX_PENDING_PINGS && slot < 0; ++i)
    if (!_pendingPings[i].used ||
        now - _pendingPings[i].sentMs >= PENDING_PING_MAX_AGE_MS)
      slot = (int8_t)i;
  if (slot < 0) {
    slot = 0;
    for (uint8_t i = 1; i < MAX_PENDING_PINGS; ++i)
      if (_pendingPings[i].sentMs < _pendingPings[slot].sentMs) slot = (int8_t)i;
  }
  PendingPing& pending = _pendingPings[slot];
  pending.used = true;
  pending.seq = seq;
  pending.timestampUs = timestamp;
  pending.ip = device.ip;
  pending.port = device.port;
  pending.incarnation = _incarnation;
  pending.sentMs = now;
  sendTo(packet, length, device.ip, device.port);
}

Hapbeat::EndpointMode Hapbeat::endpointMode(
    const StreamEndpoint& endpoint) const {
  const int8_t slot = findDevice(endpoint.ip, endpoint.port);
  if (slot < 0) return EndpointMode::Unknown;
  const KnownDevice& device = _devices[slot];
  switch (device.streamClass) {
    case HapbeatStreamClass::Gen1:
      return EndpointMode::Gen1;
    case HapbeatStreamClass::Gen2:
      switch (leaseState(device)) {
        case HapbeatLeaseState::Valid: return EndpointMode::Gen2;
        case HapbeatLeaseState::Superseded: return EndpointMode::Superseded;
        default: return EndpointMode::LeaseUnavailable;
      }
    default:
      return EndpointMode::Unknown;
  }
}

// Whether a session may keep streaming to this endpoint in its own format
// and identity: Stream Gen 1 to a Gen 1 device, or Stream Gen 2 under the
// device's current usable lease.
bool Hapbeat::sessionCurrent(const StreamSession& session,
                             const StreamEndpoint& endpoint) const {
  const EndpointMode mode = endpointMode(endpoint);
  if (!session.gen2) return mode == EndpointMode::Gen1;
  if (mode != EndpointMode::Gen2 || session.generation == 0) return false;
  const int8_t slot = findDevice(endpoint.ip, endpoint.port);
  return slot >= 0 && _devices[slot].bootId == session.bootId &&
         _devices[slot].ticket == session.ticket;
}

HapbeatLeaseState Hapbeat::leaseState(const KnownDevice& device) const {
  if (device.streamClass != HapbeatStreamClass::Gen2)
    return HapbeatLeaseState::None;
  // A lease of an earlier incarnation is not ours any more.
  if (device.leaseIncarnation != _incarnation)
    return HapbeatLeaseState::Unavailable;
  if (device.superseded) return HapbeatLeaseState::Superseded;
  if (device.leaseValid && device.bootId != 0 && device.ticket != 0)
    return HapbeatLeaseState::Valid;
  return HapbeatLeaseState::Unavailable;
}

void Hapbeat::warn(IPAddress ip, const char* format, ...) {
  if (!_warningHandler) return;
  int length = snprintf(_warningBuffer, sizeof(_warningBuffer),
                        "Hapbeat %u.%u.%u.%u: ", (unsigned)ip[0],
                        (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
  if (length < 0) length = 0;
  if ((size_t)length < sizeof(_warningBuffer)) {
    va_list args;
    va_start(args, format);
    vsnprintf(_warningBuffer + length, sizeof(_warningBuffer) - (size_t)length,
              format, args);
    va_end(args);
  }
  _warningHandler(_warningBuffer);
}

HapbeatStreamClass Hapbeat::streamClass(IPAddress ip) const {
  for (uint8_t i = 0; i < MAX_DEVICES; ++i)
    if (_devices[i].used && _devices[i].ip == ip) return _devices[i].streamClass;
  return HapbeatStreamClass::Unknown;
}

uint8_t Hapbeat::streamEndpoints(HapbeatStreamEndpointInfo* out,
                                 uint8_t maxOut) const {
  if (!out) return 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_DEVICES && count < maxOut; ++i) {
    const KnownDevice& device = _devices[i];
    if (!isAlive(device) || !device.address[0]) continue;
    HapbeatStreamEndpointInfo& info = out[count++];
    copyAddress(info.address, sizeof(info.address), device.address);
    info.ip = device.ip;
    info.port = device.port;
    info.streamClass = device.streamClass;
    info.leaseState = leaseState(device);
  }
  return count;
}

void Hapbeat::reacquireStreamOwnership() {
  newIncarnation();
  if (_ready) serviceLeasePings(extendedMillis());
}

void Hapbeat::pollPongsThrottled() {
  const uint64_t now = extendedMillis();
  if (now - _lastPollMs < 100) return;
  _lastPollMs = now;
  pollPongs();
}

void Hapbeat::poll() {
  pollPongs();
  if (_ready) reconcileStreams(extendedMillis());
}

uint8_t Hapbeat::deviceCount() {
  pollPongsThrottled();
  return aliveCount();
}

IPAddress Hapbeat::deviceIp() {
  pollPongsThrottled();
  for (uint8_t i = 0; i < MAX_DEVICES; ++i)
    if (isAlive(_devices[i])) return _devices[i].ip;
  return IPAddress((uint32_t)0);
}

uint8_t Hapbeat::deviceIps(IPAddress* out, uint8_t maxOut) {
  pollPongsThrottled();
  if (!out) return 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_DEVICES && count < maxOut; ++i)
    if (isAlive(_devices[i])) out[count++] = _devices[i].ip;
  return count;
}

void Hapbeat::setDeviceIp(IPAddress ip) {
  if ((uint32_t)ip == 0) {
    for (uint8_t i = 0; i < MAX_DEVICES; ++i)
      if (_devices[i].pinned) _devices[i] = KnownDevice();
    return;
  }
  rememberDevice(ip, _port, "", true);
}

bool Hapbeat::discover(uint32_t timeoutMs) {
  if (!_ready) return false;
  ping();
  const uint64_t start = extendedMillis();
  while (extendedMillis() - start < timeoutMs) {
    pollPongs();
    delay(5);
  }
  pollPongs();
  reconcileStreams(extendedMillis());
  return aliveCount() > 0;
}

void Hapbeat::play(const char* eventId, float gain, const char* target,
                   float pan) {
  if (!isfinite(pan)) return;
  uint8_t packet[256];
  sendRouted(packet, hapbeat::buildPlay(packet, sizeof(packet), nextSeq(), eventId,
                                        target, 0, gain, pan));
}

void Hapbeat::stop(const char* eventId, const char* target) {
  uint8_t packet[256];
  sendRouted(packet, hapbeat::buildStop(packet, sizeof(packet), nextSeq(), eventId,
                                        target));
}

void Hapbeat::stopAll(const char* target) {
  clearAllSources(false);
  uint8_t packet[128];
  sendRouted(packet, hapbeat::buildStopAll(packet, sizeof(packet), nextSeq(), target));
}

void Hapbeat::ping() {
  uint8_t packet[16];
  const uint16_t seq = nextSeq();
  const size_t length = hapbeat::buildPing(packet, sizeof(packet), seq,
                                           nextPingTimestamp());
  sendPacket(packet, length);  // discovery remains broadcast
  if (_broadcastOnly) return;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i)
    if (isAlive(_devices[i])) sendTo(packet, length, _devices[i].ip, _devices[i].port);
}

void Hapbeat::connectStatus(bool connected) {
  uint8_t packet[64];
  sendPacket(packet, hapbeat::buildConnectStatus(packet, sizeof(packet), nextSeq(),
                                                  connected, _group, _appName, ""));
}

void Hapbeat::end() {
  clearAllSources(true);
  if (_ready && _appName[0]) connectStatus(false);
  _ready = false;
}

Hapbeat::Playback Hapbeat::playSineSource(float freqHz, float intensity,
                                          uint32_t durationMs,
                                          const char* target, float pan,
                                          bool loop) {
  if (!_ready || !isfinite(freqHz) || !isfinite(intensity) || !isfinite(pan))
    return Playback();
  const char* targetValue = target ? target : "";
  const size_t targetLength = strlen(targetValue);
  if (targetLength > hapbeat::MAX_DEVICE_ADDRESS_LEN) return Playback();

  uint8_t slot = MAX_STREAM_SOURCES;
  for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
    if (!_sources[i].used) {
      slot = i;
      break;
    }
  }
  if (slot == MAX_STREAM_SOURCES) return Playback();

  SineSource& source = _sources[slot];
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    _sessions[s].attached[slot] = false;
    _sessions[s].completed[slot] = false;
    _sessions[s].cursors[slot] = 0;
  }
  source.used = true;
  source.id = nextPlaybackId();
  const double scaledStep = (double)freqHz * kPhaseModulus /
                            (double)STREAM_SAMPLE_RATE;
  const double wrappedStep = fmod(scaledStep, kPhaseModulus);
  const double roundedStep = wrappedStep >= 0.0
                                 ? floor(wrappedStep + 0.5)
                                 : ceil(wrappedStep - 0.5);
  source.generation = nextPlaybackGeneration();
  source.phaseStep = (uint32_t)(int64_t)roundedStep;
  source.amplitude = (int16_t)(0.8f * 32767.0f);
  if (durationMs == 0) {
    source.totalFrames = 0;  // open-ended source
  } else {
    const uint64_t frames = (uint64_t)durationMs * STREAM_SAMPLE_RATE / 1000;
    source.totalFrames = frames == 0 ? 1 : frames;
  }
  memcpy(source.target, targetValue, targetLength + 1);
  source.gain = clampFloat(intensity, 0.0f, 1.0f);
  source.pan = clampFloat(pan, -1.0f, 1.0f);
  source.loop = loop;
  source.status = HapbeatPlaybackStatus::Deferred;
  source.reason = HapbeatDeferredReason::NoResolvedEndpoint;
  reconcileStreams(extendedMillis());
  Playback playback;
  playback.reset(this, slot, source.id, source.generation, source.gain,
                 source.pan, source.loop);
  return playback;
}

void Hapbeat::playSine(float freqHz, float intensity, uint32_t durationMs,
                       const char* target) {
  Playback playback =
      playSineSource(freqHz, intensity, durationMs, target, 0.0f, false);
  if (!playback.valid()) return;
  const uint64_t start = extendedMillis();
  while (extendedMillis() - start < durationMs) {
    tick();
    delay(2);
  }
  playback.stop();
  tick();
}

void Hapbeat::tick() {
  if (!_ready) return;
  pollPongs();
  const uint64_t now = extendedMillis();
  serviceLeasePings(now);
  reconcileStreams(now);
  pumpStreams(now);
}

float Hapbeat::clampFloat(float value, float minimum, float maximum) {
  if (!isfinite(value)) return minimum;
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

int16_t Hapbeat::saturatePcm16(float value) {
  if (value >= 32767.0f) return 32767;
  if (value <= -32768.0f) return -32768;
  return (int16_t)lroundf(value);
}

bool Hapbeat::copyAddress(char* out, size_t capacity, const char* value) {
  if (!out || capacity == 0 || !value) return false;
  const size_t length = strlen(value);
  if (length >= capacity) return false;
  memcpy(out, value, length + 1);
  return true;
}

uint8_t Hapbeat::collectStreamEndpoints(StreamEndpoint* out,
                                         uint8_t maxOut) const {
  if (!out || maxOut == 0) return 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_DEVICES && count < maxOut; ++i) {
    const KnownDevice& device = _devices[i];
    if (!isAlive(device) || !device.address[0] ||
        (uint32_t)device.ip == 0xffffffffu)
      continue;
    StreamEndpoint endpoint;
    endpoint.ip = device.ip;
    endpoint.port = device.port ? device.port : _port;
    copyAddress(endpoint.address, sizeof(endpoint.address), device.address);
    bool duplicate = false;
    for (uint8_t j = 0; j < count; ++j)
      if (sameEndpoint(out[j], endpoint)) duplicate = true;
    if (!duplicate) out[count++] = endpoint;
  }
  return count;
}

bool Hapbeat::sameEndpoint(const StreamEndpoint& a,
                           const StreamEndpoint& b) const {
  return a.ip == b.ip && a.port == b.port && strcmp(a.address, b.address) == 0;
}

bool Hapbeat::sameIpPort(const StreamEndpoint& a,
                         const StreamEndpoint& b) const {
  return a.ip == b.ip && a.port == b.port;
}

int8_t Hapbeat::findSession(const StreamEndpoint& endpoint) const {
  for (uint8_t i = 0; i < MAX_STREAM_SESSIONS; ++i)
    if (_sessions[i].used && sameEndpoint(_sessions[i].endpoint, endpoint)) return i;
  return -1;
}

bool Hapbeat::sourceMatches(const SineSource& source,
                            const StreamEndpoint& endpoint) const {
  return hapbeat::addressMatches(source.target, endpoint.address);
}

bool Hapbeat::endpointCoolingDown(const StreamEndpoint& endpoint,
                                  uint64_t now) const {
  for (uint8_t i = 0; i < MAX_END_RECORDS; ++i) {
    const EndRecord& record = _endRecords[i];
    if (!record.used) continue;
    if (sameEndpoint(record.endpoint, endpoint)) {
      if (now - record.endedMs < SESSION_LINGER_MS) return true;
    }
  }
  return false;
}

void Hapbeat::recordEnd(const StreamEndpoint& endpoint, uint64_t now) {
  int8_t slot = -1;
  int8_t oldest = -1;
  uint64_t oldestAge = 0;
  for (uint8_t i = 0; i < MAX_END_RECORDS; ++i) {
    EndRecord& record = _endRecords[i];
    if (!record.used) {
      slot = i;
      break;
    }
    if (sameEndpoint(record.endpoint, endpoint)) {
      slot = i;
      break;
    }
    const uint64_t age = now - record.endedMs;
    if (oldest < 0 || age > oldestAge) {
      oldest = i;
      oldestAge = age;
    }
  }
  if (slot < 0) slot = oldest;
  if (slot < 0) return;
  _endRecords[slot].used = true;
  _endRecords[slot].endpoint = endpoint;
  _endRecords[slot].endedMs = now;
}

void Hapbeat::removeSession(uint8_t slot) {
  if (slot >= MAX_STREAM_SESSIONS) return;
  _sessions[slot] = StreamSession();
}

// BEGIN carries the endpoint's current address (it may have changed since the
// session began). A repeated Stream Gen 2 BEGIN is the same tuple.
bool Hapbeat::sendStreamBegin(StreamSession& session, uint64_t now) {
  uint8_t packet[128];
  size_t length = 0;
  if (session.gen2) {
    if (session.generation == 0) return false;
    length = hapbeat::buildStreamBeginGen2(
        packet, sizeof(packet), nextSeq(), session.bootId, session.ticket,
        session.generation, STREAM_SAMPLE_RATE, STREAM_CHANNELS,
        hapbeat::FORMAT_PCM16, 0, 1.0f, session.endpoint.address);
  } else {
    length = hapbeat::buildStreamBegin(
        packet, sizeof(packet), nextSeq(), STREAM_SAMPLE_RATE, STREAM_CHANNELS,
        hapbeat::FORMAT_PCM16, 0, 1.0f, session.endpoint.address);
  }
  session.lastBeginMs = now;
  return length != 0 && sendStreamTo(packet, length, session.endpoint);
}

// END once, in the session's own format and identity. Only a Stream Gen 1 END
// starts the 300 ms guard for that endpoint; a Gen 2 END is remembered so an
// ERROR about the just-ended session is still recognized.
void Hapbeat::sendStreamEnd(StreamSession& session, uint64_t now) {
  uint8_t packet[hapbeat::HEADER_SIZE + hapbeat::STREAM_ENVELOPE_SIZE];
  if (!session.gen2) {
    const size_t length =
        hapbeat::buildStreamEnd(packet, sizeof(packet), nextSeq());
    sendStreamTo(packet, length, session.endpoint);
    recordEnd(session.endpoint, now);
    return;
  }
  if (session.generation == 0) return;  // suspended: already ended
  const size_t length = hapbeat::buildStreamEndGen2(
      packet, sizeof(packet), nextSeq(), session.bootId, session.ticket,
      session.generation);
  sendStreamTo(packet, length, session.endpoint);
  const int8_t slot = findDevice(session.endpoint.ip, session.endpoint.port);
  if (slot < 0) return;
  _devices[slot].endedBootId = session.bootId;
  _devices[slot].endedTicket = session.ticket;
  _devices[slot].endedGeneration = session.generation;
}

// Next generation under the device's current lease. Generations are never
// reused; when none is left a fresh incarnation obtains a new ticket.
bool Hapbeat::allocateGeneration(StreamSession& session, uint64_t now) {
  (void)now;
  const int8_t slot = findDevice(session.endpoint.ip, session.endpoint.port);
  if (slot < 0) return false;
  KnownDevice& device = _devices[slot];
  if (device.lastGeneration >= kMaxGeneration) {
    newIncarnation();
    return false;
  }
  session.bootId = device.bootId;
  session.ticket = device.ticket;
  session.generation = ++device.lastGeneration;
  session.dataBlocksSent = 0;
  session.lastDataMs = 0;
  return true;
}

bool Hapbeat::openSession(uint8_t slot, const StreamEndpoint& endpoint,
                          EndpointMode mode, uint64_t now) {
  StreamSession& session = _sessions[slot];
  session = StreamSession();
  session.used = true;
  session.endpoint = endpoint;
  session.startedMs = now;
  if (mode == EndpointMode::Gen2) {
    session.gen2 = true;
    if (!allocateGeneration(session, now)) {
      session = StreamSession();
      return false;
    }
  }
  // No fallback is allowed. The session remains endpoint-scoped even if this
  // individual unicast attempt is rejected by the TX queue.
  session.beginSent = sendStreamBegin(session, now);
  return true;
}

// End a Stream Gen 2 session under its own identity and continue its sources
// from their cursors under the next generation of the device's current lease
// (a new ticket of the same boot, or the own-gap rule). Pacing restarts, so
// the time the sender could not send is not sent in one burst.
void Hapbeat::restartSession(StreamSession& session, uint64_t now) {
  sendStreamEnd(session, now);
  session.generation = 0;
  session.beginSent = false;
  session.byteOffset = 0;
  session.framesSent = 0;
  session.startedMs = now;
  if (!allocateGeneration(session, now)) {
    session.suspendedMs = now;  // resumes under the next lease of this boot
    return;
  }
  session.beginSent = sendStreamBegin(session, now);
}

bool Hapbeat::sourceCompletedEverywhere(
    uint8_t sourceSlot, const StreamEndpoint* endpoints,
    uint8_t endpointCount) const {
  if (sourceSlot >= MAX_STREAM_SOURCES || !_sources[sourceSlot].used) return false;
  bool matched = false;
  for (uint8_t i = 0; i < endpointCount; ++i) {
    if (!sourceMatches(_sources[sourceSlot], endpoints[i])) continue;
    matched = true;
    const int8_t sessionSlot = findSession(endpoints[i]);
    if (sessionSlot < 0 || !_sessions[sessionSlot].completed[sourceSlot]) return false;
  }
  return matched;
}

void Hapbeat::completeFinishedSources(const StreamEndpoint* endpoints,
                                      uint8_t endpointCount) {
  for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
    SineSource& source = _sources[i];
    if (!source.used || source.totalFrames == 0 || source.loop) continue;
    if (!sourceCompletedEverywhere(i, endpoints, endpointCount)) continue;
    source.status = HapbeatPlaybackStatus::Stopped;
    source.reason = HapbeatDeferredReason::None;
    source.used = false;
  }
}

void Hapbeat::reconcileStreams(uint64_t now) {
  if (!_ready) return;
  StreamEndpoint endpoints[MAX_DEVICES];
  const uint8_t endpointCount = collectStreamEndpoints(endpoints, MAX_DEVICES);
  bool claimed[MAX_DEVICES] = {false};
  bool sessionClaimed[MAX_STREAM_SESSIONS] = {false};

  // Claim every exact endpoint before considering migration. This global
  // first pass prevents a lower session slot from consuming an endpoint that
  // belongs to an exact higher-slot session.
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used) continue;
    for (uint8_t e = 0; e < endpointCount; ++e) {
      if (claimed[e] || !sameEndpoint(_sessions[s].endpoint, endpoints[e]))
        continue;
      _sessions[s].endpoint = endpoints[e];
      claimed[e] = true;
      sessionClaimed[s] = true;
      break;
    }
  }

  // A same-IP/port route is the explicit migration seam. Compute all route
  // candidates first and accept only one-to-one edges, independent of session
  // slot order.
  uint8_t routeCandidate[MAX_STREAM_SESSIONS] = {0};
  uint8_t routeCount[MAX_STREAM_SESSIONS] = {0};
  uint8_t routeAvailableCount[MAX_STREAM_SESSIONS] = {0};
  uint8_t routeEndpointCount[MAX_DEVICES] = {0};
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used || sessionClaimed[s]) continue;
    for (uint8_t e = 0; e < endpointCount; ++e) {
      if (!sameIpPort(_sessions[s].endpoint, endpoints[e])) continue;
      ++routeCount[s];
      if (!claimed[e]) {
        routeCandidate[s] = e;
        ++routeAvailableCount[s];
      }
    }
  }
  for (uint8_t e = 0; e < endpointCount; ++e) {
    if (claimed[e]) continue;
    for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s)
      if (_sessions[s].used && !sessionClaimed[s] && routeCount[s] == 1 &&
          routeAvailableCount[s] == 1 &&
          routeCandidate[s] == e)
        ++routeEndpointCount[e];
  }
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used || sessionClaimed[s] || routeCount[s] != 1 ||
        routeAvailableCount[s] != 1)
      continue;
    const uint8_t e = routeCandidate[s];
    if (claimed[e] || routeEndpointCount[e] != 1) continue;
    _sessions[s].endpoint = endpoints[e];
    claimed[e] = true;
    sessionClaimed[s] = true;
  }

  // Address-only migration is allowed only for one old session and one new
  // endpoint. Ambiguous stale/live sessions (including same-address devices
  // on different IPs) are never given a cursor-preserving migration, nor is a
  // session whose format or lease the new route does not have.
  uint8_t addressCandidate[MAX_STREAM_SESSIONS] = {0};
  uint8_t addressCount[MAX_STREAM_SESSIONS] = {0};
  uint8_t addressAvailableCount[MAX_STREAM_SESSIONS] = {0};
  uint8_t addressEndpointCount[MAX_DEVICES] = {0};
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used || sessionClaimed[s] || routeCount[s] != 0)
      continue;
    for (uint8_t e = 0; e < endpointCount; ++e) {
      if (strcmp(_sessions[s].endpoint.address, endpoints[e].address) != 0 ||
          !sessionCurrent(_sessions[s], endpoints[e]))
        continue;
      ++addressCount[s];
      if (!claimed[e]) {
        addressCandidate[s] = e;
        ++addressAvailableCount[s];
      }
    }
  }
  for (uint8_t e = 0; e < endpointCount; ++e) {
    if (claimed[e]) continue;
    for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s)
      if (_sessions[s].used && !sessionClaimed[s] && addressCount[s] == 1 &&
          addressAvailableCount[s] == 1 && addressCandidate[s] == e)
        ++addressEndpointCount[e];
  }
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used || sessionClaimed[s] || addressCount[s] != 1 ||
        addressAvailableCount[s] != 1)
      continue;
    const uint8_t e = addressCandidate[s];
    if (claimed[e] || addressEndpointCount[e] != 1) continue;
    _sessions[s].endpoint = endpoints[e];
    claimed[e] = true;
    sessionClaimed[s] = true;
  }

  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (_sessions[s].used && !sessionClaimed[s]) {
      // A vanished or ambiguous endpoint is forgotten without END. If it
      // later returns it is a new session and finite sources begin at frame 0.
      removeSession(s);
    }
  }

  // A matched reply or an ERROR may have changed an endpoint's class or
  // lease. End the running session now, in the format and identity it began
  // with, before a session in the new mode can start (stream-session.md:
  // lease state changes must propagate; a class change ends the old-format
  // session first). A new ticket of the same boot continues the cursors; a
  // lease that became unavailable suspends the session with its cursors for
  // the next lease of that boot; a superseded endpoint is not re-taken.
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (!session.used) continue;
    const EndpointMode mode = endpointMode(session.endpoint);
    const int8_t device = findDevice(session.endpoint.ip, session.endpoint.port);
    const bool sameBoot = session.gen2 && mode == EndpointMode::Gen2 &&
                          device >= 0 && _devices[device].bootId == session.bootId;
    if (session.gen2 && session.generation == 0) {
      if (sameBoot) {
        if (allocateGeneration(session, now)) {
          session.byteOffset = 0;
          session.framesSent = 0;
          session.startedMs = now;
          session.beginSent = sendStreamBegin(session, now);
        }
      } else if (mode != EndpointMode::LeaseUnavailable ||
                 now - session.suspendedMs >= LEASE_TTL_MS) {
        removeSession(s);
      }
      continue;
    }
    if (sessionCurrent(session, session.endpoint)) continue;
    if (sameBoot) {
      restartSession(session, now);
      continue;
    }
    if (session.beginSent) sendStreamEnd(session, now);
    if (session.gen2 && mode == EndpointMode::LeaseUnavailable) {
      session.generation = 0;
      session.beginSent = false;
      session.suspendedMs = now;
    } else {
      removeSession(s);
    }
  }

  // BEGIN is an exact-unicast handshake. A failed TX must leave the session
  // deferred and retry on the next cooperative tick before any DATA is sent.
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (session.used && !session.empty && !session.beginSent &&
        !(session.gen2 && session.generation == 0))
      session.beginSent = sendStreamBegin(session, now);
  }

  // Remove stale source memberships while preserving an empty session during
  // the linger interval.
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (!session.used) continue;
    bool hasAttached = false;
    for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
      if (!session.attached[i]) continue;
      if (!_sources[i].used || session.completed[i] ||
          !sourceMatches(_sources[i], session.endpoint)) {
        session.attached[i] = false;
      } else {
        hasAttached = true;
      }
    }
    if (hasAttached) {
      session.empty = false;
    } else if (!session.empty) {
      session.empty = true;
      session.emptySinceMs = now;
    }
  }

  // Resolve every source independently against every exact address endpoint.
  // Each endpoint gets the format of its class: Stream Gen 2 under its lease,
  // Stream Gen 1 with the 300 ms guard, or nothing until it is classified.
  for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
    SineSource& source = _sources[i];
    if (!source.used) continue;
    bool assigned = false;
    bool guarded = false;
    HapbeatDeferredReason reason = HapbeatDeferredReason::NoResolvedEndpoint;
    for (uint8_t e = 0; e < endpointCount; ++e) {
      if (!sourceMatches(source, endpoints[e])) continue;
      int8_t sessionSlot = findSession(endpoints[e]);
      const bool suspended = sessionSlot >= 0 && _sessions[sessionSlot].gen2 &&
                             _sessions[sessionSlot].generation == 0;
      if (suspended) {
        reason = strongerReason(reason, HapbeatDeferredReason::LeaseUnavailable);
      } else if (sessionSlot < 0) {
        const EndpointMode mode = endpointMode(endpoints[e]);
        if (mode == EndpointMode::Unknown) {
          reason = strongerReason(reason, HapbeatDeferredReason::UnknownClass);
          continue;
        }
        if (mode == EndpointMode::LeaseUnavailable) {
          reason = strongerReason(reason, HapbeatDeferredReason::LeaseUnavailable);
          continue;
        }
        if (mode == EndpointMode::Superseded) {
          reason = strongerReason(reason, HapbeatDeferredReason::Superseded);
          continue;
        }
        // Waiting out the Stream Gen 1 guard counts as Active (section 3).
        if (mode == EndpointMode::Gen1 && endpointCoolingDown(endpoints[e], now)) {
          guarded = true;
          continue;
        }
        for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
          if (_sessions[s].used) continue;
          if (openSession(s, endpoints[e], mode, now)) {
            sessionSlot = s;
          } else {
            reason = strongerReason(reason,
                                    HapbeatDeferredReason::LeaseUnavailable);
          }
          break;
        }
      }
      if (sessionSlot < 0) continue;  // fixed session pool is full
      StreamSession& session = _sessions[sessionSlot];
      if (session.completed[i]) continue;
      if (!session.attached[i]) {
        if (session.empty) {
          const uint64_t bufferedFrames =
              session.framesSent > STREAM_SEND_AHEAD_FRAMES
                  ? session.framesSent - STREAM_SEND_AHEAD_FRAMES
                  : 0;
          const uint64_t baselineMs =
              (bufferedFrames * 1000) / STREAM_SAMPLE_RATE;
          session.startedMs = now - baselineMs;
        }
        session.cursors[i] = 0;  // every new endpoint starts the source at zero
        session.attached[i] = true;
        session.empty = false;
      }
      if (!suspended) assigned = assigned || session.beginSent;
    }
    const bool active = assigned || guarded;
    source.status = active ? HapbeatPlaybackStatus::Active
                           : HapbeatPlaybackStatus::Deferred;
    source.reason = active ? HapbeatDeferredReason::None : reason;
  }
}

void Hapbeat::mixSession(StreamSession& session, uint16_t frames,
                         uint8_t* packet, size_t packetCapacity,
                         uint8_t& finishedMask) {
  float left[STREAM_CHUNK_FRAMES] = {0};
  float right[STREAM_CHUNK_FRAMES] = {0};
  finishedMask = 0;

  for (uint8_t sourceSlot = 0; sourceSlot < MAX_STREAM_SOURCES; ++sourceSlot) {
    if (!session.attached[sourceSlot] || !_sources[sourceSlot].used) continue;
    SineSource& source = _sources[sourceSlot];
    uint64_t cursor = session.cursors[sourceSlot];
    const float pan = source.pan;
    const float gain = source.gain;
    const float leftGain = gain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
    const float rightGain = gain * (pan >= 0.0f ? 1.0f : 1.0f + pan);
    for (uint16_t frame = 0; frame < frames; ++frame) {
      if (source.totalFrames != 0 && cursor >= source.totalFrames) {
        if (source.loop) {
          cursor = 0;
        } else {
          finishedMask |= (uint8_t)(1u << sourceSlot);
          break;
        }
      }
      // The cursor is a modulo-2^32 phase index. Integer multiplication keeps
      // phase exact across long-lived sources and its defined wrap is a full
      // phase wrap, unlike multiplying a large cursor by a float.
      const uint32_t phase =
          (uint32_t)((uint64_t)source.phaseStep * (uint64_t)cursor);
      const float sample = (float)source.amplitude *
                           sinf((float)phase * kPhaseToRadians);
      left[frame] += sample * leftGain;
      right[frame] += sample * rightGain;
      ++cursor;
    }
    session.cursors[sourceSlot] = cursor;
    if (source.totalFrames != 0 && !source.loop &&
        cursor >= source.totalFrames)
      finishedMask |= (uint8_t)(1u << sourceSlot);
  }

  // Stream Gen 2 DATA puts the 16-byte envelope before the offset.
  const size_t audioStart = hapbeat::HEADER_SIZE +
                            (session.gen2 ? hapbeat::STREAM_ENVELOPE_SIZE : 0) + 4;
  for (uint16_t frame = 0; frame < frames; ++frame) {
    const int16_t leftSample = saturatePcm16(left[frame]);
    const int16_t rightSample = saturatePcm16(right[frame]);
    const size_t offset = audioStart + (size_t)frame * 4;
    packet[offset] = (uint8_t)(leftSample & 0xFF);
    packet[offset + 1] = (uint8_t)((leftSample >> 8) & 0xFF);
    packet[offset + 2] = (uint8_t)(rightSample & 0xFF);
    packet[offset + 3] = (uint8_t)((rightSample >> 8) & 0xFF);
  }
  const size_t audioLength = (size_t)frames * STREAM_CHANNELS * 2;
  // packet[audioStart..] already contains the raw data; the builder writes
  // only the header/envelope/offset before moving that same range into place.
  if (session.gen2) {
    (void)hapbeat::buildStreamDataGen2(
        packet, packetCapacity, nextSeq(), session.bootId, session.ticket,
        session.generation, session.byteOffset, packet + audioStart,
        audioLength);
  } else {
    (void)hapbeat::buildStreamData(packet, packetCapacity, nextSeq(),
                                   session.byteOffset, packet + audioStart,
                                   audioLength);
  }
}

void Hapbeat::pumpStreams(uint64_t now) {
  uint8_t packet[hapbeat::HEADER_SIZE + hapbeat::STREAM_ENVELOPE_SIZE + 4 +
                 STREAM_CHUNK_FRAMES * 4];
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (!session.used) continue;
    if (session.gen2 && session.generation == 0) {
      // Suspended until the next lease: no DATA. Forget it once empty.
      bool hasAttached = false;
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
        if (session.attached[i]) hasAttached = true;
      if (!hasAttached) removeSession(s);
      continue;
    }
    if (!session.beginSent) {
      bool hasAttached = false;
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
        if (session.attached[i]) hasAttached = true;
      if (!hasAttached) {
        if (!session.empty) {
          session.empty = true;
          session.emptySinceMs = now;
        } else if (now - session.emptySinceMs >= SESSION_LINGER_MS) {
          removeSession(s);
        }
      }
      continue;
    }

    bool hasAttached = false;
    uint16_t maximum = 0;
    for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
      if (!session.attached[i]) continue;
      hasAttached = true;
      const SineSource& source = _sources[i];
      if (source.totalFrames == 0 || source.loop) {
        maximum = STREAM_CHUNK_FRAMES;
        break;
      }
      if (session.cursors[i] < source.totalFrames) {
        const uint64_t remaining = source.totalFrames - session.cursors[i];
        if (remaining > maximum)
          maximum = remaining > STREAM_CHUNK_FRAMES ? STREAM_CHUNK_FRAMES
                                                    : (uint16_t)remaining;
      }
    }
    if (session.gen2) {
      // Own gap (Sender requirements 3): the device may have timed this
      // generation out, so continue under the next one before more DATA.
      if (hasAttached && session.dataBlocksSent > 0 &&
          now - session.lastDataMs >= OWN_GAP_RESTART_MS) {
        restartSession(session, now);
        if (session.generation == 0 || !session.beginSent) continue;
      }
      if (now - session.lastBeginMs >= BEGIN_REPEAT_MS)
        sendStreamBegin(session, now);
    }
    const uint64_t elapsedMs = now - session.startedMs;
    const uint64_t wanted =
        ((uint64_t)elapsedMs * STREAM_SAMPLE_RATE) / 1000 +
        STREAM_SEND_AHEAD_FRAMES;
    for (uint8_t chunk = 0; hasAttached && chunk < MAX_CHUNKS_PER_TICK &&
                             session.framesSent < wanted;
         ++chunk) {
      maximum = 0;
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
        if (!session.attached[i]) continue;
        const SineSource& source = _sources[i];
        if (source.totalFrames == 0 || source.loop) {
          maximum = STREAM_CHUNK_FRAMES;
          break;
        }
        if (session.cursors[i] < source.totalFrames) {
          const uint64_t remaining = source.totalFrames - session.cursors[i];
          const uint16_t available = remaining > STREAM_CHUNK_FRAMES
                                         ? STREAM_CHUNK_FRAMES
                                         : (uint16_t)remaining;
          if (available > maximum) maximum = available;
        }
      }
      if (maximum == 0) break;
      if (maximum > STREAM_CHUNK_FRAMES) maximum = STREAM_CHUNK_FRAMES;

      // Stream Gen 2 repeats the same BEGIN before each of the first DATA
      // blocks; Stream Gen 1 firmware may flush on BEGIN, so never there.
      if (session.gen2 && session.dataBlocksSent < BEGIN_REPEAT_COUNT)
        sendStreamBegin(session, now);
      uint64_t oldCursors[MAX_STREAM_SOURCES];
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
        oldCursors[i] = session.cursors[i];
      uint8_t finishedMask = 0;
      mixSession(session, maximum, packet, sizeof(packet), finishedMask);
      const size_t packetLength =
          hapbeat::HEADER_SIZE +
          (session.gen2 ? hapbeat::STREAM_ENVELOPE_SIZE : 0) + 4 +
          (size_t)maximum * STREAM_CHANNELS * 2;
      if (!sendStreamTo(packet, packetLength, session.endpoint)) {
        for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
          session.cursors[i] = oldCursors[i];
        break;
      }
      session.byteOffset += (uint32_t)maximum * STREAM_CHANNELS * 2;
      session.framesSent += maximum;
      if (session.dataBlocksSent < 255) ++session.dataBlocksSent;
      session.lastDataMs = now;
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
        if (!(finishedMask & (uint8_t)(1u << i))) continue;
        session.attached[i] = false;
        session.completed[i] = true;
      }
      hasAttached = false;
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
        if (session.attached[i]) hasAttached = true;
    }

    if (!hasAttached) {
      if (!session.empty) {
        session.empty = true;
        session.emptySinceMs = now;
      } else if (now - session.emptySinceMs >= SESSION_LINGER_MS) {
        sendStreamEnd(session, now);
        removeSession(s);
      }
    }
  }

  StreamEndpoint endpoints[MAX_DEVICES];
  const uint8_t endpointCount = collectStreamEndpoints(endpoints, MAX_DEVICES);
  completeFinishedSources(endpoints, endpointCount);
}

void Hapbeat::stopSource(uint8_t slot, uint16_t id, uint64_t generation) {
  if (slot >= MAX_STREAM_SOURCES) return;
  SineSource& source = _sources[slot];
  if (!source.used || source.id != id || source.generation != generation) return;
  source.status = HapbeatPlaybackStatus::Stopped;
  source.reason = HapbeatDeferredReason::None;
  source.used = false;
  const uint64_t now = extendedMillis();
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (!session.used || !session.attached[slot]) continue;
    session.attached[slot] = false;
    if (!session.empty) {
      session.empty = true;
      session.emptySinceMs = now;
    }
  }
}

void Hapbeat::setSourceGain(uint8_t slot, uint16_t id, uint64_t generation,
                            float value) {
  if (!isfinite(value)) return;
  if (slot < MAX_STREAM_SOURCES && _sources[slot].used &&
      _sources[slot].id == id && _sources[slot].generation == generation)
    _sources[slot].gain = clampFloat(value, 0.0f, 1.0f);
}

void Hapbeat::setSourcePan(uint8_t slot, uint16_t id, uint64_t generation,
                           float value) {
  if (!isfinite(value)) return;
  if (slot < MAX_STREAM_SOURCES && _sources[slot].used &&
      _sources[slot].id == id && _sources[slot].generation == generation)
    _sources[slot].pan = clampFloat(value, -1.0f, 1.0f);
}

void Hapbeat::setSourceLoop(uint8_t slot, uint16_t id, uint64_t generation,
                            bool value) {
  if (slot >= MAX_STREAM_SOURCES || !_sources[slot].used ||
      _sources[slot].id != id || _sources[slot].generation != generation)
    return;
  _sources[slot].loop = value;
  if (value) {
    for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s)
      _sessions[s].completed[slot] = false;
    reconcileStreams(extendedMillis());
  }
}

HapbeatPlaybackStatus Hapbeat::playbackStatus(uint8_t slot, uint16_t id,
                                               uint64_t generation) const {
  if (slot >= MAX_STREAM_SOURCES || !_sources[slot].used ||
      _sources[slot].id != id || _sources[slot].generation != generation)
    return HapbeatPlaybackStatus::Stopped;
  return _sources[slot].status;
}

HapbeatDeferredReason Hapbeat::playbackReason(uint8_t slot, uint16_t id,
                                              uint64_t generation) const {
  if (slot >= MAX_STREAM_SOURCES || !_sources[slot].used ||
      _sources[slot].id != id || _sources[slot].generation != generation)
    return HapbeatDeferredReason::None;
  return _sources[slot].reason;
}

bool Hapbeat::playbackValues(uint8_t slot, uint16_t id, uint64_t generation,
                             float& gain, float& pan, bool& loop) const {
  if (slot >= MAX_STREAM_SOURCES || !_sources[slot].used ||
      _sources[slot].id != id || _sources[slot].generation != generation)
    return false;
  gain = _sources[slot].gain;
  pan = _sources[slot].pan;
  loop = _sources[slot].loop;
  return true;
}

void Hapbeat::clearAllSources(bool sendEnds) {
  for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
    if (!_sources[i].used) continue;
    _sources[i].status = HapbeatPlaybackStatus::Stopped;
    _sources[i].reason = HapbeatDeferredReason::None;
    _sources[i].used = false;
  }
  const uint64_t now = extendedMillis();
  for (uint8_t i = 0; i < MAX_STREAM_SESSIONS; ++i) {
    StreamSession& session = _sessions[i];
    if (!session.used) continue;
    for (uint8_t source = 0; source < MAX_STREAM_SOURCES; ++source)
      session.attached[source] = false;
    if (sendEnds && session.beginSent) {
      sendStreamEnd(session, now);
      removeSession(i);
    } else if (sendEnds) {
      removeSession(i);
    } else if (!session.empty) {
      session.empty = true;
      session.emptySinceMs = now;
    }
  }
}
