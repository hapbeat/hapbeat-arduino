// Hapbeat.cpp — see Hapbeat.h.
#include "Hapbeat.h"

#include <math.h>
#include <string.h>

bool Hapbeat::begin(uint16_t port, const char* appName) {
  _port = port;
  if (appName) {
    strncpy(_appName, appName, hapbeat::MAX_APP_NAME_LEN);
    _appName[hapbeat::MAX_APP_NAME_LEN] = '\0';
  }
  _udp.begin(_port);  // bind 7700 to send and to receive PONG replies (discovery)
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

bool Hapbeat::sendTo(const uint8_t* buf, size_t len, IPAddress ip) {
  if (!_ready || len == 0) return false;
  _udp.beginPacket(ip, _port);
  _udp.write(buf, len);
  return _udp.endPacket() != 0;
}

// Unicast to every device we have heard from; broadcast only while we know
// none. Unicast frames are acked and retried by the MAC and are not held back
// by the AP's DTIM buffering, so they arrive both more reliably and sooner
// (broadcast can be delayed up to one beacon interval, 100-300 ms, whenever
// any client on the AP is power-saving — including clients unrelated to us).
//
// Never both: firmware older than the (source ip, seq) dedupe would fire the
// same PLAY twice. And the device re-applies the target filter on receipt, so
// broadcasting while the table is empty cannot fire a device the target did
// not address.
bool Hapbeat::sendRouted(const uint8_t* buf, size_t len) {
  if (!_ready || len == 0) return false;

  // Poll BEFORE the broadcast-only branch, not after. Arriving PONGs are only
  // consumed here and in poll()/deviceCount(), so skipping this while
  // broadcast-only is set would let the whole table age out: switching back to
  // unicast would then find 0 devices and silently keep broadcasting until
  // something else happened to poll. Found on hardware, where a sketch that
  // toggles setBroadcastOnly() reported devices=0 after one 15 s broadcast
  // phase.
  pollPongsThrottled();  // keep the table fresh without the sketch having to

  if (_broadcastOnly) return sendPacket(buf, len);

  bool anyKnown = false;
  bool sent = false;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    if (!isAlive(_devices[i])) continue;
    anyKnown = true;
    // One unreachable device must not stop the packet reaching the others,
    // so a failed send is skipped rather than aborting the loop.
    if (sendTo(buf, len, _devices[i].ip)) {
      sent = true;
      _devices[i].fails = 0;
    } else if (!_devices[i].pinned && ++_devices[i].fails >= kMaxSendFails) {
      _devices[i] = KnownDevice();  // dead destination -- stop hammering it
    }
  }
  if (anyKnown) return sent;
  return sendPacket(buf, len);  // nobody known yet -> broadcast
}

bool Hapbeat::isAlive(const KnownDevice& d) const {
  if ((uint32_t)d.ip == 0) return false;
  if (d.pinned) return true;
  // Unsigned subtraction, so this stays correct across the millis() wrap.
  return (millis() - d.lastPongMs) < _deviceTimeoutMs;
}

uint8_t Hapbeat::aliveCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i)
    if (isAlive(_devices[i])) ++n;
  return n;
}

void Hapbeat::rememberDevice(IPAddress ip, bool pinned) {
  if ((uint32_t)ip == 0) return;
  int8_t free_slot = -1, oldest = -1;
  uint32_t oldest_age = 0;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    KnownDevice& d = _devices[i];
    if (d.ip == ip) {  // already known: refresh
      d.lastPongMs = millis();
      d.fails = 0;  // it just answered, so it is reachable again
      if (pinned) d.pinned = true;
      return;
    }
    if (!isAlive(d)) {
      free_slot = i;  // free or expired slot, reuse it
    } else if (!d.pinned) {
      const uint32_t age = millis() - d.lastPongMs;
      if (oldest < 0 || age > oldest_age) { oldest = i; oldest_age = age; }
    }
  }
  // Table full of live devices: evict the least recently heard from (never a
  // pinned one). More than MAX_DEVICES on one LAN is a broadcast setup anyway.
  const int8_t slot = (free_slot >= 0) ? free_slot : oldest;
  if (slot < 0) return;
  _devices[slot].ip = ip;
  _devices[slot].lastPongMs = millis();
  _devices[slot].pinned = pinned;
  _devices[slot].fails = 0;
}

