// Hapbeat.cpp — see Hapbeat.h.
#include "Hapbeat.h"

#include <math.h>
#include <string.h>

namespace {

constexpr size_t kPongBufferSize = 256;
constexpr float kPhaseToRadians = 6.28318530717958647692f / 4294967296.0f;
constexpr double kPhaseModulus = 4294967296.0;

bool readCString(const uint8_t* data, size_t end, size_t& pos, char* out,
                 size_t capacity) {
  if (!out || capacity == 0) return false;
  const size_t start = pos;
  while (pos < end && data[pos] != 0) ++pos;
  if (pos >= end || pos - start >= capacity) return false;
  memcpy(out, data + start, pos - start);
  out[pos - start] = '\0';
  ++pos;
  return true;
}

bool skipCString(const uint8_t* data, size_t end, size_t& pos) {
  while (pos < end && data[pos] != 0) ++pos;
  if (pos >= end) return false;
  ++pos;
  return true;
}

// PONG payload: timestamp(i64), server_time(i64), device_name\0,
// address\0, firmware_version\0. Only the complete address is needed by the
// stream router; a basic legacy PONG intentionally remains command-only.
bool parsePongAddress(const uint8_t* packet, size_t packetLength, char* address,
                      size_t capacity) {
  if (!packet || packetLength < hapbeat::HEADER_SIZE + 16) return false;
  const size_t payloadLength = (size_t)packet[6] | ((size_t)packet[7] << 8);
  const size_t packetEnd = hapbeat::HEADER_SIZE + payloadLength;
  if (packetEnd > packetLength || payloadLength < 16) return false;

  size_t pos = hapbeat::HEADER_SIZE + 16;
  if (!skipCString(packet, packetEnd, pos)) return false;
  if (!readCString(packet, packetEnd, pos, address, capacity)) return false;
  return address[0] != '\0';
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

void Hapbeat::rememberDevice(IPAddress ip, uint16_t port, const char* address,
                             bool pinned) {
  if ((uint32_t)ip == 0) return;
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

  if (slot < 0) slot = freeSlot >= 0 ? freeSlot : oldest;
  if (slot < 0) return;
  KnownDevice& device = _devices[slot];
  device.used = true;
  device.ip = ip;
  device.port = port;
  device.lastPongMs = now;
  device.fails = 0;
  if (pinned) device.pinned = true;
  if (hasAddress) copyAddress(device.address, sizeof(device.address), address);
}

void Hapbeat::pollPongs() {
  if (!_ready) return;
  for (uint8_t guard = 0; guard < MAX_DEVICES; ++guard) {
    const int packetSize = _udp.parsePacket();
    if (packetSize <= 0) return;
    const IPAddress remoteIp = _udp.remoteIP();
    uint16_t remotePort = _udp.remotePort();
    if (remotePort == 0) remotePort = _port;
    uint8_t packet[kPongBufferSize];
    const int readSize = _udp.read(packet, sizeof(packet));
    _udp.flush();
    if (packetSize > (int)sizeof(packet) || readSize < (int)hapbeat::HEADER_SIZE)
      continue;

    const bool magicOk = packet[0] == (uint8_t)(hapbeat::MAGIC & 0xFF) &&
                         packet[1] == (uint8_t)((hapbeat::MAGIC >> 8) & 0xFF);
    if (!magicOk || packet[2] != hapbeat::VERSION ||
        packet[3] != hapbeat::CMD_PONG)
      continue;
    const size_t packetLength = (size_t)readSize;
    const size_t payloadLength = (size_t)packet[6] | ((size_t)packet[7] << 8);
    if (hapbeat::HEADER_SIZE + payloadLength > packetLength || payloadLength < 16)
      continue;

    char address[hapbeat::MAX_DEVICE_ADDRESS_LEN + 1] = {0};
    const bool hasAddress = parsePongAddress(packet, packetLength, address,
                                              sizeof(address));
    rememberDevice(remoteIp, remotePort, hasAddress ? address : "");
  }
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
  if (!isfinite(gain) || !isfinite(pan)) return;
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
  const size_t length = hapbeat::buildPing(packet, sizeof(packet), nextSeq(),
                                           (int64_t)extendedMillis() * 1000);
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

bool Hapbeat::sendStreamBegin(StreamSession& session) {
  uint8_t packet[128];
  const size_t length = hapbeat::buildStreamBegin(
      packet, sizeof(packet), nextSeq(), STREAM_SAMPLE_RATE, STREAM_CHANNELS,
      hapbeat::FORMAT_PCM16, 0, 1.0f, session.endpoint.address);
  return length != 0 && sendStreamTo(packet, length, session.endpoint);
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
  // on different IPs) are never given a cursor-preserving migration.
  uint8_t addressCandidate[MAX_STREAM_SESSIONS] = {0};
  uint8_t addressCount[MAX_STREAM_SESSIONS] = {0};
  uint8_t addressAvailableCount[MAX_STREAM_SESSIONS] = {0};
  uint8_t addressEndpointCount[MAX_DEVICES] = {0};
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    if (!_sessions[s].used || sessionClaimed[s] || routeCount[s] != 0)
      continue;
    for (uint8_t e = 0; e < endpointCount; ++e) {
      if (strcmp(_sessions[s].endpoint.address, endpoints[e].address) != 0)
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

  // BEGIN is an exact-unicast handshake. A failed TX must leave the session
  // deferred and retry on the next cooperative tick before any DATA is sent.
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (session.used && !session.empty && !session.beginSent)
      session.beginSent = sendStreamBegin(session);
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
  for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i) {
    SineSource& source = _sources[i];
    if (!source.used) continue;
    bool assigned = false;
    for (uint8_t e = 0; e < endpointCount; ++e) {
      if (!sourceMatches(source, endpoints[e])) continue;
      int8_t sessionSlot = findSession(endpoints[e]);
      if (sessionSlot < 0) {
        if (endpointCoolingDown(endpoints[e], now)) continue;
        for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
          if (!_sessions[s].used) {
            sessionSlot = s;
            _sessions[s] = StreamSession();
            _sessions[s].used = true;
            _sessions[s].endpoint = endpoints[e];
            _sessions[s].startedMs = now;
            // No fallback is allowed. The session remains endpoint-scoped even
            // if this individual unicast attempt is rejected by the TX queue.
            _sessions[s].beginSent = sendStreamBegin(_sessions[s]);
            break;
          }
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
      assigned = assigned || session.beginSent;
    }
    source.status = assigned ? HapbeatPlaybackStatus::Active
                             : HapbeatPlaybackStatus::Deferred;
    source.reason = assigned ? HapbeatDeferredReason::None
                             : HapbeatDeferredReason::NoResolvedEndpoint;
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

  for (uint16_t frame = 0; frame < frames; ++frame) {
    const int16_t leftSample = saturatePcm16(left[frame]);
    const int16_t rightSample = saturatePcm16(right[frame]);
    const size_t offset = (size_t)frame * 4;
    packet[hapbeat::HEADER_SIZE + 4 + offset] = (uint8_t)(leftSample & 0xFF);
    packet[hapbeat::HEADER_SIZE + 5 + offset] = (uint8_t)((leftSample >> 8) & 0xFF);
    packet[hapbeat::HEADER_SIZE + 6 + offset] = (uint8_t)(rightSample & 0xFF);
    packet[hapbeat::HEADER_SIZE + 7 + offset] = (uint8_t)((rightSample >> 8) & 0xFF);
  }
  const size_t audioLength = (size_t)frames * STREAM_CHANNELS * 2;
  // packet[12..] already contains the raw data; the builder writes only the
  // header/offset before copying that same range into place.
  (void)hapbeat::buildStreamData(packet, packetCapacity, nextSeq(),
                                 session.byteOffset,
                                 packet + hapbeat::HEADER_SIZE + 4, audioLength);
}

void Hapbeat::pumpStreams(uint64_t now) {
  uint8_t packet[hapbeat::HEADER_SIZE + 4 + STREAM_CHUNK_FRAMES * 4];
  for (uint8_t s = 0; s < MAX_STREAM_SESSIONS; ++s) {
    StreamSession& session = _sessions[s];
    if (!session.used) continue;
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

      uint64_t oldCursors[MAX_STREAM_SOURCES];
      for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
        oldCursors[i] = session.cursors[i];
      uint8_t finishedMask = 0;
      mixSession(session, maximum, packet, sizeof(packet), finishedMask);
      const size_t packetLength = hapbeat::HEADER_SIZE + 4 +
                                  (size_t)maximum * STREAM_CHANNELS * 2;
      if (!sendStreamTo(packet, packetLength, session.endpoint)) {
        for (uint8_t i = 0; i < MAX_STREAM_SOURCES; ++i)
          session.cursors[i] = oldCursors[i];
        break;
      }
      session.byteOffset += (uint32_t)maximum * STREAM_CHANNELS * 2;
      session.framesSent += maximum;
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
        uint8_t endPacket[hapbeat::HEADER_SIZE];
        const size_t endLength =
            hapbeat::buildStreamEnd(endPacket, sizeof(endPacket), nextSeq());
        sendStreamTo(endPacket, endLength, session.endpoint);
        recordEnd(session.endpoint, now);
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
      uint8_t packet[hapbeat::HEADER_SIZE];
      const size_t length =
          hapbeat::buildStreamEnd(packet, sizeof(packet), nextSeq());
      sendStreamTo(packet, length, session.endpoint);
      recordEnd(session.endpoint, now);
      removeSession(i);
    } else if (sendEnds) {
      removeSession(i);
    } else if (!session.empty) {
      session.empty = true;
      session.emptySinceMs = now;
    }
  }
}
