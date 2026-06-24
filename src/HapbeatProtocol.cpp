// HapbeatProtocol.cpp — see HapbeatProtocol.h.
#include "HapbeatProtocol.h"

#include <string.h>

namespace {

// Allocation-free little-endian writer over a fixed buffer. Sets ok=false (and
// stops writing) on overflow, which the builders turn into a 0 return.
struct Writer {
  uint8_t* buf;
  size_t cap;
  size_t pos = 0;
  bool ok = true;

  Writer(uint8_t* b, size_t c) : buf(b), cap(c) {}

  void u8(uint8_t v) {
    if (pos + 1 > cap) { ok = false; return; }
    buf[pos++] = v;
  }
  void u16(uint16_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 0xFF);
  }
  void i64(int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) u8((uint8_t)((u >> (8 * i)) & 0xFF));
  }
  void f32(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    u32(bits);
  }
  void cstr(const char* s) {
    if (s) { while (*s) u8((uint8_t)*s++); }
    u8(0);  // null terminator
  }
  void raw(const uint8_t* d, size_t n) {
    if (pos + n > cap) { ok = false; return; }
    memcpy(buf + pos, d, n);
    pos += n;
  }
};

// Write the 8-byte header with a placeholder length (patched in finish()).
void header(Writer& w, uint8_t cmd, uint16_t seq) {
  w.u16(hapbeat::MAGIC);
  w.u8(hapbeat::VERSION);
  w.u8(cmd);
  w.u16(seq);
  w.u16(0);  // payload_length placeholder
}

// Patch payload_length, enforce the size cap, and return final length (0 = fail).
size_t finish(Writer& w, size_t sizeCap) {
  if (!w.ok || w.pos > sizeCap) return 0;
  size_t payloadLen = w.pos - hapbeat::HEADER_SIZE;
  w.buf[6] = (uint8_t)(payloadLen & 0xFF);
  w.buf[7] = (uint8_t)((payloadLen >> 8) & 0xFF);
  return w.pos;
}

}  // namespace

namespace hapbeat {

size_t buildPlay(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target, int64_t targetTimeUs, float gain) {
  Writer w(out, cap);
  header(w, CMD_PLAY, seq);
  w.cstr(eventId);
  w.cstr(target);
  w.i64(targetTimeUs);
  w.f32(gain);
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildStop(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target) {
  Writer w(out, cap);
  header(w, CMD_STOP, seq);
  w.cstr(eventId);
  w.cstr(target);
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildStopAll(uint8_t* out, size_t cap, uint16_t seq, const char* target) {
  Writer w(out, cap);
  header(w, CMD_STOP_ALL, seq);
  w.cstr(target);
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildPing(uint8_t* out, size_t cap, uint16_t seq, int64_t timestampUs) {
  Writer w(out, cap);
  header(w, CMD_PING, seq);
  w.i64(timestampUs);
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildConnectStatus(uint8_t* out, size_t cap, uint16_t seq, bool connected,
                          uint8_t group, const char* appName, const char* deviceName) {
  Writer w(out, cap);
  header(w, CMD_CONNECT_STATUS, seq);
  w.u8(connected ? 1 : 0);
  w.u8(group);
  w.cstr(appName);
  w.cstr(deviceName);
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildStreamBegin(uint8_t* out, size_t cap, uint16_t seq, uint16_t sampleRate,
                        uint8_t channels, uint8_t format, uint32_t totalSamples,
                        float gain, const char* target) {
  Writer w(out, cap);
  header(w, CMD_STREAM_BEGIN, seq);
  w.u16(sampleRate);
  w.u8(channels);
  w.u8(format);
  w.u32(totalSamples);
  w.f32(gain);
  if (target && target[0]) w.cstr(target);  // omit when broadcasting
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildStreamData(uint8_t* out, size_t cap, uint16_t seq, uint32_t offset,
                       const uint8_t* data, size_t dataLen) {
  Writer w(out, cap);
  header(w, CMD_STREAM_DATA, seq);
  w.u32(offset);
  w.raw(data, dataLen);
  return finish(w, MAX_STREAM_PACKET_SIZE);
}

size_t buildStreamEnd(uint8_t* out, size_t cap, uint16_t seq) {
  Writer w(out, cap);
  header(w, CMD_STREAM_END, seq);
  return finish(w, MAX_PACKET_SIZE);
}

}  // namespace hapbeat