void Hapbeat::pollPongs() {
  if (!_ready) return;
  // Bounded so a burst of traffic cannot stall the caller (streaming calls
  // this between audio chunks).
  for (uint8_t guard = 0; guard < MAX_DEVICES; ++guard) {
    const int sz = _udp.parsePacket();
    if (sz < (int)hapbeat::HEADER_SIZE) return;
    uint8_t rb[16];
    const int n = _udp.read(rb, sizeof(rb));
    // Only the header is needed, but the REST OF THE DATAGRAM MUST BE DROPPED:
    // ESP32's WiFiUDP::parsePacket() starts with `if (rx_buffer) return 0;`, so
    // leaving a partially-read packet in the buffer makes every later call
    // return 0 — reception wedges permanently. A device PONG is ~60 bytes
    // (header + timestamps + name/address/firmware), so reading 16 always
    // leaves a remainder. Found on hardware: the table emptied 15 s after
    // discover() and never refilled, silently dropping unicast back to
    // broadcast. remoteIP() is unaffected by flush().
    _udp.flush();
    if (n < (int)hapbeat::HEADER_SIZE) continue;
    const bool magic_ok = rb[0] == (uint8_t)(hapbeat::MAGIC & 0xFF) &&
                          rb[1] == (uint8_t)((hapbeat::MAGIC >> 8) & 0xFF);
    if (magic_ok && rb[2] == hapbeat::VERSION && rb[3] == hapbeat::CMD_PONG)
      rememberDevice(_udp.remoteIP());
  }
}

void Hapbeat::pollPongsThrottled() {
  const uint32_t now = millis();
  if ((now - _lastPollMs) < 100) return;
  _lastPollMs = now;
  pollPongs();
}

void Hapbeat::poll() { pollPongs(); }

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
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_DEVICES && n < maxOut; ++i)
    if (isAlive(_devices[i])) out[n++] = _devices[i].ip;
  return n;
}

void Hapbeat::setDeviceIp(IPAddress ip) {
  if ((uint32_t)ip == 0) {  // clear pinned entries
    for (uint8_t i = 0; i < MAX_DEVICES; ++i)
      if (_devices[i].pinned) _devices[i] = KnownDevice();
    return;
  }
  rememberDevice(ip, /*pinned=*/true);
}

bool Hapbeat::discover(uint32_t timeoutMs) {
  if (!_ready) return false;
  ping();
  // Keep listening for the whole window instead of returning on the first
  // reply: every device that answers becomes a unicast destination, and
  // stopping early would leave a multi-device setup driving only one of them.
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    pollPongs();
    delay(5);
  }
  return deviceCount() > 0;
}

void Hapbeat::play(const char* eventId, float gain, const char* target) {
  uint8_t buf[256];
  sendRouted(buf, hapbeat::buildPlay(buf, sizeof(buf), nextSeq(), eventId, target, 0, gain));
}

void Hapbeat::stop(const char* eventId, const char* target) {
  uint8_t buf[256];
  sendRouted(buf, hapbeat::buildStop(buf, sizeof(buf), nextSeq(), eventId, target));
}

void Hapbeat::stopAll(const char* target) {
  uint8_t buf[128];
  sendRouted(buf, hapbeat::buildStopAll(buf, sizeof(buf), nextSeq(), target));
}

void Hapbeat::ping() {
  uint8_t buf[16];
  // Broadcast: discovery has to reach devices we have not heard from yet.
  // millis()-based microseconds: fine for RTT — the device just echoes it back.
  sendPacket(buf, hapbeat::buildPing(buf, sizeof(buf), nextSeq(), (int64_t)millis() * 1000));
}

void Hapbeat::connectStatus(bool connected) {
  uint8_t buf[64];
  sendPacket(buf, hapbeat::buildConnectStatus(buf, sizeof(buf), nextSeq(), connected,
                                              _group, _appName, ""));
}

void Hapbeat::end() {
  if (_ready && _appName[0]) connectStatus(false);
  _ready = false;
}

// ~160 ms of audio kept buffered in the device's ~256 ms ring so Wi-Fi jitter
// never starves playback. (Just-in-time pacing underran on any network hiccup,
// which was heard as choppiness.) Chunks are sent as fast as possible until the
// estimated ring depth reaches this target, then topped up as the device drains.
static const uint16_t kSineChunk = 256;                 // samples/packet (16 ms @ 16 kHz)
static const uint32_t kSineTargetLead = 16000 * 160 / 1000;  // ~2560 samples (~160 ms)
// Max STREAM_DATA chunks emitted per pumpSine() call. Caps the start/catch-up
// burst so a single synchronous beginPacket/endPacket storm cannot overrun the
// ESP32 lwIP/Wi-Fi TX path (which silently drops). The caller (loop()/playSine)
// tops up the remaining lead across subsequent calls.
static const int kSineMaxChunksPerPump = 4;

