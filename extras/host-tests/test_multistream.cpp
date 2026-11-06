#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <type_traits>
#include <utility>

#include <vector>

// The shared JSON fixtures are not directly consumable by this Arduino fake
// UDP harness. These deterministic cases mirror the endpoint, lifecycle,
// pacing, and mixing assertions of sdk-multi-stream.md sections 4-7, and the
// Stream Gen 2 cases of stream-session.md; fixture packets are copied as hex.
#define private public
#include "Hapbeat.h"
#undef private

uint32_t hapbeat_host_now_ms = 0;
uint32_t hapbeat_host_random_state = 1;
std::vector<WiFiUDP::Sent> WiFiUDP::sent;
std::deque<WiFiUDP::Inbound> WiFiUDP::inbound;
int WiFiUDP::failEndPackets = 0;

static_assert(!std::is_copy_constructible<Hapbeat::Playback>::value,
              "Playback must be move-only");
static_assert(!std::is_copy_assignable<Hapbeat::Playback>::value,
              "Playback must not be copy-assignable");
static_assert(std::is_move_constructible<Hapbeat::Playback>::value,
              "Playback must be move-constructible");
static_assert(std::is_move_assignable<Hapbeat::Playback>::value,
              "Playback must be move-assignable");
static_assert(!std::is_copy_constructible<Hapbeat>::value,
              "Hapbeat must not be copy-constructible");
static_assert(!std::is_copy_assignable<Hapbeat>::value,
              "Hapbeat must not be copy-assignable");
static_assert(!std::is_move_constructible<Hapbeat>::value,
              "Hapbeat must not be move-constructible");
static_assert(!std::is_move_assignable<Hapbeat>::value,
              "Hapbeat must not be move-assignable");

static std::vector<uint8_t> pong(const char* address) {
  std::vector<uint8_t> packet(8 + 16, 0);
  packet[0] = 0x42;
  packet[1] = 0x48;
  packet[2] = hapbeat::VERSION;
  packet[3] = hapbeat::CMD_PONG;
  packet[4] = 1;
  const char name[] = "fake";
  packet.insert(packet.end(), name, name + sizeof(name));
  packet.insert(packet.end(), address, address + strlen(address) + 1);
  const char fw[] = "test";
  packet.insert(packet.end(), fw, fw + sizeof(fw));
  const uint16_t payload = (uint16_t)(packet.size() - hapbeat::HEADER_SIZE);
  packet[6] = (uint8_t)(payload & 0xff);
  packet[7] = (uint8_t)(payload >> 8);
  return packet;
}

static int64_t gServerTime = 0;

// A direct PONG reply (device-shaped: firmware_version, volume_level and
// volume_wiper, then `rest` = the bytes after volume_wiper).
static std::vector<uint8_t> pongReply(const char* address, uint16_t seq,
                                      int64_t timestampUs,
                                      const std::vector<uint8_t>& rest) {
  std::vector<uint8_t> packet = {0x42, 0x48, hapbeat::VERSION, hapbeat::CMD_PONG,
                                 (uint8_t)(seq & 0xff), (uint8_t)(seq >> 8), 0, 0};
  const uint64_t ts = (uint64_t)timestampUs;
  for (int i = 0; i < 8; ++i) packet.push_back((uint8_t)(ts >> (8 * i)));
  const uint64_t server = (uint64_t)++gServerTime;
  for (int i = 0; i < 8; ++i) packet.push_back((uint8_t)(server >> (8 * i)));
  const char name[] = "fake";
  packet.insert(packet.end(), name, name + sizeof(name));
  packet.insert(packet.end(), address, address + strlen(address) + 1);
  const char fw[] = "1.0.0";
  packet.insert(packet.end(), fw, fw + sizeof(fw));
  packet.push_back(0x08);  // volume_level
  packet.push_back(0xc8);  // volume_wiper
  packet.insert(packet.end(), rest.begin(), rest.end());
  const uint16_t payload = (uint16_t)(packet.size() - hapbeat::HEADER_SIZE);
  packet[6] = (uint8_t)(payload & 0xff);
  packet[7] = (uint8_t)(payload >> 8);
  return packet;
}

static void put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(value >> (8 * i)));
}

static void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(value >> (8 * i)));
}

// The 32-byte HBS2 lease tail (stream-session.md Discovery lease).
static std::vector<uint8_t> leaseTail(uint8_t flags, uint64_t incarnation,
                                      uint64_t bootId, uint32_t ticket,
                                      uint32_t highWater) {
  std::vector<uint8_t> tail = {'H', 'B', 'S', '2', 1, flags, 0, 0};
  put64(tail, incarnation);
  put64(tail, bootId);
  put32(tail, ticket);
  put32(tail, highWater);
  return tail;
}

// Place an outstanding extended PING as if the hub had sent it.
static void addPending(Hapbeat& hb, uint16_t seq, int64_t timestampUs,
                       IPAddress ip, uint16_t port, uint64_t incarnation) {
  uint8_t slot = 0;
  for (uint8_t i = 0; i < Hapbeat::MAX_PENDING_PINGS; ++i)
    if (!hb._pendingPings[i].used) {
      slot = i;
      break;
    }
  Hapbeat::PendingPing& pending = hb._pendingPings[slot];
  pending.used = true;
  pending.seq = seq;
  pending.timestampUs = timestampUs;
  pending.ip = ip;
  pending.port = port;
  pending.incarnation = incarnation;
  pending.sentMs = hb.extendedMillis();
}

// A matched reply without a tail: the device runs Stream Gen 1 firmware.
static void classifyGen1(Hapbeat& hb, IPAddress ip, uint16_t port,
                         const char* address) {
  const uint16_t seq = hb.nextSeq();
  const int64_t timestamp = hb.nextPingTimestamp();
  addPending(hb, seq, timestamp, ip, port, hb._incarnation);
  WiFiUDP::inject(ip, port, pongReply(address, seq, timestamp, {}));
  hb.poll();
}

// A matched reply with an HBS2 tail: Stream Gen 2 under (bootId, ticket).
static void classifyGen2(Hapbeat& hb, IPAddress ip, uint16_t port,
                         const char* address, uint64_t bootId,
                         uint32_t ticket, uint8_t flags = 0x01,
                         uint32_t highWater = 0) {
  const uint16_t seq = hb.nextSeq();
  const int64_t timestamp = hb.nextPingTimestamp();
  addPending(hb, seq, timestamp, ip, port, hb._incarnation);
  WiFiUDP::inject(ip, port,
                  pongReply(address, seq, timestamp,
                            leaseTail(flags, hb._incarnation, bootId, ticket,
                                      highWater ? highWater : ticket)));
  hb.poll();
}

static std::vector<uint8_t> fromHex(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    unsigned value = 0;
    sscanf(hex + i, "%2x", &value);
    out.push_back((uint8_t)value);
  }
  return out;
}

static std::vector<std::string> gWarnings;
static void recordWarning(const char* message) { gWarnings.push_back(message); }

static uint16_t read16(const std::vector<uint8_t>& data, size_t offset) {
  return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

static uint32_t read32(const std::vector<uint8_t>& data, size_t offset) {
  return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
         ((uint32_t)data[offset + 2] << 16) |
         ((uint32_t)data[offset + 3] << 24);
}

static bool isStream(const WiFiUDP::Sent& sent, uint8_t command) {
  return sent.data.size() >= hapbeat::HEADER_SIZE && sent.data[3] == command;
}

static size_t countStream(uint8_t command) {
  size_t count = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isStream(sent, command)) ++count;
  return count;
}

static size_t countStreamTo(uint8_t command, IPAddress ip) {
  size_t count = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == ip && isStream(sent, command)) ++count;
  return count;
}

static void test_routing_and_mixing() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  classifyGen1(hb, IPAddress(192, 0, 2, 10), 7700, "player_1/left/group_1");
  classifyGen1(hb, IPAddress(192, 0, 2, 11), 7701, "player_1/right/group_1");
  assert(hb.deviceCount() == 2);

  Hapbeat::Playback left = hb.playSineSource(
      4000.0f, 1.0f, 0, "player_1/left", -1.0f, true);
  Hapbeat::Playback both = hb.playSineSource(
      4000.0f, 1.0f, 0, "player_1/*", -1.0f, true);
  assert(left.valid() && both.valid());
  assert(left.status() == HapbeatPlaybackStatus::Active);
  assert(both.status() == HapbeatPlaybackStatus::Active);
  size_t beginLeft = 0;
  size_t beginRight = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (!isStream(sent, hapbeat::CMD_STREAM_BEGIN)) continue;
    if (sent.ip == IPAddress(192, 0, 2, 10)) ++beginLeft;
    if (sent.ip == IPAddress(192, 0, 2, 11)) {
      ++beginRight;
      assert(sent.port == 7701);
    }
  }
  assert(beginLeft == 1 && beginRight == 1);
  WiFiUDP::sent.clear();
  hb.setBroadcastOnly(true);  // stream routing must ignore this setting
  hb.tick();

  bool sawLeft = false;
  bool sawRight = false;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    assert(!((uint32_t)sent.ip == 0xffffffffu));
    if (!isStream(sent, hapbeat::CMD_STREAM_DATA)) continue;
    if ((uint32_t)sent.ip == (uint32_t)IPAddress(192, 0, 2, 10)) sawLeft = true;
    if ((uint32_t)sent.ip == (uint32_t)IPAddress(192, 0, 2, 11)) {
      sawRight = true;
      assert(sent.port == 7701);
    }
  }
  assert(sawLeft && sawRight);

  // Two sources on the left endpoint are mixed and saturate once, after pan.
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (sent.ip != IPAddress(192, 0, 2, 10) ||
        !isStream(sent, hapbeat::CMD_STREAM_DATA)) continue;
    assert(read16(sent.data, 12 + 4) == 32767);
    assert(read16(sent.data, 12 + 4 + 2) == 0);
    break;
  }
  left.stop();
  assert(left.isStopped());
  assert(both.isActive());
  both.stop();
  hb.tick();
  assert(both.isStopped());
}

