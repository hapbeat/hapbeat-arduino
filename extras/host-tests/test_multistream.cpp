#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>
#include <utility>

#include <vector>

// The shared JSON fixture is not directly consumable by this Arduino fake UDP
// harness. These deterministic cases mirror its endpoint, lifecycle, pacing,
// and mixing assertions from sdk-multi-stream.md sections 4-7.
#define private public
#include "Hapbeat.h"
#undef private

uint32_t hapbeat_host_now_ms = 0;
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
  WiFiUDP::inject(IPAddress(192, 0, 2, 10), 7700,
                  pong("player_1/left/group_1"));
  WiFiUDP::inject(IPAddress(192, 0, 2, 11), 7701,
                  pong("player_1/right/group_1"));
  hb.poll();
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
  WiFiUDP::inject(first, 7700, pong("player_4/chest/group_1"));
  WiFiUDP::inject(second, 7701, pong("player_4/chest/group_1"));
  hb.poll();
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

  WiFiUDP::inject(IPAddress(192, 0, 2, 20), 7700,
                  pong("player_2/chest/group_1"));
  hb.tick();
  assert(hb.deviceCount() == 1);
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

  Hapbeat::Playback cooled = hb.playSineSource(
      100.0f, 1.0f, 0, "player_2/chest", 0.0f, true);
  assert(cooled.valid() && cooled.status() == HapbeatPlaybackStatus::Deferred);
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
  WiFiUDP::inject(IPAddress(192, 0, 2, 50), 7700,
                  pong("player_5/chest/group_1"));
  hb.poll();
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
  WiFiUDP::inject(IPAddress(192, 0, 2, 51), 7700,
                  pong("player_5/generation/group_1"));
  generationHub.poll();
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
  WiFiUDP::inject(route, 7700, pong("player_3/chest/group_1"));
  hb.poll();
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
  WiFiUDP::inject(route, 7700, pong("player_9/chest/group_1"));
  WiFiUDP::inject(sibling, 7701, pong("player_9/chest/group_2"));
  hb.poll();
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
  WiFiUDP::inject(stale, 7700, pong(address));
  WiFiUDP::inject(live, 7700, pong(address));
  hb.poll();
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
  WiFiUDP::inject(oldFirst, 7700, pong(address));
  WiFiUDP::inject(oldSecond, 7700, pong(address));
  hb.poll();
  Hapbeat::Playback playback =
      hb.playSineSource(100.0f, 1.0f, 0, "player_11/chest", 0.0f, true);
  hb.tick();
  assert(playback.valid() && playback.isActive());
  assert(countStream(hapbeat::CMD_STREAM_BEGIN) == 2);

  // Two stale same-address sessions and one new route are ambiguous. They
  // must not transfer either old cursor; the newcomer gets a fresh BEGIN.
  hapbeat_host_now_ms = 20;
  WiFiUDP::inject(newcomer, 7700, pong(address));
  WiFiUDP::sent.clear();
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
  WiFiUDP::inject(endpoint, 7700, pong("player_6/chest/group_1"));
  hb.poll();
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
  WiFiUDP::inject(IPAddress(192, 0, 2, 70), 7700,
                  pong("player_7/chest/group_1"));
  hb.poll();
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
  WiFiUDP::inject(IPAddress(192, 0, 2, 80), 7700,
                  pong("player_8/chest/group_1"));
  hb.poll();
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
  WiFiUDP::inject(IPAddress(192, 0, 2, 90), 7700,
                  pong("player_12/chest/group_1"));
  hb.poll();
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

static void test_playback_owner_lifetime() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat::Playback orphan;
  {
    Hapbeat first;
    assert(first.begin());
    WiFiUDP::inject(IPAddress(192, 0, 2, 100), 7700,
                    pong("player_13/chest/group_1"));
    first.poll();
    orphan = first.playSineSource(100.0f, 0.75f, 0, "player_13/chest", 0.0f,
                                  true);
    assert(orphan.valid());
  }
  // The owner token is retired before the stack address can be reused.
  assert(!orphan.valid());
  assert(orphan.status() == HapbeatPlaybackStatus::Stopped);
  assert(orphan.deferredReason() == HapbeatDeferredReason::None);
  orphan.setGain(0.1f);
  orphan.setPan(-1.0f);
  orphan.setLoop(false);
  orphan.stop();

  Hapbeat second;
  assert(second.begin());
  WiFiUDP::inject(IPAddress(192, 0, 2, 101), 7700,
                  pong("player_13/chest/group_1"));
  second.poll();
  Hapbeat::Playback current =
      second.playSineSource(100.0f, 0.5f, 0, "player_13/chest", 0.0f, true);
  assert(current.valid());
  orphan.setGain(0.9f);
  orphan.setPan(1.0f);
  orphan.setLoop(false);
  assert(current.gain() == 0.5f);
  assert(current.pan() == 0.0f);
  assert(current.loop());
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

  WiFiUDP::inject(IPAddress(192, 0, 2, 102), 7700,
                  pong("player_14/chest/group_1"));
  hb.poll();
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
  hb.play("test.nan_gain", NAN, "player_14/chest", 0.0f);
  hb.play("test.inf_pan", 1.0f, "player_14/chest", INFINITY);
  assert(WiFiUDP::sent.empty());
  playback.stop();
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
  test_playback_owner_lifetime();
  test_nonfinite_sine_parameters_are_rejected();
  return 0;
}
