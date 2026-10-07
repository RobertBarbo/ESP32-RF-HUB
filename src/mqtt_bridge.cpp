#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <atomic>
#include <esp_timer.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "mqtt_bridge.h"
#include "mqtt_discovery.h"
#include "serial_log.h"
#if __has_include("mqtt_secrets.h")
#include "mqtt_secrets.h"
#else
#include "mqtt_secrets.example.h"
#endif

namespace s11mqtt {
namespace {

constexpr char PRESS_TOPICS[][32] = {
    "tuya_rf_s11/switch1/press", "tuya_rf_s11/switch2/press"};
constexpr char AVAILABILITY_TOPIC[] = "tuya_rf_s11/availability";
constexpr char HA_STATUS_TOPIC[] = "homeassistant/status";
constexpr uint32_t RETRY_MS = 10'000;
constexpr uint32_t EVENT_MAX_AGE_MS = 2'000;
constexpr uint32_t TELEMETRY_MS = 30'000;
constexpr uint32_t METADATA_RETRY_MS = 5'000;

struct PressEvent {
  uint32_t count;
  uint32_t captured_at_ms;
  uint8_t switch_id;
};
enum class ReportKind { Connected, Disconnected, ConnectError, Sent, PublishError,
                        DiscoveryReady, MetadataError, BufferError, SubscribeError, ControlError };
struct Report {
  ReportKind kind;
  uint32_t count;
  uint8_t switch_id;
  int state;
};

QueueHandle_t events = nullptr;
QueueHandle_t reports = nullptr;
QueueHandle_t controls = nullptr;
QueueHandle_t led_states = nullptr;
std::atomic<bool> ready{false};
std::atomic<bool> paused{false};
std::atomic<uint32_t> sent{0};
std::atomic<uint32_t> dropped{0};
std::atomic<uint32_t> reports_dropped{0};
bool configured = false;
uint32_t last_stats_ms = 0;
// Callback in spodnja obdelava teceta samo v MQTT tasku.
bool discovery_requested = false;
char restart_token[24] = {};
void report(ReportKind kind, uint32_t count, uint8_t switch_id, int state);

void on_message(char *topic, uint8_t *payload, unsigned int length) {
  if (strcmp(topic, HA_STATUS_TOPIC) == 0) {
    if (length == 6 && memcmp(payload, "online", 6) == 0) discovery_requested = true;
    return;
  }
  if (paused.load()) return;
  hubcontrol::Command command;
  if (!hubcontrol::parse(topic, payload, length, restart_token, command)) {
    report(ReportKind::ControlError, 0, 0, 1);
  } else if (xQueueSend(controls, &command, 0) != pdTRUE) {
    report(ReportKind::ControlError, 0, 0, 2);
  }
}

bool publish_discovery(PubSubClient &client, const char *device_id, size_t index) {
  char topic[discovery::TOPIC_CAPACITY];
  char payload[discovery::CONFIG_CAPACITY];
  return discovery::format(index, device_id, topic, sizeof(topic), payload, sizeof(payload), restart_token) &&
         client.publish(topic, payload, true);
}

bool publish_telemetry(PubSubClient &client, size_t index) {
  const char *topic = discovery::ENTITIES[index + 2].state_topic;
  if (index == 0) return client.publish(topic, WiFi.localIP().toString().c_str(), false);
  if (index == 1) return client.publish(topic, WiFi.SSID().c_str(), false);
  char value[32];
  if (index == 2) {
    snprintf(value, sizeof(value), "%ld", static_cast<long>(WiFi.RSSI()));
  } else {
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(esp_timer_get_time() / 1'000'000));
  }
  return client.publish(topic, value, false);
}

bool publish_led_state(PubSubClient &client, const hubcontrol::LedSettings &settings, size_t index) {
  char value[24];
  if (index == 0) snprintf(value, sizeof(value), "%s", settings.enabled ? "ON" : "OFF");
  else if (index == 1) snprintf(value, sizeof(value), "%u", static_cast<unsigned>(settings.brightness));
  else snprintf(value, sizeof(value), "%u,%u,%u", static_cast<unsigned>(settings.red),
                static_cast<unsigned>(settings.green), static_cast<unsigned>(settings.blue));
  return client.publish(hubcontrol::STATE_TOPICS[index], value, true);
}

void report(ReportKind kind, uint32_t count = 0, uint8_t switch_id = 0, int state = 0) {
  const Report item = {kind, count, switch_id, state};
  if (xQueueSend(reports, &item, 0) != pdTRUE) ++reports_dropped;
}

void discard_events() {
  PressEvent event;
  while (xQueueReceive(events, &event, 0) == pdTRUE) ++dropped;
}

void mqtt_task(void *) {
  // Samo ta task uporablja TCP in PubSubClient, tudi med OTA.
  WiFiClient tcp;
  tcp.setConnectionTimeout(1000);
  PubSubClient client(tcp);
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setSocketTimeout(2);
  client.setKeepAlive(15);
  client.setCallback(on_message);
  if (!client.setBufferSize(discovery::MQTT_BUFFER_SIZE)) {
    report(ReportKind::BufferError);
    vTaskDelete(nullptr);
    return;
  }
  char client_id[40];
  snprintf(client_id, sizeof(client_id), "tuya-rf-s11-%08lx",
           static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFF));
  uint32_t last_attempt_ms = 0;
  bool attempted = false;
  char device_id[32];
  snprintf(device_id, sizeof(device_id), "tuya_rf_s11_%012llx",
           static_cast<unsigned long long>(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL));
  size_t discovery_index = discovery::ENTITY_COUNT;
  size_t telemetry_index = discovery::TELEMETRY_COUNT;
  uint32_t last_telemetry_ms = 0;
  uint32_t metadata_error_ms = 0;
  bool metadata_failed = false;
  hubcontrol::LedSettings led_settings = hubcontrol::DEFAULT_LED;
  size_t led_state_index = 3;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    hubcontrol::LedSettings newest;
    if (xQueueReceive(led_states, &newest, 0) == pdTRUE) {
      led_settings = newest;
      led_state_index = 0;
    }
    if (paused.load() || WiFi.status() != WL_CONNECTED) {
      if (ready.exchange(false)) report(ReportKind::Disconnected);
      if (client.connected()) {
        client.publish(AVAILABILITY_TOPIC, "offline", true);
        client.disconnect();
      }
      tcp.stop();
      discard_events();
      continue;
    }