static void test_same_address_different_ip_are_distinct() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress first(192, 0, 2, 40);
  const IPAddress second(192, 0, 2, 41);
  classifyGen1(hb, first, 7700, "player_4/chest/group_1");
  classifyGen1(hb, second, 7701, "player_4/chest/group_1");
  assert(hb.deviceCount() == 2);

  Hapbeat::Playback playback =
      hb.playSineSource(160.0f, 0.5f, 0, "player_4/chest", 0.0f, true);
  assert(playback.valid() && playback.status() == HapbeatPlaybackStatus::Active);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 2);
  hb.tick();
  assert(countStreamTo(hapbeat::CMD_STREAM_DATA, first) > 0);
  assert(countStreamTo(hapbeat::CMD_STREAM_DATA, second) > 0);
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isStream(sent, hapbeat::CMD_STREAM_DATA) ||
        isStream(sent, hapbeat::CMD_STREAM_BEGIN))
      assert((uint32_t)sent.ip != 0xffffffffu);
}

static void test_deferred_late_pong_and_cooldown() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  assert(hapbeat::addressMatches("player_2/chest",
                                 "player_2/chest/group_1"));
  Hapbeat hb;
  assert(hb.begin());
  hb.setDeviceIp(IPAddress(192, 0, 2, 20));
  Hapbeat::Playback pinned = hb.playSineSource(
      100.0f, 1.0f, 0, "player_2/chest", 0.0f, true);
  assert(pinned.valid() && pinned.status() == HapbeatPlaybackStatus::Deferred);
  hb.tick();
  assert(WiFiUDP::sent.empty());
  pinned.stop();
  Hapbeat::Playback deferred = hb.playSineSource(
      100.0f, 1.0f, 1000, "player_2/chest", 0.0f, false);
  assert(deferred.valid() && deferred.status() == HapbeatPlaybackStatus::Deferred);
  hb.tick();
  assert(WiFiUDP::sent.empty());

  // An unsolicited PONG resolves the endpoint but not its stream class: the
  // source waits for the matched reply to the unicast lease PING.
  WiFiUDP::inject(IPAddress(192, 0, 2, 20), 7700,
                  pong("player_2/chest/group_1"));
  hb.tick();
  assert(hb.deviceCount() == 1);
  assert(deferred.status() == HapbeatPlaybackStatus::Deferred);
  assert(deferred.deferredReason() == HapbeatDeferredReason::UnknownClass);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 0);
  assert(WiFiUDP::sent.size() == 1);
  const WiFiUDP::Sent leasePing = WiFiUDP::sent[0];
  assert(leasePing.ip == IPAddress(192, 0, 2, 20));
  assert(leasePing.data.size() == hapbeat::HEADER_SIZE + 16);
  WiFiUDP::sent.clear();
  WiFiUDP::inject(IPAddress(192, 0, 2, 20), 7700,
                  pongReply("player_2/chest/group_1", read16(leasePing.data, 4),
                            (int64_t)(read32(leasePing.data, 8) |
                                      ((uint64_t)read32(leasePing.data, 12) << 32)),
                            {}));
  hb.tick();
  assert(hb.streamClass(IPAddress(192, 0, 2, 20)) == HapbeatStreamClass::Gen1);
  assert(deferred.status() == HapbeatPlaybackStatus::Active);
  assert(!WiFiUDP::sent.empty());
  assert(WiFiUDP::sent[0].data[3] == hapbeat::CMD_STREAM_BEGIN);
  assert(WiFiUDP::sent[0].ip == IPAddress(192, 0, 2, 20));
  assert(WiFiUDP::sent[0].port == 7700);
  assert(read16(WiFiUDP::sent[0].data, 8) == 16000);
  assert(WiFiUDP::sent[0].data[10] == 2);
  assert(WiFiUDP::sent[0].data[11] == hapbeat::FORMAT_PCM16);
  assert(read32(WiFiUDP::sent[0].data, 12) == 0);
  assert(read32(WiFiUDP::sent[0].data, 16) == 0x3f800000u);
  const char expectedTarget[] = "player_2/chest/group_1";
  for (size_t i = 0; i < sizeof(expectedTarget); ++i)
    assert(WiFiUDP::sent[0].data[20 + i] == (uint8_t)expectedTarget[i]);
  assert(read16(WiFiUDP::sent[1].data, 8) == 0);

  deferred.stop();
  hb.tick();
  hapbeat_host_now_ms += 310;
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_END) == 1);

  // Waiting out the Stream Gen 1 guard is Active, not Deferred
  // (sdk-multi-stream.md section 3).
  Hapbeat::Playback cooled = hb.playSineSource(
      100.0f, 1.0f, 0, "player_2/chest", 0.0f, true);
  assert(cooled.valid() && cooled.status() == HapbeatPlaybackStatus::Active);
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 1);
  hapbeat_host_now_ms += 310;
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 2);
}

static void test_begin_retry_and_source_pool_exhaustion() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  classifyGen1(hb, IPAddress(192, 0, 2, 50), 7700, "player_5/chest/group_1");
  WiFiUDP::failEndPackets = 1;
  Hapbeat::Playback playback = hb.playSineSource(
      100.0f, 1.0f, 0, "player_5/chest", 0.0f, true);
  assert(playback.valid() && playback.status() == HapbeatPlaybackStatus::Deferred);
  assert(playback.deferredReason() == HapbeatDeferredReason::NoResolvedEndpoint);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 1);
  assert(countStream(hapbeat::CMD_STREAM_DATA) == 0);
  hb.tick();
  assert(playback.status() == HapbeatPlaybackStatus::Active);
  assert(playback.deferredReason() == HapbeatDeferredReason::None);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 2);
  assert(countStream(hapbeat::CMD_STREAM_DATA) > 0);

  // Playback identity is a generation token, not the reusable source slot.
  Hapbeat generationHub;
  assert(generationHub.begin());
  classifyGen1(generationHub, IPAddress(192, 0, 2, 51), 7700,
               "player_5/generation/group_1");
  Hapbeat::Playback old = generationHub.playSineSource(
      100.0f, 0.25f, 0, "player_5/generation", 0.0f, true);
  assert(old.valid() && old.isActive());
  const uint16_t oldId = old.id();
  old.stop();
  Hapbeat::Playback replacement = generationHub.playSineSource(
      100.0f, 0.75f, 0, "player_5/generation", 0.0f, true);
  assert(old.valid() && replacement.valid());
  assert(old.id() == oldId && old.id() != replacement.id());
  assert(old.status() == HapbeatPlaybackStatus::Stopped);
  old.setGain(0.1f);
  old.setPan(-1.0f);
  old.setLoop(false);
  old.stop();
  assert(replacement.status() == HapbeatPlaybackStatus::Active);
  assert(replacement.gain() == 0.75f);
  assert(replacement.pan() == 0.0f);
  assert(replacement.loop());
  assert(generationHub._sources[0].gain == 0.75f);
  assert(generationHub._sources[0].pan == 0.0f);
  assert(generationHub._sources[0].loop);

  // Getters read the production source state, not a stale handle cache.
  generationHub._sources[0].gain = 0.6f;
  generationHub._sources[0].pan = -0.25f;
  generationHub._sources[0].loop = false;
  assert(replacement.gain() == 0.6f);
  assert(replacement.pan() == -0.25f);
  assert(!replacement.loop());

  // Moving transfers the generation token and invalidates the old value.
  Hapbeat::Playback moved(std::move(replacement));
  assert(!replacement.valid());
  assert(moved.valid());
  replacement.setGain(0.1f);
  replacement.setPan(1.0f);
  replacement.setLoop(true);
  assert(moved.gain() == 0.6f);
  assert(moved.pan() == -0.25f);
  assert(!moved.loop());
  Hapbeat::Playback assigned;
  assigned = std::move(moved);
  assert(!moved.valid());
  assert(assigned.valid());
  assigned.setGain(0.4f);
  assigned.setPan(0.25f);
  assigned.setLoop(true);
  assert(assigned.gain() == 0.4f);
  assert(assigned.pan() == 0.25f);
  assert(assigned.loop());
  assert(generationHub._sources[0].gain == 0.4f);
  assert(generationHub._sources[0].pan == 0.25f);
  assert(generationHub._sources[0].loop);
  moved.setGain(0.9f);
  moved.setPan(-1.0f);
  moved.setLoop(false);
  assert(assigned.gain() == 0.4f);
  assert(assigned.pan() == 0.25f);
  assert(assigned.loop());

  // Fixed source-pool exhaustion is reported without heap allocation.
  Hapbeat poolHub;
  assert(poolHub.begin());
  Hapbeat::Playback handles[Hapbeat::MAX_STREAM_SOURCES];
  for (uint8_t i = 0; i < Hapbeat::MAX_STREAM_SOURCES; ++i) {
    handles[i] = poolHub.playSineSource(100.0f, 1.0f, 0, "unresolved", 0.0f,
                                        true);
    assert(handles[i].valid());
  }
  Hapbeat::Playback exhausted =
      poolHub.playSineSource(100.0f, 1.0f, 0, "unresolved", 0.0f, true);
  assert(!exhausted.valid());
  for (uint8_t i = 0; i < Hapbeat::MAX_STREAM_SOURCES; ++i)
    handles[i].stop();

  // Value handles do not occupy a registry; repeated source-slot reuse stays
  // allocation-free and remains safe for every retired handle.
  for (uint16_t i = 0; i < 400; ++i) {
    Hapbeat::Playback sequential = poolHub.playSineSource(
        100.0f, 1.0f, 0, "unresolved", 0.0f, true);
    assert(sequential.valid());
    sequential.stop();
  }
}

