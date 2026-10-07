#include "mqtt_discovery.h"
#include <assert.h>
#include <string.h>

int main() {
  char topic[s11mqtt::discovery::TOPIC_CAPACITY];
  char payload[s11mqtt::discovery::CONFIG_CAPACITY];
  const char *device_id = "tuya_rf_s11_0123456789ab";  // Testni, ne dejanski ID.
  const char *restart_token = "RESTART_12345678";
  for (size_t i = 0; i < s11mqtt::discovery::ENTITY_COUNT; ++i) {
    assert(s11mqtt::discovery::format(i, device_id, topic, sizeof(topic), payload, sizeof(payload), restart_token));
    // PubSubClient zahteva prostor tudi za MQTT glavo in dolzino teme.
    assert(5 + 2 + strlen(topic) + strlen(payload) <= s11mqtt::discovery::MQTT_BUFFER_SIZE);
    printf("%s\t%s\n", topic, payload);
  }
  assert(!s11mqtt::discovery::format(s11mqtt::discovery::ENTITY_COUNT, device_id,
                                    topic, sizeof(topic), payload, sizeof(payload), restart_token));
  assert(!s11mqtt::discovery::format(0, device_id, topic, 8, payload, sizeof(payload), restart_token));
  assert(!s11mqtt::discovery::format(0, device_id, topic, sizeof(topic), payload, 8, restart_token));
}
