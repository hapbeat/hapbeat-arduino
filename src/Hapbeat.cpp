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
  _udp.begin(0);  // ephemeral local port for sending; PONG receive is level-2
  _ready = true;
  if (_appName[0]) connectStatus(true);
  return true;
}

void Hapbeat::sendPacket(const uint8_t* buf, size_t len) {
  if (!_ready || len == 0) return;
  _udp.beginPacket(IPAddress(255, 255, 255, 255), _port);
  _udp.write(buf, len);
  _udp.endPacket();
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

void Hapbeat::playSine(float freqHz, float intensity, uint32_t durationMs, const char* target) {
  if (!_ready) return;
  if (intensity < 0.0f) intensity = 0.0f;
  if (intensity > 1.0f) intensity = 1.0f;

  const uint16_t rate = 16000;  // device clip-stream rate (16 kHz PCM16)
  const uint16_t CHUNK = 256;   // samples per packet (16 ms @ 16 kHz)
  const uint32_t totalSamples = (uint32_t)((uint64_t)rate * durationMs / 1000);

  // STREAM_BEGIN: mono PCM16. intensity rides the gain field (device-mixed).
  uint8_t hdr[64];
  sendPacket(hdr, hapbeat::buildStreamBegin(hdr, sizeof(hdr), nextSeq(), rate, 1,
                                            hapbeat::FORMAT_PCM16, totalSamples,
                                            intensity, target));

  int16_t pcm[CHUNK];
  uint8_t pkt[hapbeat::HEADER_SIZE + 4 + CHUNK * 2];
  float phase = 0.0f;
  const float dphase = 2.0f * (float)PI * freqHz / (float)rate;
  const int16_t amp = (int16_t)(0.8f * 32767.0f);  // 0.8 headroom; loudness via gain
  uint32_t sent = 0, byteOffset = 0;
  const uint32_t startMs = millis();

  while (sent < totalSamples) {
    const uint16_t cnt =
        (totalSamples - sent < CHUNK) ? (uint16_t)(totalSamples - sent) : CHUNK;
    for (uint16_t i = 0; i < cnt; ++i) {
      pcm[i] = (int16_t)(amp * sinf(phase));
      phase += dphase;
      if (phase >= 2.0f * (float)PI) phase -= 2.0f * (float)PI;
    }
    // PCM16 little-endian: native byte order on ESP32 / common Arduino MCUs.
    sendPacket(pkt, hapbeat::buildStreamData(pkt, sizeof(pkt), nextSeq(), byteOffset,
                                             (const uint8_t*)pcm, (size_t)cnt * 2));
    sent += cnt;
    byteOffset += (uint32_t)cnt * 2;
    // Pace ~real-time so the device's ~256 ms ring buffer never overflows,
    // keeping a tiny lead so it never underruns.
    const uint32_t dueMs = startMs + (uint32_t)((uint64_t)sent * 1000 / rate);
    const int32_t wait = (int32_t)(dueMs - millis());
    if (wait > 1) delay((uint32_t)(wait - 1));
  }

  sendPacket(hdr, hapbeat::buildStreamEnd(hdr, sizeof(hdr), nextSeq()));
}
