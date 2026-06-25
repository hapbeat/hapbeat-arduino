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
constexpr uint8_t VERSION = 0x01;
constexpr size_t HEADER_SIZE = 8;
constexpr size_t MAX_PACKET_SIZE = 512;          // command packets
constexpr size_t MAX_STREAM_PACKET_SIZE = 1472;  // 1500 MTU - 20 IP - 8 UDP
constexpr size_t MAX_APP_NAME_LEN = 16;          // device OLED grid width

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

constexpr uint8_t FORMAT_PCM16 = 0;
constexpr uint8_t FORMAT_IMA_ADPCM = 1;

// Each builder writes one complete packet into `out` (capacity `cap`) and
// returns the number of bytes written, or 0 if it would not fit or exceeds the
// protocol size cap. `target`/`eventId` may be nullptr (treated as "").

// PLAY (0x01): event_id\0 + target\0 + target_time(i64) + gain(f32). target "" = broadcast.
size_t buildPlay(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target, int64_t targetTimeUs, float gain);

// STOP (0x02): event_id\0 + target\0.
size_t buildStop(uint8_t* out, size_t cap, uint16_t seq, const char* eventId,
                 const char* target);

// STOP_ALL (0x03): target\0.
size_t buildStopAll(uint8_t* out, size_t cap, uint16_t seq, const char* target);

// PING (0x10): timestamp(i64, microseconds).
size_t buildPing(uint8_t* out, size_t cap, uint16_t seq, int64_t timestampUs);

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

}  // namespace hapbeat

#endif  // HAPBEAT_PROTOCOL_H
