#ifndef HAPBEAT_HOST_ARDUINO_H
#define HAPBEAT_HOST_ARDUINO_H

#include <stddef.h>
#include <stdint.h>

extern uint32_t hapbeat_host_now_ms;

inline uint32_t millis() { return hapbeat_host_now_ms; }
inline uint32_t micros() { return hapbeat_host_now_ms * 1000u; }
inline void delay(uint32_t value) { hapbeat_host_now_ms += value; }

// Deterministic stand-ins for the Arduino RNG (client incarnations).
extern uint32_t hapbeat_host_random_state;
inline void randomSeed(unsigned long seed) {
  hapbeat_host_random_state = (uint32_t)seed | 1u;
}
inline long random(long howbig) {
  hapbeat_host_random_state = hapbeat_host_random_state * 1664525u + 1013904223u;
  return howbig <= 0 ? 0 : (long)((hapbeat_host_random_state >> 8) % (uint32_t)howbig);
}

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
  uint8_t operator[](int index) const {
    return (uint8_t)(_value >> (8 * (3 - index)));
  }
  bool operator==(const IPAddress& other) const { return _value == other._value; }
  bool operator!=(const IPAddress& other) const { return !(*this == other); }

 private:
  uint32_t _value;
};

#endif