void Hapbeat::beginSine(float freqHz, float intensity, const char* target) {
  if (!_ready || _sineActive) return;
  if (intensity < 0.0f) intensity = 0.0f;
  if (intensity > 1.0f) intensity = 1.0f;

  _sineRate = 16000;  // device clip-stream rate (16 kHz PCM16)
  _sinePhase = 0.0f;
  _sineDphase = 2.0f * (float)PI * freqHz / (float)_sineRate;
  _sineAmp = (int16_t)(0.8f * 32767.0f);  // 0.8 headroom; loudness via gain
  _sineSent = 0;
  _sineByteOffset = 0;
  _sineStartMs = millis();  // anchor before BEGIN: biases toward a fuller ring
  _sineActive = true;

  // STREAM_BEGIN: mono PCM16, open-ended (total_samples=0; device ignores it).
  // intensity rides the gain field (device-mixed). It is unacked UDP and gates
  // the whole stream — a single loss = total silence — so send it a few times
  // with a small gap. The device only flushes its ring on BEGIN if residual
  // exceeds ~32 ms (512 frames); here residual is ~0, so the dupes are no-ops.
  uint8_t hdr[64];
  const size_t beginLen = hapbeat::buildStreamBegin(hdr, sizeof(hdr), nextSeq(), _sineRate, 1,
                                                    hapbeat::FORMAT_PCM16, 0, intensity, target);
  for (int i = 0; i < 3; ++i) {
    sendRouted(hdr, beginLen);
    // Stays at 1 ms. Tried 3 ms to spread the N-destination burst, and it made
    // things WORSE on hardware (ENOMEM 8/50s -> 31/65s): the extra blocking
    // deepens the ring deficit, so pumpSine catches up harder afterwards.
    delay(1);
  }
  pumpSine();  // prime the ring (bounded per call, and per destination count)
}

void Hapbeat::pumpSine() {
  if (!_sineActive) return;
  int16_t pcm[kSineChunk];
  uint8_t pkt[hapbeat::HEADER_SIZE + 4 + kSineChunk * 2];
  // Unicasting to N devices multiplies the packet rate by N, so bound the burst
  // in PACKETS rather than chunks — otherwise the prime burst scales with the
  // fleet and exhausts the lwIP TX pool. Observed on hardware the moment
  // streaming to 2 devices started: a burst of
  // `endPacket(): could not send data: 12` (ENOMEM). Sends are dropped there,
  // which is a hole in the audio.
  const uint8_t dests = _broadcastOnly ? 1 : (aliveCount() > 0 ? aliveCount() : 1);
  const int maxChunks = (kSineMaxChunksPerPump + dests - 1) / dests;  // ceil, >= 1
  // Top the device ring up to the lead target; self-paces (sends nothing once
  // full). Phase-continuous across calls. Bounded per call so the prime/catch-up
  // is spread over several invocations instead of one synchronous TX storm.
  for (int chunks = 0; chunks < maxChunks; ++chunks) {
    const uint32_t elapsedMs = millis() - _sineStartMs;
    const uint32_t consumed = (uint32_t)((uint64_t)elapsedMs * _sineRate / 1000);
    const uint32_t buffered = (_sineSent > consumed) ? (_sineSent - consumed) : 0;
    if (buffered >= kSineTargetLead) break;
    for (uint16_t i = 0; i < kSineChunk; ++i) {
      pcm[i] = (int16_t)(_sineAmp * sinf(_sinePhase));
      _sinePhase += _sineDphase;
      if (_sinePhase >= 2.0f * (float)PI) _sinePhase -= 2.0f * (float)PI;
    }
    // PCM16 little-endian: native byte order on ESP32 / common Arduino MCUs.
    const bool ok = sendRouted(pkt,
        hapbeat::buildStreamData(pkt, sizeof(pkt), nextSeq(), _sineByteOffset,
                                 (const uint8_t*)pcm, (size_t)kSineChunk * 2));
    // TX queue full: stop topping up this call (don't count un-sent audio, or
    // the open-loop estimate would overshoot the device's true ring depth).
    if (!ok) break;
    _sineSent += kSineChunk;
    _sineByteOffset += (uint32_t)kSineChunk * 2;
  }
}

void Hapbeat::endSine() {
  if (!_sineActive) return;
  _sineActive = false;
  // STREAM_END is also unacked; resend a few times so a lost END can't leave the
  // device streaming (it would otherwise drain its ~160 ms tail and re-gate).
  uint8_t hdr[64];
  const size_t endLen = hapbeat::buildStreamEnd(hdr, sizeof(hdr), nextSeq());
  for (int i = 0; i < 3; ++i) {
    sendRouted(hdr, endLen);
    delay(1);  // see beginSine: longer blocking measured worse, not better
  }
}

void Hapbeat::playSine(float freqHz, float intensity, uint32_t durationMs, const char* target) {
  if (!_ready) return;
  beginSine(freqHz, intensity, target);
  const uint32_t startMs = millis();
  while (millis() - startMs < durationMs) {
    pumpSine();
    delay(2);  // yield to Wi-Fi/other tasks between top-ups
  }
  endSine();
}
