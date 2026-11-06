// HapbeatProtocol.h — portable Hapbeat Layer 1 (SDK -> device) packet builders.
//
// Framework-agnostic C++ (no Arduino / no dynamic allocation). Builds packets
// into a caller-provided buffer so the same core can back the Arduino transport,
// an ESP-IDF transport, or an ESP-NOW transport later.
//
// Byte-for-byte compatible with hapbeat-contracts/specs/message-format.md and
// the reference impls (HapbeatProtocol.cs, python-sdk protocol.py). Every
// multi-byte field is little-endian. PCM16 stream data must be little-endian
// (native on ESP32 / all common Arduino MCUs).
#ifndef HAPBEAT_PROTOCOL_H
#define HAPBEAT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace hapbeat {

constexpr uint16_t MAGIC = 0x4842;  // "HB"
constexpr uint8_t VERSION = 0x01;         // every non-stream command
constexpr uint8_t STREAM_VERSION = 0x02;  // Stream Gen 2 BEGIN / DATA / END
constexpr size_t HEADER_SIZE = 8;
constexpr size_t STREAM_ENVELOPE_SIZE = 16;  // boot_id u64 + ticket u32 + generation u32
constexpr size_t PONG_TAIL_SIZE = 32;        // the HBS2 lease tail read by 1.x
constexpr size_t MAX_PACKET_SIZE = 512;          // command packets
constexpr size_t MAX_STREAM_PACKET_SIZE = 1472;  // 1500 MTU - 20 IP - 8 UDP
constexpr size_t MAX_APP_NAME_LEN = 16;          // device OLED grid width
constexpr size_t MAX_DEVICE_ADDRESS_LEN = 64;    // including no null terminator

constexpr uint8_t CMD_PLAY = 0x01;
constexpr uint8_t CMD_STOP = 0x02;
constexpr uint8_t CMD_STOP_ALL = 0x03;
constexpr uint8_t CMD_PING = 0x10;
constexpr uint8_t CMD_CONNECT_STATUS = 0x20;
constexpr uint8_t CMD_STREAM_BEGIN = 0x30;
constexpr uint8_t CMD_STREAM_DATA = 0x31;
constexpr uint8_t CMD_STREAM_END = 0x32;

// Response types (device -> SDK)
constexpr uint8_t CMD_PONG = 0x11;
constexpr uint8_t CMD_ERROR = 0xff;

// ERROR (0xFF) codes, message-format.md 0xFF.
constexpr uint16_t ERR_UNSUPPORTED_VERSION = 0x0001;
constexpr uint16_t ERR_UNKNOWN_COMMAND = 0x0002;
constexpr uint16_t ERR_MALFORMED = 0x0003;
constexpr uint16_t ERR_STREAM_GEN_UNSUPPORTED = 0x0010;
constexpr uint16_t ERR_STREAM_LEASE_INVALID = 0x0011;
constexpr uint16_t ERR_STREAM_SUPERSEDED = 0x0012;
constexpr uint16_t ERR_STREAM_NOT_STARTED = 0x0013;
constexpr uint16_t ERR_STREAM_TIMED_OUT = 0x0014;
constexpr uint16_t ERR_STREAM_STOPPED = 0x0015;
constexpr uint16_t ERR_STREAM_ENDED = 0x0016;
constexpr uint16_t ERR_STREAM_TARGET_MISMATCH = 0x0017;

constexpr uint8_t FORMAT_PCM16 = 0;
constexpr uint8_t FORMAT_IMA_ADPCM = 1;

// Positional target matching from device-addressing.md. `*` matches one whole
// segment; a target with fewer segments is a prefix match. This helper is
// allocation-free so transports can use it on embedded targets.
bool addressMatches(const char* target, const char* address);

// Each builder writes one complete packet into `out` (capacity `cap`) and
// returns the number of bytes written, or 0 if it would not fit or exceeds the
// protocol size cap. `target`/`eventId` may be nullptr (treated as "").

// PLAY (0x01): event_id\0 + target\0 + target_time(i64) + gain(f32) + pan(f32).
// target "" = broadcast. pan is the left/right balance (-1 = left only, 0 =
// centre, +1 = right only) and is clamped to [-1, 1] here, so every sender goes
// out spec-conformant; senders must always write it (a receiver that predates
// the field just ignores the trailing 4 bytes). gain is written as
// clamp(gain, 0, 1), and NaN / infinity as 0: the device does not clamp it.
size_t buildPlay(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target, int64_t targetTimeUs, float gain,
                 float pan = 0.0f);

// STOP (0x02): event_id\0 + target\0.
size_t buildStop(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target);

// STOP_ALL (0x03): target\0.
size_t buildStopAll(uint8_t* out, size_t cap, uint16_t seq, const char* target);

// PING (0x10): timestamp(i64, microseconds). Discovery only, no lease.
size_t buildPing(uint8_t* out, size_t cap, uint16_t seq, int64_t timestampUs);