static void test_route_migration_keeps_session() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress route(192, 0, 2, 30);
  classifyGen1(hb, route, 7700, "player_3/chest/group_1");
  Hapbeat::Playback playback = hb.playSineSource(
      100.0f, 1.0f, 0, "player_3/chest", 0.0f, true);
  hb.tick();
  size_t begins = 0;
  uint32_t oldOffset = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (isStream(sent, hapbeat::CMD_STREAM_BEGIN)) ++begins;
    if (isStream(sent, hapbeat::CMD_STREAM_DATA) && sent.ip == route)
      oldOffset = read32(sent.data, 8);
  }
  WiFiUDP::inject(route, 7700, pong("player_3/chest/group_2"));
  hapbeat_host_now_ms += 20;
  hb.tick();
  uint32_t newOffset = 0;
  begins = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (isStream(sent, hapbeat::CMD_STREAM_BEGIN)) ++begins;
    if (isStream(sent, hapbeat::CMD_STREAM_DATA) && sent.ip == route) {
      assert(sent.port == 7700);
      newOffset = read32(sent.data, 8);
    }
  }
  assert(playback.valid() && playback.status() == HapbeatPlaybackStatus::Active);
  assert(begins == 1);
  assert(newOffset > oldOffset);
}

static void test_route_identity_wins_over_address_sibling() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress route(192, 0, 2, 31);
  const IPAddress sibling(192, 0, 2, 32);
  classifyGen1(hb, route, 7700, "player_9/chest/group_1");
  classifyGen1(hb, sibling, 7701, "player_9/chest/group_2");
  Hapbeat::Playback playback = hb.playSineSource(
      100.0f, 1.0f, 0, "player_9/chest", 0.0f, true);
  hb.tick();
  const size_t beginsBefore = countStream(hapbeat::CMD_STREAM_BEGIN);
  assert(beginsBefore == 2);
  uint32_t oldOffset = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == route && isStream(sent, hapbeat::CMD_STREAM_DATA))
      oldOffset = read32(sent.data, 8);

  // The route keeps its IP/port identity but now reports the sibling address.
  // Migration must select that exact route, not coalesce the two endpoints by
  // their now-equal address.
  WiFiUDP::inject(route, 7700, pong("player_9/chest/group_2"));
  hapbeat_host_now_ms += 20;
  hb.tick();
  assert(playback.valid() && playback.isActive());
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == beginsBefore);
  uint32_t migratedOffset = 0;
  size_t routeData = 0;
  size_t siblingData = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (!isStream(sent, hapbeat::CMD_STREAM_DATA)) continue;
    if (sent.ip == route) {
      ++routeData;
      migratedOffset = read32(sent.data, 8);
    }
    if (sent.ip == sibling) ++siblingData;
  }
  assert(routeData > 0 && siblingData > 0);
  assert(migratedOffset > oldOffset);
}

static void test_exact_claim_precedes_ambiguous_migration() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  hb.setDeviceTimeout(10);
  const IPAddress stale(192, 0, 2, 33);
  const IPAddress live(192, 0, 2, 34);
  const char* address = "player_10/chest/group_1";
  classifyGen1(hb, stale, 7700, address);
  classifyGen1(hb, live, 7700, address);
  Hapbeat::Playback playback =
      hb.playSineSource(100.0f, 1.0f, 0, "player_10/chest", 0.0f, true);
  hb.tick();
  assert(playback.valid() && playback.isActive());
  size_t initialBegins = countStream(hapbeat::CMD_STREAM_BEGIN);
  assert(initialBegins == 2);
  uint32_t oldLiveOffset = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == live && isStream(sent, hapbeat::CMD_STREAM_DATA))
      oldLiveOffset = read32(sent.data, 8);

  // The stale same-address session must not steal the live session's exact
  // endpoint during migration. A global exact-claim pass keeps its cursor.
  hapbeat_host_now_ms = 20;
  WiFiUDP::inject(live, 7700, pong(address));
  WiFiUDP::sent.clear();
  hb.tick();
  size_t usedSessions = 0;
  for (uint8_t i = 0; i < Hapbeat::MAX_STREAM_SESSIONS; ++i)
    if (hb._sessions[i].used) {
      ++usedSessions;
      assert(hb._sessions[i].endpoint.ip == live);
    }
  assert(usedSessions == 1);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 0);
  uint32_t newLiveOffset = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == live && isStream(sent, hapbeat::CMD_STREAM_DATA))
      newLiveOffset = read32(sent.data, 8);
  assert(newLiveOffset > oldLiveOffset);
}

static void test_ambiguous_address_migration_starts_new_session() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  hb.setDeviceTimeout(10);
  const IPAddress oldFirst(192, 0, 2, 35);
  const IPAddress oldSecond(192, 0, 2, 36);
  const IPAddress newcomer(192, 0, 2, 37);
  const char* address = "player_11/chest/group_1";
  classifyGen1(hb, oldFirst, 7700, address);
  classifyGen1(hb, oldSecond, 7700, address);
  Hapbeat::Playback playback =
      hb.playSineSource(100.0f, 1.0f, 0, "player_11/chest", 0.0f, true);
  hb.tick();
  assert(playback.valid() && playback.isActive());
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 2);

  // Two stale same-address sessions and one new route are ambiguous. They
  // must not transfer either old cursor; the newcomer gets a fresh BEGIN.
  hapbeat_host_now_ms = 20;
  WiFiUDP::sent.clear();
  classifyGen1(hb, newcomer, 7700, address);
  hb.tick();
  assert(playback.isActive());
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 1);
  assert(countStreamTo(hapbeat::CMD_STREAM_DATA, newcomer) > 0);
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == newcomer && isStream(sent, hapbeat::CMD_STREAM_DATA)) {
      assert(read32(sent.data, 8) == 0);
      break;
    }
}

static void test_linger_rejoin_has_no_catchup_burst() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress endpoint(192, 0, 2, 60);
  classifyGen1(hb, endpoint, 7700, "player_6/chest/group_1");
  Hapbeat::Playback first = hb.playSineSource(
      100.0f, 1.0f, 0, "player_6/chest", 0.0f, true);
  hb.tick();
  hapbeat_host_now_ms = 160;
  hb.tick();
  first.stop();
  assert(first.isStopped());

  // Rejoin during the 310 ms linger after more than one send-ahead quantum.
  hapbeat_host_now_ms = 200;
  Hapbeat::Playback second = hb.playSineSource(
      100.0f, 1.0f, 0, "player_6/chest", 0.0f, true);
  assert(second.valid() && second.status() == HapbeatPlaybackStatus::Active);
  WiFiUDP::sent.clear();
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 0);
  assert(countStream(hapbeat::CMD_STREAM_DATA) == 0);
  hapbeat_host_now_ms = 220;
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_DATA) > 0);
  assert(countStream(hapbeat::CMD_STREAM_DATA) <= Hapbeat::MAX_CHUNKS_PER_TICK);
}

static void test_frame_counter_wraparound() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0xfffffff0u;
  Hapbeat hb;
  assert(hb.begin());
  classifyGen1(hb, IPAddress(192, 0, 2, 70), 7700, "player_7/chest/group_1");
  Hapbeat::Playback playback = hb.playSineSource(
      100.0f, 1.0f, 0, "player_7/chest", 0.0f, true);
  hb.tick();
  assert(playback.valid() && playback.isActive());
  assert(!WiFiUDP::sent.empty());
  Hapbeat::StreamSession& session = hb._sessions[0];
  session.startedMs = 0;
  session.framesSent = (uint64_t)0xffffffffu - 255u;
  WiFiUDP::sent.clear();
  hapbeat_host_now_ms = 0x100u;
  hb.tick();
  assert(hb._extendedMillis > (uint64_t)0xffffffffu);
  assert(session.framesSent > (uint64_t)0xffffffffu);
  assert(countStream(hapbeat::CMD_STREAM_DATA) <= Hapbeat::MAX_CHUNKS_PER_TICK);
}

static void test_fixed_point_phase_after_long_cursor() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  classifyGen1(hb, IPAddress(192, 0, 2, 80), 7700, "player_8/chest/group_1");
  Hapbeat::Playback playback = hb.playSineSource(
      440.0f, 1.0f, 0, "player_8/chest", 0.0f, true);
  assert(playback.valid() && playback.isActive());
  Hapbeat::StreamSession& session = hb._sessions[0];
  Hapbeat::SineSource& source = hb._sources[0];
  session.cursors[0] = 1u << 24;
  uint8_t packet[hapbeat::HEADER_SIZE + 4 + 4] = {0};
  uint8_t finishedMask = 0;
  hb.mixSession(session, 1, packet, sizeof(packet), finishedMask);
  const uint32_t phase =
      (uint32_t)((uint64_t)source.phaseStep * (uint64_t)(1u << 24));
  const float phaseRadians =
      (float)phase * (6.28318530717958647692f / 4294967296.0f);
  const int16_t expected =
      (int16_t)lroundf((float)source.amplitude * sinf(phaseRadians));
  std::vector<uint8_t> packetVector(packet, packet + sizeof(packet));
  assert((int16_t)read16(packetVector, 12) == expected);
  assert((int16_t)read16(packetVector, 14) == expected);
}

