#pragma once

#include <stddef.h>
#include <stdio.h>
#include "mqtt_bridge.h"

namespace s11mqtt {
namespace discovery {

constexpr size_t MQTT_BUFFER_SIZE = 1536;
constexpr size_t CONFIG_CAPACITY = 1280;
constexpr size_t TOPIC_CAPACITY = 128;

struct Entity {
  const char *component;
  const char *key;
  const char *name;
  const char *state_topic;
  const char *extra;
};

// Predloga pretvori obstojeci PRESS v JSON samo za MQTT event entiteto.
constexpr char EVENT_OPTIONS[] =
    ",\"device_class\":\"button\",\"event_types\":[\"press\"],"
    "\"value_template\":\"{% if value == 'PRESS' %}{\\\"event_type\\\":\\\"press\\\"}{% else %}{}{% endif %}\"";
constexpr char TEXT_OPTIONS[] = ",\"entity_category\":\"diagnostic\",\"expire_after\":90";
constexpr char RSSI_OPTIONS[] =
    ",\"entity_category\":\"diagnostic\",\"expire_after\":90,"
    "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\"";
constexpr char UPTIME_OPTIONS[] =
    ",\"entity_category\":\"diagnostic\",\"expire_after\":90,"
    "\"device_class\":null,\"unit_of_measurement\":null,\"icon\":\"mdi:clock-outline\","
    "\"value_template\":\"{% set t = value | int(0) %}{{ t // 86400 }} d "
    "{{ '%02d' | format((t // 3600) % 24) }} h {{ '%02d' | format((t // 60) % 60) }} min\"";
constexpr char LIGHT_OPTIONS[] =
    ",\"entity_category\":\"config\",\"command_topic\":\"tuya_rf_s11/led/set\","
    "\"brightness_command_topic\":\"tuya_rf_s11/led/brightness/set\","
    "\"brightness_state_topic\":\"tuya_rf_s11/led/brightness/state\","
    "\"rgb_command_topic\":\"tuya_rf_s11/led/rgb/set\",\"rgb_state_topic\":\"tuya_rf_s11/led/rgb/state\","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"brightness_scale\":255,"
    "\"on_command_type\":\"last\",\"optimistic\":false,\"retain\":false";
constexpr char RESTART_OPTIONS[] =
    ",\"entity_category\":\"config\",\"device_class\":\"restart\","
    "\"command_topic\":\"tuya_rf_s11/restart/set\",\"retain\":false";

constexpr Entity ENTITIES[] = {
    {"event", "switch1", "Stikalo 1", "tuya_rf_s11/switch1/press", EVENT_OPTIONS},
    {"event", "switch2", "Stikalo 2", "tuya_rf_s11/switch2/press", EVENT_OPTIONS},
    {"sensor", "ip", "IP naslov", "tuya_rf_s11/diagnostics/ip", TEXT_OPTIONS},
    {"sensor", "ssid", "Wi-Fi SSID", "tuya_rf_s11/diagnostics/ssid", TEXT_OPTIONS},
    {"sensor", "rssi", "Wi-Fi signal", "tuya_rf_s11/diagnostics/rssi", RSSI_OPTIONS},
    {"sensor", "uptime", "Cas delovanja", "tuya_rf_s11/diagnostics/uptime", UPTIME_OPTIONS},
    {"light", "rgb_led", "RGB indikator", hubcontrol::STATE_TOPICS[0], LIGHT_OPTIONS},
    {"button", "restart", "Ponovni zagon ESP", nullptr, RESTART_OPTIONS}};
constexpr size_t ENTITY_COUNT = sizeof(ENTITIES) / sizeof(ENTITIES[0]);
constexpr size_t TELEMETRY_COUNT = 4;

// device_id je interni alfanumericni identifikator iz eFuse MAC.
inline bool format(size_t index, const char *device_id, char *topic, size_t topic_size,
                   char *payload, size_t payload_size, const char *restart_token) {
  if (index >= ENTITY_COUNT) return false;
  const Entity &entity = ENTITIES[index];
  char state_field[128] = {};
  char restart_field[96] = {};
  if (entity.state_topic != nullptr) {
    const int length = snprintf(state_field, sizeof(state_field), ",\"state_topic\":\"%s\"", entity.state_topic);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(state_field)) return false;
  }
  if (entity.state_topic == nullptr) {
    const int length = snprintf(restart_field, sizeof(restart_field), ",\"payload_press\":\"%s\"", restart_token);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(restart_field)) return false;
  }
  const int topic_length = snprintf(topic, topic_size, "homeassistant/%s/%s_%s/config",
                                    entity.component, device_id, entity.key);
  const int payload_length = snprintf(payload, payload_size,
      "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
      "\"availability_topic\":\"tuya_rf_s11/availability\","
      "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
      "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"S11 RF Hub\","
      "\"model\":\"ESP32-S3 N16R8 + RX500A\",\"sw_version\":\"%s\"}%s%s%s}",
      entity.name, device_id, entity.key, device_id, FIRMWARE_VERSION, state_field, entity.extra, restart_field);
  return topic_length >= 0 && static_cast<size_t>(topic_length) < topic_size &&
         payload_length >= 0 && static_cast<size_t>(payload_length) < payload_size;
}

}  // namespace discovery
}  // namespace s11mqtt