// Extended PING (0x10): timestamp(i64) + client_incarnation(u64), a Stream
// Gen 2 lease request (stream-session.md). Send it by unicast only. Returns 0
// for incarnation 0, which senders must not send.
size_t buildPingExtended(uint8_t* out, size_t cap, uint16_t seq,
                         int64_t timestampUs, uint64_t clientIncarnation);

// CONNECT_STATUS (0x20): connected(u8) + group(u8) + app_name\0 + device_name\0.
size_t buildConnectStatus(uint8_t* out, size_t cap, uint16_t seq, bool connected,
                          uint8_t group, const char* appName, const char* deviceName);

// STREAM_BEGIN (0x30): sample_rate(u16) channels(u8) format(u8) total_samples(u32)
//   gain(f32) [+ target\0]. target "" = broadcast (omitted from the wire).
size_t buildStreamBegin(uint8_t* out, size_t cap, uint16_t seq, uint16_t sampleRate,
                        uint8_t channels, uint8_t format, uint32_t totalSamples,
                        float gain, const char* target);

// STREAM_DATA (0x31): offset(u32) + data. Uses the larger MTU-safe cap.
size_t buildStreamData(uint8_t* out, size_t cap, uint16_t seq, uint32_t offset,
                       const uint8_t* data, size_t dataLen);

// STREAM_END (0x32): no payload.
size_t buildStreamEnd(uint8_t* out, size_t cap, uint16_t seq);

// Stream Gen 2 (firmware 0.5.0+, stream-session.md): header version 2 and the
// 16-byte envelope (device_boot_id, lease_ticket, generation) first in every
// payload. Each builder returns 0 when bootId, ticket or generation is 0.
// BEGIN: envelope + the 12-byte Gen 1 body [+ target\0, at most 63 chars].
size_t buildStreamBeginGen2(uint8_t* out, size_t cap, uint16_t seq,
                            uint64_t bootId, uint32_t ticket,
                            uint32_t generation, uint16_t sampleRate,
                            uint8_t channels, uint8_t format,
                            uint32_t totalSamples, float gain,
                            const char* target);

// DATA: envelope + offset(u32) + at least one data byte. `data` may alias the
// destination range inside `out` (it is moved, not copied).
size_t buildStreamDataGen2(uint8_t* out, size_t cap, uint16_t seq,
                           uint64_t bootId, uint32_t ticket,
                           uint32_t generation, uint32_t offset,
                           const uint8_t* data, size_t dataLen);

// END: the envelope only.
size_t buildStreamEndGen2(uint8_t* out, size_t cap, uint16_t seq,
                          uint64_t bootId, uint32_t ticket,
                          uint32_t generation);

// How a PONG's bytes after volume_wiper classify the device (stream-session.md
// Receiver classification, 1.x rules). Only a matched direct reply to the
// sender's own extended PING may change the class.
enum class PongTail : uint8_t {
  Absent,   // nothing after volume_wiper: Stream Gen 1 firmware
  Gen2,     // >= 32 bytes, "HBS2", extension_version 1 (first 32 bytes read)
  Ignored,  // anything else: no class or lease change
};

struct PongInfo {
  uint16_t seq = 0;
  int64_t timestampUs = 0;  // echoed PING timestamp; 0 = unsolicited
  bool hasAddress = false;
  char address[MAX_DEVICE_ADDRESS_LEN + 1] = {0};
  PongTail tail = PongTail::Absent;
  // Gen2 only:
  uint8_t flags = 0;
  uint64_t echoedIncarnation = 0;
  uint64_t bootId = 0;
  uint32_t ticket = 0;
  uint32_t highWaterTicket = 0;
  bool leaseValid = false;  // flag bit 0 && bootId != 0 && ticket != 0
  bool superseded = false;  // flag bit 1
  bool reissued = false;    // flag bit 2
};

// Parse a whole PONG datagram (header version 1, command 0x11). The payload is
// bounded by payload_length. Returns false for anything that is not a PONG.
bool parsePong(const uint8_t* packet, size_t length, PongInfo& out);

struct ErrorInfo {
  uint16_t code = 0;
  char message[64] = {0};  // truncated to 63 characters
  bool hasReference = false;  // ref_command / ref_seq / detail present
  uint8_t refCommand = 0;
  uint16_t refSeq = 0;
  uint8_t detailLength = 0;  // detail bytes present in the datagram
  uint8_t detail[STREAM_ENVELOPE_SIZE] = {0};  // first 16 detail bytes
};

// Parse a whole ERROR datagram (header version 1, command 0xFF).
bool parseError(const uint8_t* packet, size_t length, ErrorInfo& out);

// The name of an ERROR code ("STREAM_SUPERSEDED"), or "UNKNOWN".
const char* errorName(uint16_t code);

}  // namespace hapbeat

#endif  // HAPBEAT_PROTOCOL_H