static void test_finite_duration_frame_count_width() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  classifyGen1(hb, IPAddress(192, 0, 2, 90), 7700, "player_12/chest/group_1");
  // 2^28 ms * 16 frames/ms is exactly 2^32 frames, which used to wrap to
  // zero and turn this finite source into an open-ended source.
  Hapbeat::Playback playback = hb.playSineSource(
      440.0f, 1.0f, 268435456u, "player_12/chest", 0.0f, false);
  assert(playback.valid() && playback.isActive());
  Hapbeat::SineSource& source = hb._sources[0];
  Hapbeat::StreamSession& session = hb._sessions[0];
  assert(source.totalFrames == 4294967296ULL);
  session.cursors[0] = 4294967295ULL;
  uint8_t packet[hapbeat::HEADER_SIZE + 4 + 4] = {0};
  uint8_t finishedMask = 0;
  hb.mixSession(session, 1, packet, sizeof(packet), finishedMask);
  assert(session.cursors[0] == 4294967296ULL);
  assert((finishedMask & 1u) != 0);

  Hapbeat::Playback maximum = hb.playSineSource(
      440.0f, 1.0f, 0xffffffffu, "player_12/chest", 0.0f, false);
  assert(maximum.valid());
  assert(hb._sources[1].totalFrames ==
         ((uint64_t)0xffffffffu * 16000ULL) / 1000ULL);
}

static void test_nonfinite_sine_parameters_are_rejected() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  Hapbeat::Playback nanFrequency =
      hb.playSineSource(NAN, 1.0f, 0, "player_14/chest", 0.0f, true);
  Hapbeat::Playback infiniteGain =
      hb.playSineSource(100.0f, INFINITY, 0, "player_14/chest", 0.0f, true);
  Hapbeat::Playback infinitePan = hb.playSineSource(
      100.0f, 1.0f, 0, "player_14/chest", -INFINITY, true);
  assert(!nanFrequency.valid());
  assert(!infiniteGain.valid());
  assert(!infinitePan.valid());
  assert(!hb._sources[0].used);

  classifyGen1(hb, IPAddress(192, 0, 2, 102), 7700, "player_14/chest/group_1");
  Hapbeat::Playback playback =
      hb.playSineSource(100.0f, 0.5f, 0, "player_14/chest", 0.25f, true);
  assert(playback.valid());
  playback.setGain(NAN);
  playback.setPan(INFINITY);
  assert(playback.gain() == 0.5f);
  assert(playback.pan() == 0.25f);
  assert(playback.loop());
  assert(hb._sources[0].gain == 0.5f);
  assert(hb._sources[0].pan == 0.25f);
  WiFiUDP::sent.clear();
  hb.play("test.inf_pan", 1.0f, "player_14/chest", INFINITY);
  assert(WiFiUDP::sent.empty());
  playback.stop();
}

// The PLAY gain on the wire (after event_id\0, target\0 and target_time).
static float playGainOnWire(const WiFiUDP::Sent& sent, const char* eventId) {
  const size_t offset = hapbeat::HEADER_SIZE + strlen(eventId) + 1 + 1 + 8;
  const uint32_t bits = read32(sent.data, offset);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

// PLAY carries clamp(gain, 0, 1), and 0 for NaN / infinity (contracts
// kit-format.md "強さの式"); the device does not clamp it.
static void test_play_gain_is_clamped() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const float gains[] = {0.6f, 1.6f, -0.2f, (float)NAN, (float)INFINITY, -(float)INFINITY};
  const float sent[] = {0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  for (size_t i = 0; i < sizeof(gains) / sizeof(gains[0]); ++i) {
    WiFiUDP::sent.clear();
    hb.play("k.e", gains[i], "", 0.0f);
    assert(WiFiUDP::sent.size() == 1);
    assert(playGainOnWire(WiFiUDP::sent[0], "k.e") == sent[i]);
  }
}

// ---- Stream Gen 2 (stream-session.md, DEC-096) ----------------------------

static uint64_t read64(const std::vector<uint8_t>& data, size_t offset) {
  return (uint64_t)read32(data, offset) | ((uint64_t)read32(data, offset + 4) << 32);
}

static bool isLeasePing(const WiFiUDP::Sent& sent) {
  return sent.data.size() == hapbeat::HEADER_SIZE + 16 &&
         sent.data[3] == hapbeat::CMD_PING;
}

static size_t countLeasePingsTo(IPAddress ip) {
  size_t count = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == ip && isLeasePing(sent)) ++count;
  return count;
}

static bool isGen2Stream(const WiFiUDP::Sent& sent) {
  return sent.data.size() >= hapbeat::HEADER_SIZE + hapbeat::STREAM_ENVELOPE_SIZE &&
         sent.data[2] == hapbeat::STREAM_VERSION &&
         sent.data[3] >= hapbeat::CMD_STREAM_BEGIN &&
         sent.data[3] <= hapbeat::CMD_STREAM_END;
}

static uint64_t bootOf(const WiFiUDP::Sent& sent) { return read64(sent.data, 8); }
static uint32_t ticketOf(const WiFiUDP::Sent& sent) { return read32(sent.data, 16); }
static uint32_t generationOf(const WiFiUDP::Sent& sent) { return read32(sent.data, 20); }

static std::vector<uint8_t> envelopeBytes(uint64_t bootId, uint32_t ticket,
                                          uint32_t generation) {
  std::vector<uint8_t> out;
  put64(out, bootId);
  put32(out, ticket);
  put32(out, generation);
  return out;
}

// ERROR (0xFF) as a 1.x device sends it (message-format.md 0xFF).
static std::vector<uint8_t> errorPacket(uint16_t code, const char* message,
                                        uint8_t refCommand,
                                        const std::vector<uint8_t>& detail) {
  std::vector<uint8_t> packet = {0x42, 0x48, hapbeat::VERSION, hapbeat::CMD_ERROR,
                                 9, 0, 0, 0};
  packet.push_back((uint8_t)(code & 0xff));
  packet.push_back((uint8_t)(code >> 8));
  packet.insert(packet.end(), message, message + strlen(message) + 1);
  packet.push_back(refCommand);
  packet.push_back(0x34);
  packet.push_back(0x12);
  packet.push_back((uint8_t)detail.size());
  packet.insert(packet.end(), detail.begin(), detail.end());
  const uint16_t payload = (uint16_t)(packet.size() - hapbeat::HEADER_SIZE);
  packet[6] = (uint8_t)(payload & 0xff);
  packet[7] = (uint8_t)(payload >> 8);
  return packet;
}

// Answer every lease PING sent to ip since `from` with an HBS2 reply.
static size_t answerLeasePings(size_t from, IPAddress ip, uint16_t port,
                               const char* address, uint64_t bootId,
                               uint32_t ticket, uint8_t flags = 0x01) {
  for (size_t i = from; i < WiFiUDP::sent.size(); ++i) {
    const WiFiUDP::Sent& sent = WiFiUDP::sent[i];
    if (sent.ip != ip || !isLeasePing(sent)) continue;
    WiFiUDP::inject(ip, port,
                    pongReply(address, read16(sent.data, 4),
                              (int64_t)read64(sent.data, 8),
                              leaseTail(flags, read64(sent.data, 16), bootId,
                                        ticket, ticket)));
  }
  return WiFiUDP::sent.size();
}

static const Hapbeat::StreamSession* sessionFor(const Hapbeat& hb, IPAddress ip) {
  for (uint8_t i = 0; i < Hapbeat::MAX_STREAM_SESSIONS; ++i)
    if (hb._sessions[i].used && hb._sessions[i].endpoint.ip == ip)
      return &hb._sessions[i];
  return nullptr;
}

static const uint64_t kBoot = 0x0102030405060708ULL;

