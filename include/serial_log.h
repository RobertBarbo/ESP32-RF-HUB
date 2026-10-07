#pragma once

#include <Arduino.h>

namespace s11log {

// false = brez aplikacijskih izpisov; true = vsi izpisi za diagnostiko.
// Po spremembi ponovno prevedi in nalozi firmware. ROM izpis ob zagonu ostane.
constexpr bool SERIAL_LOG_ENABLED = false;

template <typename... Args>
inline void print(const Args &...args) {
  if (SERIAL_LOG_ENABLED) Serial.print(args...);
}

template <typename... Args>
inline void println(const Args &...args) {
  if (SERIAL_LOG_ENABLED) Serial.println(args...);
}

template <typename... Args>
inline void printf(const char *format, Args... args) {
  if (SERIAL_LOG_ENABLED) Serial.printf(format, args...);
}

}  // namespace s11log
