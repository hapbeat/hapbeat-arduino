#ifndef HAPBEAT_HOST_WIFI_UDP_H
#define HAPBEAT_HOST_WIFI_UDP_H

#include <deque>
#include <vector>

#include "Arduino.h"

class WiFiUDP {
 public:
  struct Sent {
    IPAddress ip;
    uint16_t port;
    std::vector<uint8_t> data;
  };
  struct Inbound {
    IPAddress ip;
    uint16_t port;
    std::vector<uint8_t> data;
  };

  static std::vector<Sent> sent;
  static std::deque<Inbound> inbound;

  bool begin(uint16_t) { return true; }
  int parsePacket() {
    if (_rxActive || inbound.empty()) return _rxActive ? (int)inbound.front().data.size() : 0;
    _rxActive = true;
    return (int)inbound.front().data.size();
  }
  int read(uint8_t* out, size_t length) {
    if (!_rxActive || inbound.empty()) return 0;
    const size_t count = length < inbound.front().data.size() ? length : inbound.front().data.size();
    for (size_t i = 0; i < count; ++i) out[i] = inbound.front().data[i];
    return (int)count;
  }
  void flush() {
    if (_rxActive && !inbound.empty()) inbound.pop_front();
    _rxActive = false;
  }
  IPAddress remoteIP() const { return inbound.empty() ? IPAddress((uint32_t)0) : inbound.front().ip; }
  uint16_t remotePort() const { return inbound.empty() ? 0 : inbound.front().port; }

  int beginPacket(IPAddress ip, uint16_t port) {
    _txIp = ip;
    _txPort = port;
    _tx.clear();
    return 1;
  }
  size_t write(const uint8_t* data, size_t length) {
    _tx.insert(_tx.end(), data, data + length);
    return length;
  }
  int endPacket() {
    sent.push_back({_txIp, _txPort, _tx});
    return 1;
  }

  static void clear() {
    sent.clear();
    inbound.clear();
  }
  static void inject(IPAddress ip, uint16_t port, const std::vector<uint8_t>& data) {
    inbound.push_back({ip, port, data});
  }

 private:
  bool _rxActive = false;
  IPAddress _txIp;
  uint16_t _txPort = 0;
  std::vector<uint8_t> _tx;
};

#endif