    if (!client.connected()) {
      if (ready.exchange(false)) report(ReportKind::Disconnected);
      discard_events();
      const uint32_t now_ms = millis();
      if (attempted && static_cast<uint32_t>(now_ms - last_attempt_ms) < RETRY_MS) continue;
      attempted = true;
      last_attempt_ms = now_ms;
      // QoS0 dogodki niso retained in se po izpadu ne predvajajo ponovno.
      if (!client.connect(client_id, MQTT_USER, MQTT_PASSWORD,
                          AVAILABILITY_TOPIC, 0, true, "offline")) {
        report(ReportKind::ConnectError, 0, 0, client.state());
        continue;
      }
      if (paused.load() || WiFi.status() != WL_CONNECTED ||
          !client.publish(AVAILABILITY_TOPIC, "online", true)) {
        client.disconnect();
        continue;
      }
      ready = true;
      report(ReportKind::Connected);
      if (!client.subscribe(HA_STATUS_TOPIC)) report(ReportKind::SubscribeError, 0, 0, client.state());
      for (const char *topic : hubcontrol::COMMAND_TOPICS) {
        if (!client.subscribe(topic)) report(ReportKind::SubscribeError, 0, 0, client.state());
      }
      discovery_requested = true;
    }

    if (!client.loop()) continue;
    PressEvent event;
    if (xQueueReceive(events, &event, 0) == pdTRUE) {
      if (paused.load() || static_cast<uint32_t>(millis() - event.captured_at_ms) > EVENT_MAX_AGE_MS) {
        ++dropped;
        continue;
      }
      if (client.publish(PRESS_TOPICS[event.switch_id - 1], "PRESS", false)) {
        ++sent;
        report(ReportKind::Sent, event.count, event.switch_id);
      } else {
        // Ne ponavljamo toggle dogodka z neznanim izidom prenosa.
        ++dropped;
        ready = false;
        report(ReportKind::PublishError, event.count, event.switch_id, client.state());
        tcp.stop();
      }
      continue;
    }

