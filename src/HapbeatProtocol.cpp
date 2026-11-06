// HapbeatProtocol.cpp — see HapbeatProtocol.h.
#include "HapbeatProtocol.h"

#include <math.h>
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
  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) u8((uint8_t)((v >> (8 * i)) & 0xFF));
  }
  void i64(int64_t v) { u64((uint64_t)v); }
  void f32(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    u32(bits);
  }
  void cstr(const char* s) {
    if (s) { while (*s) u8((uint8_t)*s++); }
    u8(0);  // null terminator
  }
  // memmove: the stream mixer passes a source that already sits at the
  // destination inside the same packet buffer.
  void raw(const uint8_t* d, size_t n) {
    if (pos + n > cap) { ok = false; return; }
    memmove(buf + pos, d, n);
    pos += n;
  }
};

// Write the 8-byte header with a placeholder length (patched in finish()).
void header(Writer& w, uint8_t cmd, uint16_t seq,
            uint8_t version = hapbeat::VERSION) {
  w.u16(hapbeat::MAGIC);
  w.u8(version);
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

// Stream Gen 2 envelope; false (nothing written) if any field is 0.
bool envelope(Writer& w, uint64_t bootId, uint32_t ticket, uint32_t generation) {
  if (bootId == 0 || ticket == 0 || generation == 0) return false;
  w.u64(bootId);
  w.u32(ticket);
  w.u32(generation);
  return true;
}

uint16_t readU16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t readU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

uint64_t readU64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

// Validate the common header of a received datagram and return the end of its
// payload (header + payload_length), or 0.
size_t payloadEnd(const uint8_t* packet, size_t length, uint8_t command) {
  if (!packet || length < hapbeat::HEADER_SIZE) return 0;
  if (readU16(packet) != hapbeat::MAGIC || packet[2] != hapbeat::VERSION ||
      packet[3] != command)
    return 0;
  const size_t end = hapbeat::HEADER_SIZE + readU16(packet + 6);
  return end <= length ? end : 0;
}

// Skip one NUL-terminated string; false if it has no terminator before end.
bool skipString(const uint8_t* data, size_t end, size_t& pos) {
  while (pos < end && data[pos] != 0) ++pos;
  if (pos >= end) return false;
  ++pos;
  return true;
}

// Read one NUL-terminated string into out (truncated to capacity - 1).
// Returns false if it has no terminator; `fits` reports whether it fitted.
bool readString(const uint8_t* data, size_t end, size_t& pos, char* out,
                size_t capacity, bool& fits) {
  const size_t start = pos;
  if (!skipString(data, end, pos)) return false;
  const size_t length = pos - 1 - start;
  fits = length < capacity;
  const size_t copied = fits ? length : capacity - 1;
  memcpy(out, data + start, copied);
  out[copied] = '\0';
  return true;
}

}  // namespace