// stream-session-v2.json, wire-extension-cases.json ping_lengths and error,
// and stream-session-cases.json ping_hex / receiver_cases begin packets.
static void test_fixture_packets() {
  uint8_t out[160];
  const std::vector<uint8_t> begin = fromHex(
      "4248023034121e000807060504030201070000002a000000803e0200000000000000803f2a00");
  size_t n = hapbeat::buildStreamBeginGen2(out, sizeof(out), 4660, kBoot, 7, 42,
                                           16000, 2, hapbeat::FORMAT_PCM16, 0,
                                           1.0f, "*");
  assert(n == begin.size() && memcmp(out, begin.data(), n) == 0);

  const std::vector<uint8_t> data =
      fromHex("42480231341218000807060504030201070000002a000000000000001000f0ff");
  const uint8_t audio[] = {0x10, 0x00, 0xf0, 0xff};
  n = hapbeat::buildStreamDataGen2(out, sizeof(out), 4660, kBoot, 7, 42, 0, audio,
                                   sizeof(audio));
  assert(n == data.size() && memcmp(out, data.data(), n) == 0);

  const std::vector<uint8_t> end =
      fromHex("42480232341210000807060504030201070000002a000000");
  n = hapbeat::buildStreamEndGen2(out, sizeof(out), 4660, kBoot, 7, 42);
  assert(n == end.size() && memcmp(out, end.data(), n) == 0);
  // envelope_hex
  const std::vector<uint8_t> envelope = fromHex("0807060504030201070000002a000000");
  assert(memcmp(out + hapbeat::HEADER_SIZE, envelope.data(), envelope.size()) == 0);

  // The data may already sit at its destination (the mixer builds in place).
  memcpy(out + hapbeat::HEADER_SIZE + 20, audio, sizeof(audio));
  n = hapbeat::buildStreamDataGen2(out, sizeof(out), 4660, kBoot, 7, 42, 0,
                                   out + hapbeat::HEADER_SIZE + 20, sizeof(audio));
  assert(n == data.size() && memcmp(out, data.data(), n) == 0);

  // A zero identity, empty DATA or a 64-character target is never built.
  assert(hapbeat::buildStreamEndGen2(out, sizeof(out), 1, 0, 7, 42) == 0);
  assert(hapbeat::buildStreamEndGen2(out, sizeof(out), 1, kBoot, 0, 42) == 0);
  assert(hapbeat::buildStreamEndGen2(out, sizeof(out), 1, kBoot, 7, 0) == 0);
  assert(hapbeat::buildStreamDataGen2(out, sizeof(out), 1, kBoot, 7, 42, 0, audio, 0) == 0);
  char target[65];
  memset(target, 'p', 63);
  target[63] = '\0';
  assert(hapbeat::buildStreamBeginGen2(out, sizeof(out), 1, kBoot, 7, 42, 16000, 2,
                                       0, 0, 1.0f, target) ==
         hapbeat::HEADER_SIZE + 28 + 64);
  target[63] = 'p';
  target[64] = '\0';
  assert(hapbeat::buildStreamBeginGen2(out, sizeof(out), 1, kBoot, 7, 42, 16000, 2,
                                       0, 0, 1.0f, target) == 0);

  // receiver_cases begin_valid (Gen 2) and begin_gen1 (Stream Gen 1 format).
  const std::vector<uint8_t> beginValid = fromHex(
      "42480230010037000807060504030201070000002a000000803e0200000000000000803f"
      "706c617965725f312f706f735f63686573742f67726f75705f3100");
  n = hapbeat::buildStreamBeginGen2(out, sizeof(out), 1, kBoot, 7, 42, 16000, 2, 0,
                                    0, 1.0f, "player_1/pos_chest/group_1");
  assert(n == beginValid.size() && memcmp(out, beginValid.data(), n) == 0);
  const std::vector<uint8_t> beginGen1 = fromHex(
      "4248013002002700803e0200000000000000803f706c617965725f312f706f735f6368"
      "6573742f67726f75705f3100");
  n = hapbeat::buildStreamBegin(out, sizeof(out), 2, 16000, 2, 0, 0, 1.0f,
                                "player_1/pos_chest/group_1");
  assert(n == beginGen1.size() && memcmp(out, beginGen1.data(), n) == 0);

  // ping_8 (discovery) and ping_16 / ping_hex (lease request).
  const std::vector<uint8_t> ping8 = fromHex("424801100100080000401e18240a0600");
  n = hapbeat::buildPing(out, sizeof(out), 1, 1700000000000000LL);
  assert(n == ping8.size() && memcmp(out, ping8.data(), n) == 0);
  const std::vector<uint8_t> ping16 =
      fromHex("424801100101100000401e18240a06008877665544332211");
  n = hapbeat::buildPingExtended(out, sizeof(out), 257, 1700000000000000LL,
                                 1234605616436508552ULL);
  assert(n == ping16.size() && memcmp(out, ping16.data(), n) == 0);
  assert(hapbeat::buildPingExtended(out, sizeof(out), 257, 1, 0) == 0);

  // error: stream_timed_out and code_and_message_only.
  hapbeat::ErrorInfo error;
  std::vector<uint8_t> packet = fromHex(
      "424801ff03032700140073747265616d2074696d6564206f7574003134121008070605"
      "040302010700000029000000");
  assert(hapbeat::parseError(packet.data(), packet.size(), error));
  assert(error.code == 20 && strcmp(error.message, "stream timed out") == 0);
  assert(strcmp(hapbeat::errorName(error.code), "STREAM_TIMED_OUT") == 0);
  assert(error.hasReference && error.refCommand == 49 && error.refSeq == 4660);
  const std::vector<uint8_t> detail = fromHex("08070605040302010700000029000000");
  assert(error.detailLength == 16 &&
         memcmp(error.detail, detail.data(), detail.size()) == 0);
  packet = fromHex(
      "424801ff03031800100073747265616d2067656e203220726571756972656400");
  assert(hapbeat::parseError(packet.data(), packet.size(), error));
  assert(error.code == 16 && strcmp(error.message, "stream gen 2 required") == 0);
  assert(strcmp(hapbeat::errorName(error.code), "STREAM_GEN_UNSUPPORTED") == 0);
  assert(!error.hasReference && error.detailLength == 0);
}

// stream-session-cases.json pong_classification: every case is a matched
// reply to ping_hex (seq 257, timestamp 1700000000000000, the incarnation).
static void test_fixture_pong_classification() {
  struct Case {
    const char* name;
    const char* hex;
    HapbeatStreamClass expected;
    bool leaseValid;
    bool superseded;
    uint32_t ticket;
    uint32_t highWater;
    bool reissued;
  };
  const char* head =
      "00401e18240a060088531e18240a0600486170626561742d4100706c617965725f312f"
      "706f735f63686573742f67726f75705f3100312e302e300008c8";
  const HapbeatStreamClass G1 = HapbeatStreamClass::Gen1;
  const HapbeatStreamClass G2 = HapbeatStreamClass::Gen2;
  const HapbeatStreamClass IGN = HapbeatStreamClass::Unknown;  // "ignored"
  const Case cases[] = {
      {"no_tail", "4248011101013d00", G1, false, false, 0, 0, false},
      {"tail_32", "4248011101015d00", G2, true, false, 7, 7, false},
      {"tail_superseded", "4248011101015d00", G2, true, true, 5, 7, false},
      {"tail_40", "4248011101016500", G2, true, false, 7, 7, false},
      {"tail_reissued", "4248011101015d00", G2, true, false, 9, 7, true},
      {"tail_unknown_flag", "4248011101015d00", G2, true, false, 7, 7, false},
      {"tail_reserved_nonzero", "4248011101015d00", G2, true, false, 7, 7, false},
      {"tail_ext_version_2", "4248011101015d00", IGN, false, false, 0, 0, false},
      {"tail_short", "4248011101015500", IGN, false, false, 0, 0, false},
      {"other_bytes", "4248011101015d00", IGN, false, false, 0, 0, false},
      {"truncated_marker", "4248011101014000", IGN, false, false, 0, 0, false},
      {"non_streaming_node", "4248011101015d00", G2, false, false, 0, 0, false},
  };
  const char* tails[] = {
      "",
      "4842533201010000887766554433221108070605040302010700000007000000",
      "4842533201030000887766554433221108070605040302010500000007000000",
      "48425332010100008877665544332211080706050403020107000000070000000001020304050607",
      "4842533201050000887766554433221108070605040302010900000007000000",
      "4842533201090000887766554433221108070605040302010700000007000000",
      "4842533201010201887766554433221108070605040302010700000007000000",
      "4842533202010000887766554433221108070605040302010700000007000000",
      "484253320101000088776655443322110807060504030201",
      "5854524100000000000000000000000000000000000000000000000000000000",
      "484253",
      "4842533201000000887766554433221108070605040302010000000000000000",
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    const Case& testCase = cases[c];
    const std::string hex = std::string(testCase.hex) + head + tails[c];
    const std::vector<uint8_t> packet = fromHex(hex.c_str());
    assert(read16(packet, 6) + hapbeat::HEADER_SIZE == packet.size());

    hapbeat::PongInfo info;
    assert(hapbeat::parsePong(packet.data(), packet.size(), info));
    assert(strcmp(info.address, "player_1/pos_chest/group_1") == 0);
    if (testCase.expected == G2) {
      assert(info.tail == hapbeat::PongTail::Gen2);
      assert(info.leaseValid == testCase.leaseValid);
      assert(info.superseded == testCase.superseded);
      assert(info.ticket == testCase.ticket);
      assert(info.highWaterTicket == testCase.highWater);
      assert(info.reissued == testCase.reissued);
    } else {
      assert(info.tail == (testCase.expected == G1 ? hapbeat::PongTail::Absent
                                                   : hapbeat::PongTail::Ignored));
    }

    WiFiUDP::clear();
    hapbeat_host_now_ms = 0;
    Hapbeat hb;
    assert(hb.begin());
    hb._incarnation = 1234605616436508552ULL;
    const IPAddress ip(192, 0, 2, 100);
    addPending(hb, 257, 1700000000000000LL, ip, 7700, hb._incarnation);
    WiFiUDP::inject(ip, 7700, packet);
    hb.poll();
    assert(hb.deviceCount() == 1);
    assert(hb.streamClass(ip) == testCase.expected);
    HapbeatStreamEndpointInfo endpoints[2];
    assert(hb.streamEndpoints(endpoints, 2) == 1);
    assert(endpoints[0].streamClass == testCase.expected);
    if (testCase.expected == G2) {
      const HapbeatLeaseState lease =
          testCase.superseded ? HapbeatLeaseState::Superseded
          : testCase.leaseValid ? HapbeatLeaseState::Valid
                                : HapbeatLeaseState::Unavailable;
      assert(endpoints[0].leaseState == lease);
      assert(hb._devices[hb.findDevice(ip, 7700)].ticket == testCase.ticket);
    } else {
      assert(endpoints[0].leaseState == HapbeatLeaseState::None);
    }
  }
}

// Extended PINGs go only by unicast to devices a source targets; ping() and
// discovery stay 8 bytes; nothing 16-byte is broadcast.
static void test_lease_ping_unicast_only() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress a(192, 0, 2, 110);
  const IPAddress b(192, 0, 2, 111);
  WiFiUDP::inject(a, 7700, pong("player_20/a/group_1"));
  WiFiUDP::inject(b, 7700, pong("player_20/b/group_1"));
  hb.tick();
  assert(hb.deviceCount() == 2);
  assert(countLeasePingsTo(a) == 0 && countLeasePingsTo(b) == 0);
  hb.ping();
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.data[3] == hapbeat::CMD_PING)
      assert(sent.data.size() == hapbeat::HEADER_SIZE + 8);

  WiFiUDP::sent.clear();
  Hapbeat::Playback playback =
      hb.playSineSource(100.0f, 1.0f, 0, "player_20/a", 0.0f, true);
  assert(playback.status() == HapbeatPlaybackStatus::Deferred);
  assert(playback.deferredReason() == HapbeatDeferredReason::UnknownClass);
  hb.tick();
  assert(countLeasePingsTo(a) == 1 && countLeasePingsTo(b) == 0);
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    assert((uint32_t)sent.ip != 0xffffffffu);
    assert(sent.data[3] < hapbeat::CMD_STREAM_BEGIN ||
           sent.data[3] > hapbeat::CMD_STREAM_END);
  }
  // The extended PING carries this incarnation and a timestamp newer than the
  // discovery PING's.
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isLeasePing(sent)) assert(read64(sent.data, 16) == hb._incarnation);
  playback.stop();
}