    // Pritiski imajo prednost; najvec ena dodatna objava na obhod.
    if (paused.load()) continue;
    if (discovery_requested) {
      discovery_requested = false;
      discovery_index = 0;
      telemetry_index = discovery::TELEMETRY_COUNT;
      metadata_failed = false;
      led_state_index = 0;
    }
    const uint32_t now_ms = millis();
    if (metadata_failed && static_cast<uint32_t>(now_ms - metadata_error_ms) < METADATA_RETRY_MS) continue;
    bool published = true;
    if (led_state_index < 3) {
      published = publish_led_state(client, led_settings, led_state_index);
      if (published) ++led_state_index;
    } else if (discovery_index < discovery::ENTITY_COUNT) {
      published = publish_discovery(client, device_id, discovery_index);
      if (published && ++discovery_index == discovery::ENTITY_COUNT) {
        report(ReportKind::DiscoveryReady);
        telemetry_index = 0;
      }
    } else {
      if (telemetry_index == discovery::TELEMETRY_COUNT &&
          static_cast<uint32_t>(now_ms - last_telemetry_ms) >= TELEMETRY_MS) telemetry_index = 0;
      if (telemetry_index < discovery::TELEMETRY_COUNT) {
        published = publish_telemetry(client, telemetry_index);
        if (published && ++telemetry_index == discovery::TELEMETRY_COUNT) last_telemetry_ms = now_ms;
      }
    }
    metadata_failed = !published;
    if (!published) {
      metadata_error_ms = now_ms;
      report(ReportKind::MetadataError, 0, 0, client.state());
    }
  }
}

}  // namespace

