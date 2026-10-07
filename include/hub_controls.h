#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace hubcontrol {

constexpr char POWER_COMMAND[] = "tuya_rf_s11/led/set";
constexpr char BRIGHTNESS_COMMAND[] = "tuya_rf_s11/led/brightness/set";
constexpr char COLOR_COMMAND[] = "tuya_rf_s11/led/rgb/set";
constexpr char RESTART_COMMAND[] = "tuya_rf_s11/restart/set";
constexpr const char *COMMAND_TOPICS[] = {POWER_COMMAND, BRIGHTNESS_COMMAND, COLOR_COMMAND, RESTART_COMMAND};
constexpr char STATE_TOPICS[][40] = {
    "tuya_rf_s11/led/state", "tuya_rf_s11/led/brightness/state", "tuya_rf_s11/led/rgb/state"};

struct LedSettings {
  bool enabled;
  uint8_t brightness;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};
constexpr LedSettings DEFAULT_LED = {true, 16, 255, 255, 255};

enum class Kind { Power, Brightness, Color, Restart };
struct Command {
  Kind kind;
  uint8_t first;
  uint8_t second;
  uint8_t third;
};
struct Color { uint8_t red, green, blue; };

inline bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
inline bool byte_value(const char *&cursor, uint8_t &value) {
  while (space(*cursor)) ++cursor;
  if (*cursor < '0' || *cursor > '9') return false;
  unsigned number = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    number = number * 10 + (*cursor++ - '0');
    if (number > 255) return false;
  }
  while (space(*cursor)) ++cursor;
  value = static_cast<uint8_t>(number);
  return true;
}

// Payload PubSubClient ni nujno zakljucen z niclo. Najprej ga omejeno kopiramo.
inline bool parse(const char *topic, const uint8_t *payload, size_t length,
                  const char *restart_token, Command &command) {
  if (length == 0 || length >= 40 || memchr(payload, '\0', length) != nullptr) return false;
  char text[40];
  memcpy(text, payload, length);
  text[length] = '\0';
  Command parsed = {};
  const char *cursor = text;
  if (strcmp(topic, POWER_COMMAND) == 0) {
    parsed.kind = Kind::Power;
    if (strcmp(text, "ON") == 0) parsed.first = 1;
    else if (strcmp(text, "OFF") != 0) return false;
  } else if (strcmp(topic, BRIGHTNESS_COMMAND) == 0) {
    parsed.kind = Kind::Brightness;
    if (!byte_value(cursor, parsed.first) || *cursor != '\0') return false;
  } else if (strcmp(topic, COLOR_COMMAND) == 0) {
    parsed.kind = Kind::Color;
    if (!byte_value(cursor, parsed.first) || *cursor++ != ',' ||
        !byte_value(cursor, parsed.second) || *cursor++ != ',' ||
        !byte_value(cursor, parsed.third) || *cursor != '\0') return false;
  } else if (strcmp(topic, RESTART_COMMAND) == 0) {
    if (strcmp(text, restart_token) != 0) return false;
    parsed.kind = Kind::Restart;
  } else {
    return false;
  }
  command = parsed;
  return true;
}

inline bool equal(const LedSettings &a, const LedSettings &b) {
  return a.enabled == b.enabled && a.brightness == b.brightness &&
         a.red == b.red && a.green == b.green && a.blue == b.blue;
}

inline bool apply(LedSettings &settings, const Command &command) {
  const LedSettings previous = settings;
  if (command.kind == Kind::Power) {
    settings.enabled = command.first != 0;
    if (settings.enabled && settings.brightness == 0) settings.brightness = DEFAULT_LED.brightness;
  } else if (command.kind == Kind::Brightness) {
    settings.brightness = command.first;
    if (settings.brightness == 0) settings.enabled = false;
  } else if (command.kind == Kind::Color) {
    settings.red = command.first;
    settings.green = command.second;
    settings.blue = command.third;
  }
  return !equal(previous, settings);
}

inline uint32_t pack(const LedSettings &settings) {
  return (static_cast<uint32_t>(settings.brightness) << 24) |
         (static_cast<uint32_t>(settings.red) << 16) |
         (static_cast<uint32_t>(settings.green) << 8) | settings.blue;
}
inline LedSettings unpack(uint32_t value, bool enabled) {
  return {enabled, static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
          static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
}
inline Color output(const LedSettings &settings, bool indicator_on) {
  if (!settings.enabled || !indicator_on) return {0, 0, 0};
  return {static_cast<uint8_t>((settings.red * settings.brightness + 127) / 255),
          static_cast<uint8_t>((settings.green * settings.brightness + 127) / 255),
          static_cast<uint8_t>((settings.blue * settings.brightness + 127) / 255)};
}

}  // namespace hubcontrol