// Unknown, lease-unavailable and superseded endpoints defer without any
// stream packet; with several, the strongest reason is reported.
static void test_unknown_and_unavailable_defer() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress unknown(192, 0, 2, 120);
  const IPAddress node(192, 0, 2, 121);
  const IPAddress later(192, 0, 2, 122);
  WiFiUDP::inject(unknown, 7700, pong("player_21/unknown/group_1"));
  hb.poll();
  // A 1.x broker / sensor: an HBS2 tail with flags 0 and tickets 0.
  classifyGen2(hb, node, 7700, "player_21/node/group_1", kBoot, 0, 0x00);
  classifyGen2(hb, later, 7700, "player_21/later/group_1", kBoot, 5, 0x03, 7);

  Hapbeat::Playback u = hb.playSineSource(100.0f, 1.0f, 0, "player_21/unknown", 0.0f, true);
  Hapbeat::Playback n = hb.playSineSource(100.0f, 1.0f, 0, "player_21/node", 0.0f, true);
  Hapbeat::Playback s = hb.playSineSource(100.0f, 1.0f, 0, "player_21/later", 0.0f, true);
  Hapbeat::Playback all = hb.playSineSource(100.0f, 1.0f, 0, "player_21", 0.0f, true);
  Hapbeat::Playback none = hb.playSineSource(100.0f, 1.0f, 0, "player_99", 0.0f, true);
  for (int i = 0; i < 5; ++i) {
    hapbeat_host_now_ms += 20;
    hb.tick();
  }
  assert(u.deferredReason() == HapbeatDeferredReason::UnknownClass);
  assert(n.deferredReason() == HapbeatDeferredReason::LeaseUnavailable);
  assert(s.deferredReason() == HapbeatDeferredReason::Superseded);
  assert(all.deferredReason() == HapbeatDeferredReason::Superseded);
  assert(none.deferredReason() == HapbeatDeferredReason::NoResolvedEndpoint);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 0);
  assert(countStream(hapbeat::CMD_STREAM_DATA) == 0);
  assert(countStream(hapbeat::CMD_STREAM_END) == 0);
  HapbeatStreamEndpointInfo endpoints[4];
  assert(hb.streamEndpoints(endpoints, 4) == 3);
  for (uint8_t i = 0; i < 3; ++i) {
    if (endpoints[i].ip == unknown)
      assert(endpoints[i].streamClass == HapbeatStreamClass::Unknown);
    if (endpoints[i].ip == node)
      assert(endpoints[i].leaseState == HapbeatLeaseState::Unavailable);
    if (endpoints[i].ip == later)
      assert(endpoints[i].leaseState == HapbeatLeaseState::Superseded);
  }
}

// Only a matched reply changes the class; an unmatched lease reply is dropped
// whole; a late reply never rolls the lease back; an unsolicited PONG keeps it.
static void test_unmatched_pong_keeps_class() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 125);
  const IPAddress other(192, 0, 2, 126);
  const char* address = "player_22/a/group_1";
  WiFiUDP::inject(ip, 7700, pong(address));
  hb.poll();

  const uint16_t seq = hb.nextSeq();
  const int64_t timestamp = hb.nextPingTimestamp();
  addPending(hb, seq, timestamp, ip, 7700, hb._incarnation);
  WiFiUDP::inject(ip, 7700, pongReply(address, (uint16_t)(seq + 1), timestamp, {}));
  WiFiUDP::inject(ip, 7700, pongReply(address, seq, timestamp + 1, {}));
  WiFiUDP::inject(other, 7700, pongReply(address, seq, timestamp, {}));
  WiFiUDP::inject(ip, 7700, pongReply(address, seq, 0, {}));
  hb.poll();
  assert(hb.streamClass(ip) == HapbeatStreamClass::Unknown);
  assert(hb.streamClass(other) == HapbeatStreamClass::Unknown);

  // A lease reply echoing another incarnation is dropped, address included.
  WiFiUDP::inject(ip, 7700,
                  pongReply("player_22/z/group_1", seq, timestamp,
                            leaseTail(0x01, hb._incarnation + 1, kBoot, 7, 7)));
  hb.poll();
  assert(hb.streamClass(ip) == HapbeatStreamClass::Unknown);
  assert(strcmp(hb._devices[hb.findDevice(ip, 7700)].address, address) == 0);

  // Accept a newer request, then a late reply to the older one.
  const uint16_t newerSeq = hb.nextSeq();
  const int64_t newerTimestamp = hb.nextPingTimestamp();
  addPending(hb, newerSeq, newerTimestamp, ip, 7700, hb._incarnation);
  WiFiUDP::inject(ip, 7700,
                  pongReply(address, newerSeq, newerTimestamp,
                            leaseTail(0x01, hb._incarnation, kBoot + 1, 3, 3)));
  WiFiUDP::inject(ip, 7700,
                  pongReply(address, seq, timestamp,
                            leaseTail(0x01, hb._incarnation, kBoot, 7, 7)));
  WiFiUDP::inject(ip, 7700, pong(address));  // unsolicited, no tail
  hb.poll();
  const Hapbeat::KnownDevice& device = hb._devices[hb.findDevice(ip, 7700)];
  assert(device.streamClass == HapbeatStreamClass::Gen2);
  assert(device.bootId == kBoot + 1 && device.ticket == 3 && device.leaseValid);
}

// ERRORs count only from a known device on the Hapbeat port.
static void test_error_acceptance() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 130);
  classifyGen1(hb, ip, 7700, "player_23/a/group_1");
  const std::vector<uint8_t> genUnsupported = fromHex(
      "424801ff03031800100073747265616d2067656e203220726571756972656400");
  WiFiUDP::inject(ip, 7701, genUnsupported);
  WiFiUDP::inject(IPAddress(192, 0, 2, 131), 7700, genUnsupported);
  WiFiUDP::sent.clear();
  hb.poll();
  assert(hb.streamClass(ip) == HapbeatStreamClass::Gen1);
  assert(WiFiUDP::sent.empty());
  WiFiUDP::inject(ip, 7700, genUnsupported);
  hb.poll();
  assert(hb.streamClass(ip) == HapbeatStreamClass::Unknown);
  assert(countLeasePingsTo(ip) == 1);  // classify again at once
}

// Gen 2 devices get version-2, envelope-carrying exact unicast; Gen 1 devices
// get Stream Gen 1; both under one target stream concurrently.
static void test_gen2_packets_and_mixed_classes() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress g2(192, 0, 2, 140);
  const IPAddress g1(192, 0, 2, 141);
  classifyGen2(hb, g2, 7700, "player_30/a/group_1", kBoot, 7);
  classifyGen1(hb, g1, 7700, "player_30/b/group_1");
  Hapbeat::Playback playback =
      hb.playSineSource(100.0f, 1.0f, 0, "player_30", 0.0f, true);
  assert(playback.isActive());
  hb.tick();
  hapbeat_host_now_ms += 20;
  hb.tick();
  size_t g2Data = 0;
  size_t g1Data = 0;
  uint32_t expectedOffset = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (sent.data[3] < hapbeat::CMD_STREAM_BEGIN ||
        sent.data[3] > hapbeat::CMD_STREAM_END)
      continue;
    assert((uint32_t)sent.ip != 0xffffffffu && sent.port == 7700);
    assert(read16(sent.data, 6) + hapbeat::HEADER_SIZE == sent.data.size());
    if (sent.ip == g2) {
      assert(isGen2Stream(sent));
      assert(bootOf(sent) == kBoot && ticketOf(sent) == 7 && generationOf(sent) == 1);
      if (sent.data[3] == hapbeat::CMD_STREAM_BEGIN) {
        assert(strcmp((const char*)&sent.data[36], "player_30/a/group_1") == 0);
      }
      if (sent.data[3] == hapbeat::CMD_STREAM_DATA) {
        assert(read32(sent.data, 24) == expectedOffset);
        expectedOffset += (uint32_t)(sent.data.size() - 28);
        ++g2Data;
      }
    } else {
      assert(sent.ip == g1 && sent.data[2] == hapbeat::VERSION);
      if (sent.data[3] == hapbeat::CMD_STREAM_DATA) ++g1Data;
    }
  }
  assert(g2Data > 0 && g1Data > 0);
  assert(countStreamTo(hapbeat::CMD_STREAM_BEGIN, g1) == 1);
}

// After a Gen 2 END the next source begins the next generation at once; only
// the Gen 1 device waits 300 ms (and its source counts as Active meanwhile).
static void test_gen2_immediate_restart_gen1_guard() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress g2(192, 0, 2, 145);
  const IPAddress g1(192, 0, 2, 146);
  classifyGen2(hb, g2, 7700, "player_31/a/group_1", kBoot, 7);
  classifyGen1(hb, g1, 7700, "player_31/b/group_1");
  Hapbeat::Playback first = hb.playSineSource(100.0f, 1.0f, 0, "player_31", 0.0f, true);
  hb.tick();
  first.stop();
  hapbeat_host_now_ms += 10;
  hb.tick();
  hapbeat_host_now_ms += 310;
  hb.tick();
  assert(countStreamTo(hapbeat::CMD_STREAM_END, g2) == 1);
  assert(countStreamTo(hapbeat::CMD_STREAM_END, g1) == 1);

  WiFiUDP::sent.clear();
  Hapbeat::Playback second = hb.playSineSource(100.0f, 1.0f, 0, "player_31", 0.0f, true);
  assert(second.isActive());
  assert(countStreamTo(hapbeat::CMD_STREAM_BEGIN, g2) == 1);
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (sent.ip == g2 && isStream(sent, hapbeat::CMD_STREAM_BEGIN))
      assert(generationOf(sent) == 2);
  assert(countStreamTo(hapbeat::CMD_STREAM_BEGIN, g1) == 0);
  hapbeat_host_now_ms += 100;
  hb.tick();
  assert(countStreamTo(hapbeat::CMD_STREAM_BEGIN, g1) == 0);
  assert(countStreamTo(hapbeat::CMD_STREAM_DATA, g2) > 0);
  hapbeat_host_now_ms += 210;
  hb.tick();
  assert(countStreamTo(hapbeat::CMD_STREAM_BEGIN, g1) == 1);
  second.stop();
}