void begin(const hubcontrol::LedSettings &settings) {
  if (MQTT_PASSWORD[0] == '\0') {
    s11log::println("MQTT_NOT_CONFIGURED: vnesi MQTT_PASSWORD v include/mqtt_secrets.h in ponovno prevedi.");
    return;
  }
  events = xQueueCreate(16, sizeof(PressEvent));
  reports = xQueueCreate(32, sizeof(Report));
  controls = xQueueCreate(16, sizeof(hubcontrol::Command));
  led_states = xQueueCreate(1, sizeof(hubcontrol::LedSettings));
  if (events == nullptr || reports == nullptr || controls == nullptr || led_states == nullptr) {
    if (events != nullptr) vQueueDelete(events);
    if (reports != nullptr) vQueueDelete(reports);
    if (controls != nullptr) vQueueDelete(controls);
    if (led_states != nullptr) vQueueDelete(led_states);
    events = reports = controls = led_states = nullptr;
    s11log::println("MQTT_ERROR: ni pomnilnika za vrsti; RF in OTA nadaljujeta.");
    return;
  }
  xQueueOverwrite(led_states, &settings);
  // Novi token ob vsakem zagonu zavrne stare shranjene ukaze za restart.
  snprintf(restart_token, sizeof(restart_token), "RESTART_%08lx", static_cast<unsigned long>(esp_random()));
  if (xTaskCreatePinnedToCore(mqtt_task, "s11_mqtt", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    vQueueDelete(events);
    vQueueDelete(reports);
    vQueueDelete(controls);
    vQueueDelete(led_states);
    events = reports = controls = led_states = nullptr;
    s11log::println("MQTT_ERROR: ni pomnilnika za task; RF in OTA nadaljujeta.");
    return;
  }
  configured = true;
  s11log::printf("MQTT_CONFIG broker=%s port=%u TLS=off; dogodki PRESS brez retained.\n", MQTT_HOST, MQTT_PORT);
}

bool take_control(hubcontrol::Command &command) {
  return controls != nullptr && xQueueReceive(controls, &command, 0) == pdTRUE;
}

void publish_led_settings(const hubcontrol::LedSettings &settings) {
  if (led_states != nullptr) xQueueOverwrite(led_states, &settings);
}

void enqueue_press(uint8_t switch_id, uint32_t count, uint32_t captured_at_ms) {
  if (switch_id < 1 || switch_id > 2 || !ready.load() || paused.load()) {
    ++dropped;
    return;
  }
  const PressEvent event = {count, captured_at_ms, switch_id};
  if (xQueueSend(events, &event, 0) != pdTRUE) ++dropped;
}

void set_paused(bool value) {
  paused = value;
  if (value) ready = false;
}

void service_reports() {
  Report item;
  if (reports != nullptr && xQueueReceive(reports, &item, 0) == pdTRUE) {
    if (item.kind == ReportKind::Connected) {
      s11log::println("MQTT_CONNECTED: pripravljeno; S11_PRESS poslje PRESS v HA.");
    } else if (item.kind == ReportKind::Disconnected) {
      s11log::println("MQTT_DISCONNECTED: RF/LED nadaljujeta; offline pritiski se ne posiljajo pozneje.");
    } else if (item.kind == ReportKind::ConnectError) {
      s11log::printf("MQTT_CONNECT_ERROR state=%d; nov poskus cez 10 s.\n", item.state);
    } else if (item.kind == ReportKind::Sent) {
      s11log::printf("MQTT_SENT switch=%u count=%lu topic=%s payload=PRESS qos=0 retained=no\n",
                    item.switch_id, static_cast<unsigned long>(item.count), PRESS_TOPICS[item.switch_id - 1]);
    } else if (item.kind == ReportKind::DiscoveryReady) {
      s11log::printf("MQTT_DISCOVERY_READY device=S11 RF Hub entities=%u; diagnostika vsakih 30 s.\n",
                    static_cast<unsigned>(discovery::ENTITY_COUNT));
    } else if (item.kind == ReportKind::MetadataError) {
      s11log::printf("MQTT_METADATA_ERROR state=%d; nov poskus cez 5 s, PRESS ima prednost.\n", item.state);
    } else if (item.kind == ReportKind::BufferError) {
      s11log::println("MQTT_ERROR: ni pomnilnika za Discovery medpomnilnik; RF in OTA nadaljujeta.");
    } else if (item.kind == ReportKind::SubscribeError) {
      s11log::printf("MQTT_SUBSCRIBE_ERROR state=%d; retained Discovery ostane na voljo.\n", item.state);
    } else if (item.kind == ReportKind::ControlError) {
      s11log::printf("MQTT_CONTROL_ERROR reason=%s; ukaz ni izveden.\n", item.state == 1 ? "invalid_payload" : "queue_full");
    } else {
      s11log::printf("MQTT_PUBLISH_ERROR switch=%u count=%lu state=%d; brez ponovnega posiljanja.\n",
                    item.switch_id, static_cast<unsigned long>(item.count), item.state);
    }
  }
  const uint32_t now_ms = millis();
  if (configured && static_cast<uint32_t>(now_ms - last_stats_ms) >= 5'000) {
    last_stats_ms = now_ms;
    s11log::printf("MQTT_STATS connected=%s sent=%lu dropped=%lu queued=%u log_dropped=%lu\n",
                  ready.load() ? "yes" : "no", static_cast<unsigned long>(sent.load()),
                  static_cast<unsigned long>(dropped.load()), static_cast<unsigned>(uxQueueMessagesWaiting(events)),
                  static_cast<unsigned long>(reports_dropped.load()));
  }
}

}  // namespace s11mqtt
