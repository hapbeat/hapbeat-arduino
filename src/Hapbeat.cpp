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

bool Hapbeat::streamSend(const uint8_t* buf, size_t len) {
  // Unicast to a discovered device (Wi-Fi MAC-ACK + retry = far fewer drops,
  // ~ESP-NOW reliability); fall back to broadcast if not discovered.
  if ((uint32_t)_deviceIp != 0)
    return sendTo(buf, len, _deviceIp);
  return sendPacket(buf, len);
}

bool Hapbeat::discover(uint32_t timeoutMs) {
  if (!_ready) return false;
  uint8_t buf[16];
  sendPacket(buf, hapbeat::buildPing(buf, sizeof(buf), nextSeq(), (int64_t)millis() * 1000));
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    const int sz = _udp.parsePacket();
    if (sz >= (int)hapbeat::HEADER_SIZE) {
      uint8_t rb[16];
      _udp.read(rb, sizeof(rb));
      const bool magic_ok = rb[0] == (uint8_t)(hapbeat::MAGIC & 0xFF) &&
                            rb[1] == (uint8_t)((hapbeat::MAGIC >> 8) & 0xFF);
      if (magic_ok && rb[2] == hapbeat::VERSION && rb[3] == hapbeat::CMD_PONG) {
        _deviceIp = _udp.remoteIP();  // unicast stream target
        return true;
      }
    } else {
      delay(5);
    }
  }
  return false;
}

void Hapbeat::play(const char* eventId, float gain, const char* target) {
  uint8_t buf[256];
  sendPacket(buf, hapbeat::buildPlay(buf, sizeof(buf), nextSeq(), eventId, target, 0, gain));
}

void Hapbeat::stop(const char* eventId, const char* target) {
  uint8_t buf[256];
  sendPacket(buf, hapbeat::buildStop(buf, sizeof(buf), nextSeq(), eventId, target));
}

void Hapbeat::stopAll(const char* target) {
  uint8_t buf[128];
  sendPacket(buf, hapbeat::buildStopAll(buf, sizeof(buf), nextSeq(), target));
}

void Hapbeat::ping() {
  uint8_t buf[16];
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
    streamSend(hdr, beginLen);
    delay(1);
  }
  pumpSine();  // prime the ring (bounded to kSineMaxChunksPerPump per call)
}

void Hapbeat::pumpSine() {
  if (!_sineActive) return;
  int16_t pcm[kSineChunk];
  uint8_t pkt[hapbeat::HEADER_SIZE + 4 + kSineChunk * 2];
  // Top the device ring up to the lead target; self-paces (sends nothing once
  // full). Phase-continuous across calls. Bounded per call so the prime/catch-up
  // is spread over several invocations instead of one synchronous TX storm.
  for (int chunks = 0; chunks < kSineMaxChunksPerPump; ++chunks) {
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
    const bool ok = streamSend(pkt,
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
    streamSend(hdr, endLen);
    delay(1);
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