// BEGIN is sent at least three times before the third DATA and then at most
// 500 ms apart, always the same tuple; never repeated to a Gen 1 device.
static void test_gen2_begin_repetition() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress g2(192, 0, 2, 150);
  const IPAddress g1(192, 0, 2, 151);
  classifyGen2(hb, g2, 7700, "player_32/a/group_1", kBoot, 7);
  classifyGen1(hb, g1, 7700, "player_32/b/group_1");
  WiFiUDP::sent.clear();
  Hapbeat::Playback playback = hb.playSineSource(100.0f, 1.0f, 0, "player_32", 0.0f, true);
  std::vector<uint32_t> beginTimes;
  size_t beginsBeforeThirdData = 0;
  size_t data = 0;
  size_t seen = 0;
  for (uint32_t t = 0; t <= 3000; t += 10) {
    hapbeat_host_now_ms = t;
    hb.tick();
    for (; seen < WiFiUDP::sent.size(); ++seen) {
      const WiFiUDP::Sent& sent = WiFiUDP::sent[seen];
      if (sent.ip != g2 || !isGen2Stream(sent)) continue;
      assert(bootOf(sent) == kBoot && ticketOf(sent) == 7 && generationOf(sent) == 1);
      if (sent.data[3] == hapbeat::CMD_STREAM_BEGIN) {
        beginTimes.push_back(t);
        if (data < 3) ++beginsBeforeThirdData;
      }
      if (sent.data[3] == hapbeat::CMD_STREAM_DATA) ++data;
    }
  }
  assert(beginsBeforeThirdData >= 3);
  for (size_t i = 1; i < beginTimes.size(); ++i)
    assert(beginTimes[i] - beginTimes[i - 1] <= 500);
  assert(beginTimes.back() >= 2500);
  assert(countStreamTo(hapbeat::CMD_STREAM_BEGIN, g1) == 1);
  assert(countStreamTo(hapbeat::CMD_STREAM_END, g2) == 0);
  playback.stop();
}

// A sender that could not tick for 3 s ends the generation and begins the
// next one before more DATA, which starts again at offset 0.
static void test_gen2_own_gap_restart() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress g2(192, 0, 2, 155);
  classifyGen2(hb, g2, 7700, "player_33/a/group_1", kBoot, 7);
  Hapbeat::Playback playback = hb.playSineSource(100.0f, 1.0f, 0, "player_33", 0.0f, true);
  for (uint32_t t = 0; t <= 200; t += 20) {
    hapbeat_host_now_ms = t;
    hb.tick();
  }
  const uint64_t cursor = sessionFor(hb, g2)->cursors[0];
  WiFiUDP::sent.clear();
  hapbeat_host_now_ms += 3000;
  hb.tick();
  int endIndex = -1;
  int beginIndex = -1;
  int dataIndex = -1;
  size_t data = 0;
  for (size_t i = 0; i < WiFiUDP::sent.size(); ++i) {
    const WiFiUDP::Sent& sent = WiFiUDP::sent[i];
    if (!isGen2Stream(sent)) continue;
    if (sent.data[3] == hapbeat::CMD_STREAM_END && endIndex < 0) {
      assert(generationOf(sent) == 1);
      endIndex = (int)i;
    }
    if (sent.data[3] == hapbeat::CMD_STREAM_BEGIN && beginIndex < 0) {
      assert(generationOf(sent) == 2);
      beginIndex = (int)i;
    }
    if (sent.data[3] == hapbeat::CMD_STREAM_DATA) {
      assert(generationOf(sent) == 2);
      if (dataIndex < 0) {
        dataIndex = (int)i;
        assert(read32(sent.data, 24) == 0);
      }
      ++data;
    }
  }
  assert(endIndex >= 0 && endIndex < beginIndex && beginIndex < dataIndex);
  assert(data <= Hapbeat::MAX_CHUNKS_PER_TICK);
  assert(sessionFor(hb, g2)->cursors[0] > cursor);  // continued, not restarted
  playback.stop();
}

// A superseded lease defers; a new ticket without lease_reissued does not
// clear it; lease_reissued does; reacquireStreamOwnership() also does.
static void test_superseded_and_reissue() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 160);
  const char* address = "player_34/a/group_1";
  classifyGen2(hb, ip, 7700, address, kBoot, 5, 0x03, 7);
  Hapbeat::Playback playback = hb.playSineSource(100.0f, 1.0f, 0, "player_34", 0.0f, true);
  hb.tick();
  assert(playback.deferredReason() == HapbeatDeferredReason::Superseded);
  classifyGen2(hb, ip, 7700, address, kBoot, 8, 0x01);
  hb.tick();
  assert(playback.deferredReason() == HapbeatDeferredReason::Superseded);
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 0);
  classifyGen2(hb, ip, 7700, address, kBoot, 9, 0x05, 7);
  assert(playback.isActive());
  hb.tick();
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isGen2Stream(sent)) assert(ticketOf(sent) == 9);
  assert(countStream(hapbeat::CMD_STREAM_DATA) > 0);

  // Superseded while streaming: END now, then defer.
  WiFiUDP::sent.clear();
  classifyGen2(hb, ip, 7700, address, kBoot, 9, 0x03, 11);
  assert(countStream(hapbeat::CMD_STREAM_END) == 1);
  assert(playback.deferredReason() == HapbeatDeferredReason::Superseded);
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_DATA) == 0);

  // Explicit reacquisition: a fresh incarnation and an immediate lease PING.
  const uint64_t oldIncarnation = hb._incarnation;
  WiFiUDP::sent.clear();
  hb.reacquireStreamOwnership();
  assert(hb._incarnation != oldIncarnation && hb._incarnation != 0);
  assert(countLeasePingsTo(ip) == 1);
  HapbeatStreamEndpointInfo info;
  assert(hb.streamEndpoints(&info, 1) == 1);
  assert(info.leaseState == HapbeatLeaseState::Unavailable);
  answerLeasePings(0, ip, 7700, address, kBoot, 12);
  hb.tick();
  assert(playback.isActive());
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) > 0);
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isGen2Stream(sent)) assert(ticketOf(sent) == 12 && generationOf(sent) == 1);
  playback.stop();
}

// A new ticket of the same boot ENDs the old identity and continues the
// cursor under the new ticket; a new boot ENDs the old identity first too.
static void test_ticket_and_boot_change() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 165);
  const char* address = "player_35/a/group_1";
  classifyGen2(hb, ip, 7700, address, kBoot, 7);
  Hapbeat::Playback playback = hb.playSineSource(440.0f, 1.0f, 0, "player_35", 0.0f, true);
  for (uint32_t t = 0; t <= 100; t += 20) {
    hapbeat_host_now_ms = t;
    hb.tick();
  }
  const uint64_t cursor = sessionFor(hb, ip)->cursors[0];
  assert(cursor > 0);
  WiFiUDP::sent.clear();
  classifyGen2(hb, ip, 7700, address, kBoot, 8);
  assert(WiFiUDP::sent.size() == 2);
  assert(isGen2Stream(WiFiUDP::sent[0]) &&
         WiFiUDP::sent[0].data[3] == hapbeat::CMD_STREAM_END &&
         ticketOf(WiFiUDP::sent[0]) == 7 && generationOf(WiFiUDP::sent[0]) == 1);
  assert(WiFiUDP::sent[1].data[3] == hapbeat::CMD_STREAM_BEGIN &&
         ticketOf(WiFiUDP::sent[1]) == 8 && generationOf(WiFiUDP::sent[1]) == 1);
  assert(sessionFor(hb, ip)->cursors[0] == cursor);
  hapbeat_host_now_ms += 10;
  hb.tick();
  bool checked = false;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (!isStream(sent, hapbeat::CMD_STREAM_DATA) || checked) continue;
    assert(ticketOf(sent) == 8 && read32(sent.data, 24) == 0);
    // The first sample continues the phase from the carried cursor.
    const Hapbeat::SineSource& source = hb._sources[0];
    const uint32_t phase = (uint32_t)((uint64_t)source.phaseStep * cursor);
    const int16_t expected = (int16_t)lroundf(
        (float)source.amplitude *
        sinf((float)phase * (6.28318530717958647692f / 4294967296.0f)));
    assert((int16_t)read16(sent.data, 28) == expected);
    checked = true;
  }
  assert(checked);

  WiFiUDP::sent.clear();
  classifyGen2(hb, ip, 7700, address, kBoot + 1, 1);
  assert(WiFiUDP::sent.size() == 2);
  assert(WiFiUDP::sent[0].data[3] == hapbeat::CMD_STREAM_END &&
         bootOf(WiFiUDP::sent[0]) == kBoot && ticketOf(WiFiUDP::sent[0]) == 8);
  assert(WiFiUDP::sent[1].data[3] == hapbeat::CMD_STREAM_BEGIN &&
         bootOf(WiFiUDP::sent[1]) == kBoot + 1 && ticketOf(WiFiUDP::sent[1]) == 1);
  assert(sessionFor(hb, ip)->cursors[0] == 0);  // a rebooted device starts anew
  playback.stop();
}

