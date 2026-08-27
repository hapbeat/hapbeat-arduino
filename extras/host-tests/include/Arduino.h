#ifndef HAPBEAT_HOST_ARDUINO_H
#define HAPBEAT_HOST_ARDUINO_H

#include <stddef.h>
#include <stdint.h>

extern uint32_t hapbeat_host_now_ms;

inline uint32_t millis() { return hapbeat_host_now_ms; }
inline void delay(uint32_t value) { hapbeat_host_now_ms += value; }

#ifndef PI
#define PI 3.14159265358979323846f
#endif

class IPAddress {
 public:
  IPAddress() : _value(0) {}
  explicit IPAddress(uint32_t value) : _value(value) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : _value(((uint32_t)a << 24) | ((uint32_t)b << 16) |
               ((uint32_t)c << 8) | (uint32_t)d) {}

  operator uint32_t() const { return _value; }
  bool operator==(const IPAddress& other) const { return _value == other._value; }
  bool operator!=(const IPAddress& other) const { return !(*this == other); }

 private:
  uint32_t _value;
};

#endif