namespace hapbeat {

bool addressMatches(const char* target, const char* address) {
  if (!target || !target[0]) return true;
  if (!address || !address[0]) return false;

  const char* targetPart = target;
  const char* addressPart = address;
  while (true) {
    const char* targetEnd = targetPart;
    while (*targetEnd && *targetEnd != '/') ++targetEnd;
    const char* addressEnd = addressPart;
    while (*addressEnd && *addressEnd != '/') ++addressEnd;

    if (addressPart == addressEnd) return false;
    const size_t targetLen = (size_t)(targetEnd - targetPart);
    const size_t addressLen = (size_t)(addressEnd - addressPart);
    const bool wildcard = targetLen == 1 && targetPart[0] == '*';
    if (!wildcard &&
        (targetLen != addressLen || memcmp(targetPart, addressPart, targetLen) != 0)) {
      return false;
    }

    if (!*targetEnd) return true;
    if (!*addressEnd) return false;
    targetPart = targetEnd + 1;
    addressPart = addressEnd + 1;
  }
}

size_t buildPlay(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target, int64_t targetTimeUs, float gain, float pan) {
  // The device does not clamp gain (above 1 distorts, below 0 inverts): send
  // clamp(gain, 0, 1), and 0 for NaN / infinity (contracts kit-format.md).
  if (!isfinite(gain) || gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  if (pan < -1.0f) pan = -1.0f;
  if (pan > 1.0f) pan = 1.0f;
  Writer w(out, cap);
  header(w, CMD_PLAY, seq);
  w.cstr(eventId);
  w.cstr(target);
  w.i64(targetTimeUs);
  w.f32(gain);
  w.f32(pan);
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

size_t buildPingExtended(uint8_t* out, size_t cap, uint16_t seq,
                         int64_t timestampUs, uint64_t clientIncarnation) {
  if (clientIncarnation == 0) return 0;
  Writer w(out, cap);
  header(w, CMD_PING, seq);
  w.i64(timestampUs);
  w.u64(clientIncarnation);
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

size_t buildStreamBeginGen2(uint8_t* out, size_t cap, uint16_t seq,
                            uint64_t bootId, uint32_t ticket,
                            uint32_t generation, uint16_t sampleRate,
                            uint8_t channels, uint8_t format,
                            uint32_t totalSamples, float gain,
                            const char* target) {
  // The target is at most 64 bytes including its NUL (device-addressing.md).
  if (target && strlen(target) >= MAX_DEVICE_ADDRESS_LEN) return 0;
  Writer w(out, cap);
  header(w, CMD_STREAM_BEGIN, seq, STREAM_VERSION);
  if (!envelope(w, bootId, ticket, generation)) return 0;
  w.u16(sampleRate);
  w.u8(channels);
  w.u8(format);
  w.u32(totalSamples);
  w.f32(gain);
  if (target && target[0]) w.cstr(target);
  return finish(w, MAX_PACKET_SIZE);
}

size_t buildStreamDataGen2(uint8_t* out, size_t cap, uint16_t seq,
                           uint64_t bootId, uint32_t ticket,
                           uint32_t generation, uint32_t offset,
                           const uint8_t* data, size_t dataLen) {
  if (!data || dataLen == 0) return 0;
  Writer w(out, cap);
  header(w, CMD_STREAM_DATA, seq, STREAM_VERSION);
  if (!envelope(w, bootId, ticket, generation)) return 0;
  w.u32(offset);
  w.raw(data, dataLen);
  return finish(w, MAX_STREAM_PACKET_SIZE);
}

size_t buildStreamEndGen2(uint8_t* out, size_t cap, uint16_t seq,
                          uint64_t bootId, uint32_t ticket,
                          uint32_t generation) {
  Writer w(out, cap);
  header(w, CMD_STREAM_END, seq, STREAM_VERSION);
  if (!envelope(w, bootId, ticket, generation)) return 0;
  return finish(w, MAX_PACKET_SIZE);
}

bool parsePong(const uint8_t* packet, size_t length, PongInfo& out) {
  out = PongInfo();
  const size_t end = payloadEnd(packet, length, CMD_PONG);
  if (end < HEADER_SIZE + 16) return false;
  out.seq = readU16(packet + 4);
  out.timestampUs = (int64_t)readU64(packet + HEADER_SIZE);

  // device_name\0, address\0, firmware_version\0, volume_level, volume_wiper.
  // A PONG that ends at or before volume_wiper has no tail (Stream Gen 1).
  size_t pos = HEADER_SIZE + 16;
  if (!skipString(packet, end, pos)) return true;
  bool fits = false;
  if (!readString(packet, end, pos, out.address, sizeof(out.address), fits))
    return true;
  out.hasAddress = fits && out.address[0] != '\0';
  if (!fits) out.address[0] = '\0';
  if (!skipString(packet, end, pos)) return true;
  if (end - pos <= 2) return true;
  pos += 2;

  const uint8_t* rest = packet + pos;
  const size_t restLength = end - pos;
  if (restLength < PONG_TAIL_SIZE || memcmp(rest, "HBS2", 4) != 0 ||
      rest[4] != 1) {
    out.tail = PongTail::Ignored;
    return true;
  }
  // Read the first 32 bytes; ignore the reserved field, flag bits other than
  // 0-2 and any bytes after the tail (a later major may append them).
  out.tail = PongTail::Gen2;
  out.flags = rest[5];
  out.echoedIncarnation = readU64(rest + 8);
  out.bootId = readU64(rest + 16);
  out.ticket = readU32(rest + 24);
  out.highWaterTicket = readU32(rest + 28);
  out.leaseValid = (out.flags & 0x01) && out.bootId != 0 && out.ticket != 0;
  out.superseded = (out.flags & 0x02) != 0;
  out.reissued = (out.flags & 0x04) != 0;
  return true;
}

bool parseError(const uint8_t* packet, size_t length, ErrorInfo& out) {
  out = ErrorInfo();
  const size_t end = payloadEnd(packet, length, CMD_ERROR);
  if (end < HEADER_SIZE + 2) return false;
  out.code = readU16(packet + HEADER_SIZE);
  size_t pos = HEADER_SIZE + 2;
  bool fits = false;
  if (!readString(packet, end, pos, out.message, sizeof(out.message), fits))
    return false;
  // ref_command, ref_seq, detail_length and detail were appended in 1.x; 0.x
  // firmware sends only the code and the message.
  if (end - pos < 4) return true;
  out.hasReference = true;
  out.refCommand = packet[pos];
  out.refSeq = readU16(packet + pos + 1);
  const size_t declared = packet[pos + 3];
  pos += 4;
  const size_t available = end - pos < declared ? end - pos : declared;
  out.detailLength = (uint8_t)available;
  memcpy(out.detail, packet + pos,
         available < sizeof(out.detail) ? available : sizeof(out.detail));
  return true;
}

const char* errorName(uint16_t code) {
  switch (code) {
    case ERR_UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
    case ERR_UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
    case ERR_MALFORMED: return "MALFORMED";
    case ERR_STREAM_GEN_UNSUPPORTED: return "STREAM_GEN_UNSUPPORTED";
    case ERR_STREAM_LEASE_INVALID: return "STREAM_LEASE_INVALID";
    case ERR_STREAM_SUPERSEDED: return "STREAM_SUPERSEDED";
    case ERR_STREAM_NOT_STARTED: return "STREAM_NOT_STARTED";
    case ERR_STREAM_TIMED_OUT: return "STREAM_TIMED_OUT";
    case ERR_STREAM_STOPPED: return "STREAM_STOPPED";
    case ERR_STREAM_ENDED: return "STREAM_ENDED";
    case ERR_STREAM_TARGET_MISMATCH: return "STREAM_TARGET_MISMATCH";
    default: return "UNKNOWN";
  }
}

}  // namespace hapbeat
