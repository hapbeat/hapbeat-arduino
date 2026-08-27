#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "Hapbeat.h"

uint32_t hapbeat_host_now_ms = 0;
std::vector<WiFiUDP::Sent> WiFiUDP::sent;
std::deque<WiFiUDP::Inbound> WiFiUDP::inbound;

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

  Hapbeat::Playback* left = hb.playSineSource(
      4000.0f, 1.0f, 0, "player_1/left", -1.0f, true);
  Hapbeat::Playback* both = hb.playSineSource(
      4000.0f, 1.0f, 0, "player_1/*", -1.0f, true);
  assert(left && both);
  assert(left->status() == HapbeatPlaybackStatus::Active);
  assert(both->status() == HapbeatPlaybackStatus::Active);
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
  left->stop();
  assert(left->isStopped());
  assert(both->isActive());
  both->stop();
  hb.tick();
  assert(both->isStopped());
}

static void test_deferred_late_pong_and_cooldown() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  assert(hapbeat::addressMatches("player_2/chest",
                                 "player_2/chest/group_1"));
  Hapbeat hb;
  assert(hb.begin());
  hb.setDeviceIp(IPAddress(192, 0, 2, 20));
  Hapbeat::Playback* pinned = hb.playSineSource(
      100.0f, 1.0f, 0, "player_2/chest", 0.0f, true);
  assert(pinned && pinned->status() == HapbeatPlaybackStatus::Deferred);
  hb.tick();
  assert(WiFiUDP::sent.empty());
  pinned->stop();
  Hapbeat::Playback* deferred = hb.playSineSource(
      100.0f, 1.0f, 1000, "player_2/chest", 0.0f, false);
  assert(deferred && deferred->status() == HapbeatPlaybackStatus::Deferred);
  hb.tick();
  assert(WiFiUDP::sent.empty());

  WiFiUDP::inject(IPAddress(192, 0, 2, 20), 7700,
                  pong("player_2/chest/group_1"));
  hb.tick();
  assert(hb.deviceCount() == 1);
  assert(deferred->status() == HapbeatPlaybackStatus::Active);
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
    assert(WiFiUDP::sent[0].data[20 + i] ==
           (uint8_t)expectedTarget[i]);
  assert(read16(WiFiUDP::sent[1].data, 8) == 0);

  deferred->stop();
  hb.tick();
  hapbeat_host_now_ms += 310;
  hb.tick();
  size_t endCount = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isStream(sent, hapbeat::CMD_STREAM_END)) ++endCount;
  assert(endCount == 1);

  Hapbeat::Playback* cooled = hb.playSineSource(
      100.0f, 1.0f, 0, "player_2/chest", 0.0f, true);
  assert(cooled && cooled->status() == HapbeatPlaybackStatus::Deferred);
  hb.tick();
  size_t beginCount = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isStream(sent, hapbeat::CMD_STREAM_BEGIN)) ++beginCount;
  assert(beginCount == 1);
  hapbeat_host_now_ms += 310;
  hb.tick();
  beginCount = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent)
    if (isStream(sent, hapbeat::CMD_STREAM_BEGIN)) ++beginCount;
  assert(beginCount == 2);
}

static void test_route_migration_keeps_session() {
  WiFiUDP::clear();
  hapbeat_host_now_ms = 0;
  Hapbeat hb;
  assert(hb.begin());
  WiFiUDP::inject(IPAddress(192, 0, 2, 30), 7700,
                  pong("player_3/chest/group_1"));
  hb.poll();
  Hapbeat::Playback* playback = hb.playSineSource(
      100.0f, 1.0f, 0, "player_3/chest", 0.0f, true);
  hb.tick();
  size_t begins = 0;
  uint32_t oldOffset = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (isStream(sent, hapbeat::CMD_STREAM_BEGIN)) ++begins;
    if (isStream(sent, hapbeat::CMD_STREAM_DATA) && sent.ip == IPAddress(192, 0, 2, 30))
      oldOffset = (uint32_t)sent.data[8] | ((uint32_t)sent.data[9] << 8) |
                  ((uint32_t)sent.data[10] << 16) | ((uint32_t)sent.data[11] << 24);
  }
  WiFiUDP::inject(IPAddress(192, 0, 2, 31), 7701,
                  pong("player_3/chest/group_1"));
  hapbeat_host_now_ms += 20;
  hb.tick();
  uint32_t newOffset = 0;
  begins = 0;
  for (const WiFiUDP::Sent& sent : WiFiUDP::sent) {
    if (isStream(sent, hapbeat::CMD_STREAM_BEGIN)) ++begins;
    if (isStream(sent, hapbeat::CMD_STREAM_DATA) && sent.ip == IPAddress(192, 0, 2, 31)) {
      assert(sent.port == 7701);
      newOffset = (uint32_t)sent.data[8] | ((uint32_t)sent.data[9] << 8) |
                  ((uint32_t)sent.data[10] << 16) | ((uint32_t)sent.data[11] << 24);
    }
  }
  assert(playback->status() == HapbeatPlaybackStatus::Active);
  assert(begins == 1);
  assert(newOffset > oldOffset);
}

int main() {
  test_routing_and_mixing();
  test_deferred_late_pong_and_cooldown();
  test_route_migration_keeps_session();
  return 0;
}