// A class change (firmware update) ENDs the Gen 1 session in the Gen 1 format
// before the first Gen 2 BEGIN, with no 300 ms wait.
static void test_class_change_ends_old_format() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 170);
  const char* address = "player_36/a/group_1";
  classifyGen1(hb, ip, 7700, address);
  Hapbeat::Playback playback = hb.playSineSource(100.0f, 1.0f, 0, "player_36", 0.0f, true);
  hb.tick();
  WiFiUDP::sent.clear();
  classifyGen2(hb, ip, 7700, address, kBoot, 7);
  assert(WiFiUDP::sent.size() == 2);
  assert(WiFiUDP::sent[0].data[2] == hapbeat::VERSION &&
         WiFiUDP::sent[0].data[3] == hapbeat::CMD_STREAM_END &&
         WiFiUDP::sent[0].data.size() == hapbeat::HEADER_SIZE);
  assert(isGen2Stream(WiFiUDP::sent[1]) &&
         WiFiUDP::sent[1].data[3] == hapbeat::CMD_STREAM_BEGIN);
  assert(playback.isActive());
  playback.stop();
}

// Stream ERRORs are handled only for this library's own envelopes.
static void test_stream_errors() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 175);
  const char* address = "player_37/a/group_1";
  classifyGen2(hb, ip, 7700, address, kBoot, 7);
  Hapbeat::Playback playback = hb.playSineSource(100.0f, 1.0f, 0, "player_37", 0.0f, true);
  for (uint32_t t = 0; t <= 100; t += 20) {
    hapbeat_host_now_ms = t;
    hb.tick();
  }

  // NOT_STARTED: an unknown envelope is ignored, ours resends the BEGIN.
  WiFiUDP::sent.clear();
  WiFiUDP::inject(ip, 7700, errorPacket(hapbeat::ERR_STREAM_NOT_STARTED, "not started",
                                        hapbeat::CMD_STREAM_DATA,
                                        envelopeBytes(kBoot, 7, 99)));
  hb.poll();
  assert(WiFiUDP::sent.empty());
  WiFiUDP::inject(ip, 7700, errorPacket(hapbeat::ERR_STREAM_NOT_STARTED, "not started",
                                        hapbeat::CMD_STREAM_DATA,
                                        envelopeBytes(kBoot, 7, 1)));
  hb.poll();
  assert(WiFiUDP::sent.size() == 1 &&
         WiFiUDP::sent[0].data[3] == hapbeat::CMD_STREAM_BEGIN &&
         generationOf(WiFiUDP::sent[0]) == 1);

  // LEASE_INVALID: renew by lease PING; the session ends and keeps its cursor.
  const uint64_t cursor = sessionFor(hb, ip)->cursors[0];
  WiFiUDP::sent.clear();
  WiFiUDP::inject(ip, 7700, errorPacket(hapbeat::ERR_STREAM_LEASE_INVALID, "lease",
                                        hapbeat::CMD_STREAM_DATA,
                                        envelopeBytes(kBoot, 7, 1)));
  hb.poll();
  assert(countLeasePingsTo(ip) == 1);
  assert(countStream(hapbeat::CMD_STREAM_END) == 1);
  assert(playback.deferredReason() == HapbeatDeferredReason::LeaseUnavailable);
  hapbeat_host_now_ms += 20;
  hb.tick();
  assert(countStream(hapbeat::CMD_STREAM_DATA) == 0);
  answerLeasePings(0, ip, 7700, address, kBoot, 8);
  WiFiUDP::sent.clear();
  hb.poll();
  assert(playback.isActive());
  assert(WiFiUDP::sent.size() == 1 && ticketOf(WiFiUDP::sent[0]) == 8 &&
         generationOf(WiFiUDP::sent[0]) == 1);
  assert(sessionFor(hb, ip)->cursors[0] == cursor);
  hapbeat_host_now_ms += 20;
  hb.tick();

  // An ERROR about the session just ended is still ours.
  const std::vector<uint8_t> endedEnvelope = envelopeBytes(kBoot, 7, 1);
  gWarnings.clear();
  hb.setWarningHandler(recordWarning);
  WiFiUDP::inject(ip, 7700, errorPacket(hapbeat::ERR_STREAM_ENDED, "ended",
                                        hapbeat::CMD_STREAM_DATA, endedEnvelope));
  WiFiUDP::inject(ip, 7700, errorPacket(hapbeat::ERR_STREAM_TIMED_OUT, "timed out",
                                        hapbeat::CMD_STREAM_DATA,
                                        envelopeBytes(kBoot, 6, 1)));
  hb.poll();
  assert(gWarnings.size() == 1 &&
         gWarnings[0].find("STREAM_ENDED") != std::string::npos);

  // SUPERSEDED: END now and defer, no automatic takeover.
  WiFiUDP::sent.clear();
  WiFiUDP::inject(ip, 7700, errorPacket(hapbeat::ERR_STREAM_SUPERSEDED, "superseded",
                                        hapbeat::CMD_STREAM_DATA,
                                        envelopeBytes(kBoot, 8, 1)));
  hb.poll();
  assert(countStream(hapbeat::CMD_STREAM_END) == 1);
  assert(playback.deferredReason() == HapbeatDeferredReason::Superseded);
  hb.setWarningHandler(nullptr);
  playback.stop();
}

// Lease PINGs refresh at most every 5 s while streaming and stop one TTL
// (15 s) after the last session ended.
static void test_lease_ping_interval_and_stop() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 180);
  const char* address = "player_38/a/group_1";
  classifyGen2(hb, ip, 7700, address, kBoot, 7);
  Hapbeat::Playback playback = hb.playSineSource(100.0f, 1.0f, 0, "player_38", 0.0f, true);
  std::vector<uint32_t> pingTimes;
  size_t answered = 0;
  size_t seen = 0;
  for (uint32_t t = 0; t <= 45000; t += 100) {
    hapbeat_host_now_ms = t;
    if (t == 10000) playback.stop();
    hb.tick();
    for (; seen < WiFiUDP::sent.size(); ++seen)
      if (WiFiUDP::sent[seen].ip == ip && isLeasePing(WiFiUDP::sent[seen]))
        pingTimes.push_back(t);
    answered = answerLeasePings(answered, ip, 7700, address, kBoot, 7);
    if (t > 11000) WiFiUDP::sent.clear(), seen = 0, answered = 0;
  }
  assert(pingTimes.size() >= 4);
  for (size_t i = 1; i < pingTimes.size(); ++i)
    assert(pingTimes[i] - pingTimes[i - 1] <= 5000);
  assert(pingTimes.back() > 20000);           // kept up after the session
  assert(pingTimes.back() <= 10310 + 15000);  // and stopped one TTL later
}

// Warnings go only through the handler, once per device.
static void test_warning_handler() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  const IPAddress ip(192, 0, 2, 185);
  const char* address = "player_39/a/group_1";
  // Without a handler nothing is reported (and nothing breaks).
  classifyGen2(hb, ip, 7700, address, kBoot, 5, 0x03, 7);
  gWarnings.clear();
  hb.setWarningHandler(recordWarning);
  for (int i = 0; i < 2; ++i) {
    const uint16_t seq = hb.nextSeq();
    const int64_t timestamp = hb.nextPingTimestamp();
    addPending(hb, seq, timestamp, ip, 7700, hb._incarnation);
    WiFiUDP::inject(ip, 7700,
                    pongReply(address, seq, timestamp,
                              fromHex("4842533202010000887766554433221108070605"
                                      "040302010700000007000000")));
    hb.poll();
  }
  assert(gWarnings.size() == 1);
  assert(gWarnings[0].rfind("Hapbeat 192.0.2.185: ", 0) == 0);
  assert(gWarnings[0].find("tail not understood") != std::string::npos);

  const IPAddress other(192, 0, 2, 186);
  classifyGen2(hb, other, 7700, "player_39/b/group_1", kBoot, 5, 0x03, 7);
  classifyGen2(hb, other, 7700, "player_39/b/group_1", kBoot, 5, 0x03, 7);
  assert(gWarnings.size() == 2);
  assert(gWarnings[1].find("superseded") != std::string::npos);
  hb.setWarningHandler(nullptr);
}

int main() {
  test_routing_and_mixing();
  test_same_address_different_ip_are_distinct();
  test_deferred_late_pong_and_cooldown();
  test_begin_retry_and_source_pool_exhaustion();
  test_route_migration_keeps_session();
  test_route_identity_wins_over_address_sibling();
  test_exact_claim_precedes_ambiguous_migration();
  test_ambiguous_address_migration_starts_new_session();
  test_linger_rejoin_has_no_catchup_burst();
  test_frame_counter_wraparound();
  test_fixed_point_phase_after_long_cursor();
  test_finite_duration_frame_count_width();
  test_nonfinite_sine_parameters_are_rejected();
  test_play_gain_is_clamped();
  test_fixture_packets();
  test_fixture_pong_classification();
  test_lease_ping_unicast_only();
  test_unknown_and_unavailable_defer();
  test_unmatched_pong_keeps_class();
  test_error_acceptance();
  test_gen2_packets_and_mixed_classes();
  test_gen2_immediate_restart_gen1_guard();
  test_gen2_begin_repetition();
  test_gen2_own_gap_restart();
  test_superseded_and_reissue();
  test_ticket_and_boot_change();
  test_class_change_ends_old_format();
  test_stream_errors();
  test_lease_ping_interval_and_stop();
  test_warning_handler();
  printf("host tests: 30 passed\n");
  return 0;
}
