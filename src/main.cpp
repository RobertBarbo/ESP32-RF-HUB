#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp32-hal-psram.h>
#include <driver/rmt_common.h>
#include <driver/rmt_rx.h>
#include <driver/gpio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "s11_detector.h"
#include "mqtt_bridge.h"
#include "serial_log.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace {

// RX500A DATA je fizično priključen na ta GPIO.
constexpr gpio_num_t RF_DATA_PIN = GPIO_NUM_4;
// Uporabnikova vgrajena naslovljiva RGB LED na GPIO48.
constexpr uint8_t TEST_LED_PIN = 48;
constexpr char WIFI_HOSTNAME[] = "tuya-rf-s11";
constexpr uint32_t WIFI_RETRY_MS = 30'000;

// Ena RMT enota ustreza eni mikrosekundi, zato se trajanja izpišejo neposredno v us.
constexpr uint32_t RMT_RESOLUTION_HZ = 1'000'000;

// Ti dve meji določata, kaj šteje kot šum in kaj zaključi RF paket.
// Gonilnik na tej napravi dovoljuje filter < 3187 ns, ne glede na ločljivost zajema.
constexpr uint32_t GLITCH_FILTER_NS = 1'000;    // 1 us
constexpr uint32_t PACKET_GAP_NS = 15'000'000;  // 15 ms
constexpr size_t RX_SYMBOL_CAPACITY = 256;
constexpr bool RAW_OUTPUT = false;  // Celotni bližnji zajem je analiziran; brez poplave ozadja.
constexpr uint32_t RF_STATS_INTERVAL_MS = 5'000;
constexpr uint32_t RX_NO_CALLBACK_MS = 2'000;
// Obnova samo ob daljsi opazeni aktivnosti brez zakljucka, ne zaradi tisine.
constexpr uint32_t RX_RECOVERY_MIN_CHANGES = 10;
constexpr uint32_t RAW_WINDOW_MS = 20'000;
constexpr size_t RAW_PACKET_CAPACITY = 512;

enum class ReportKind { Ready, Press, Repeat, Stats, Error };
struct RfReport {
  ReportKind kind;
  uint32_t uptime_ms;
  uint32_t presses;
  uint32_t captures;
  uint32_t unmatched;
  uint32_t full;
  uint32_t compact;
  uint32_t flex;
  uint32_t merged;
  uint32_t capacity_reached;
  uint32_t rearm_max_ms;
  uint32_t log_dropped;
  uint32_t raw_lock_misses;
  uint32_t callbacks;
  uint32_t rx_restarts;
  uint32_t gpio4_changes;
  int gpio4;
  uint16_t packet_full;
  uint16_t packet_compact;
  uint16_t packet_flex;
  uint8_t switch_id;
  bool led_on;
  esp_err_t error;
};
TaskHandle_t rf_task_handle = nullptr;
QueueHandle_t rf_reports = nullptr;
SemaphoreHandle_t raw_mutex = nullptr;

struct RawCapture {
  uint32_t uptime_ms;
  uint16_t symbol_count;
  uint16_t full_matches;
  uint16_t compact_matches;
  uint16_t flex_matches;
  uint16_t switch1_matches;
  uint16_t switch2_matches;
  bool new_press;
  bool is_last;
  rmt_symbol_word_t symbols[RX_SYMBOL_CAPACITY];
};

rmt_channel_handle_t rf_receiver = nullptr;
rmt_symbol_word_t rx_symbols[RX_SYMBOL_CAPACITY];
rmt_symbol_word_t captured_symbols[RX_SYMBOL_CAPACITY];
s11::Pulse captured_pulses[RX_SYMBOL_CAPACITY * 2];
s11::PressGate press_gates[2];
uint32_t press_count = 0;
uint32_t capture_count = 0;
uint32_t unmatched_capture_count = 0;
uint32_t full_match_count = 0;
uint32_t compact_match_count = 0;
uint32_t flex_match_count = 0;
uint32_t merged_capture_count = 0;
uint32_t capacity_reached_count = 0;
uint32_t last_rf_stats_ms = 0;
uint32_t rearm_max_ms = 0;
uint32_t log_dropped_count = 0;
uint32_t raw_lock_misses = 0;
uint32_t rx_restarts = 0;
uint32_t last_rx_progress_ms = 0;
uint32_t gpio4_changes = 0;
uint32_t gpio4_changes_at_arm = 0;
int last_gpio4_level = -1;
bool test_led_on = false;
std::atomic<bool> requested_led_on{false};
hubcontrol::LedSettings led_settings = hubcontrol::DEFAULT_LED;
Preferences led_preferences;
bool led_preferences_ready = false;
bool led_save_pending = false;
uint32_t led_changed_ms = 0;
bool restart_pending = false;
uint32_t restart_at_ms = 0;
bool wifi_configured = false;
bool ota_started = false;
std::atomic<bool> ota_updating{false};
uint32_t last_wifi_retry_ms = 0;
volatile size_t received_symbol_count = 0;
volatile bool capture_ready = false;
volatile bool capture_is_last = true;
volatile uint32_t received_at_ms = 0;
volatile uint32_t receive_callback_count = 0;
RawCapture *raw_captures = nullptr;
size_t raw_capture_count = 0;
size_t raw_dump_index = 0;
uint32_t raw_started_ms = 0;
bool raw_recording = false;
bool raw_dumping = false;
bool raw_report_pending = false;
const char *raw_finish_reason = nullptr;
uint32_t raw_elapsed_ms = 0;

void release_raw_capture() {
  xSemaphoreTake(raw_mutex, portMAX_DELAY);
  RawCapture *released = raw_captures;
  raw_recording = false;
  raw_dumping = false;
  raw_captures = nullptr;
  raw_capture_count = 0;
  raw_dump_index = 0;
  raw_report_pending = false;
  xSemaphoreGive(raw_mutex);
  free(released);
}

void setup_network() {
  if (WIFI_PASSWORD[0] == '\0') {
    s11log::println("WIFI_NOT_CONFIGURED: vnesi geslo v include/secrets.h in ponovno nalozi firmware.");
    return;
  }

  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.onStart([]() {
    ota_updating = true;
    s11mqtt::set_paused(true);
    if (raw_captures != nullptr) {
      release_raw_capture();
      s11log::println("RAW_CANCELLED: OTA ima prednost.");
    }
    s11log::println("OTA_START: RF dogodki med posodobitvijo niso obdelani.");
  });
  ArduinoOTA.onEnd([]() {
    s11log::println("OTA_END: firmware prejet, sledi ponovni zagon.");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ota_updating = false;
    s11mqtt::set_paused(false);
    s11log::printf("OTA_ERROR code=%u\n", static_cast<unsigned>(error));
  });

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Hub je stalno napajan: prednost ima odzivnost pred modem-sleep varcevanjem.
  if (!WiFi.setSleep(false)) {
    s11log::println("WIFI_SLEEP_ERROR: izklop varcevanja ni uspel.");
  }
  wifi_configured = true;
  last_wifi_retry_ms = millis();
  s11log::println("WIFI_CONNECTING: BarboNet, RF test deluje tudi brez povezave.");
}

void service_network() {
  if (!wifi_configured) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!ota_started) {
      ArduinoOTA.begin();
      ota_started = true;
      s11log::print("WIFI_CONNECTED ip=");
      s11log::println(WiFi.localIP());
      wifi_ps_type_t power_save;
      const esp_err_t ps_error = esp_wifi_get_ps(&power_save);
      if (ps_error == ESP_OK) {
        s11log::printf("WIFI_LINK rssi_dbm=%ld power_save=%s ps_mode=%d\n",
                      static_cast<long>(WiFi.RSSI()), power_save == WIFI_PS_NONE ? "off" : "on",
                      static_cast<int>(power_save));
      } else {
        s11log::printf("WIFI_LINK_ERROR: %s (%d)\n", esp_err_to_name(ps_error), ps_error);
      }
      s11log::println("OTA_READY host=tuya-rf-s11.local port=3232 env=esp32s3_ota");
    }
    ArduinoOTA.handle();
  } else {
    if (ota_started) {
      ArduinoOTA.end();
      ota_started = false;
      s11log::println("WIFI_DISCONNECTED: RF test nadaljuje, OTA caka na povezavo.");
    }
    const uint32_t now_ms = millis();
    if (static_cast<uint32_t>(now_ms - last_wifi_retry_ms) >= WIFI_RETRY_MS) {
      last_wifi_retry_ms = now_ms;
      WiFi.reconnect();
      s11log::println("WIFI_RETRY");
    }
  }
}

bool IRAM_ATTR on_receive_done(
    rmt_channel_handle_t,
    const rmt_rx_done_event_data_t *event,
    void *) {
  receive_callback_count = receive_callback_count + 1;
  received_symbol_count = event->num_symbols;
  capture_is_last = event->flags.is_last;
  received_at_ms = millis();
  capture_ready = true;
  BaseType_t task_woken = pdFALSE;
  if (rf_task_handle != nullptr) vTaskNotifyGiveFromISR(rf_task_handle, &task_woken);
  return task_woken == pdTRUE;
}

void fail_if_error(esp_err_t error, const char *operation) {
  if (error == ESP_OK) {
    return;
  }

  s11log::printf("ERROR %s: %s (%d)\n", operation, esp_err_to_name(error), error);
  while (true) {
    delay(1000);
  }
}

esp_err_t arm_capture() {
  capture_ready = false;
  received_symbol_count = 0;
  capture_is_last = true;

  rmt_receive_config_t receive_config = {};
  receive_config.signal_range_min_ns = GLITCH_FILTER_NS;
  receive_config.signal_range_max_ns = PACKET_GAP_NS;
  receive_config.flags.en_partial_rx = 0;

  const esp_err_t error = rmt_receive(rf_receiver, rx_symbols, sizeof(rx_symbols), &receive_config);
  if (error == ESP_OK) {
    last_rx_progress_ms = millis();
    gpio4_changes_at_arm = gpio4_changes;
  }
  return error;
}

esp_err_t setup_rf_receiver() {
  rmt_rx_channel_config_t channel_config = {};
  channel_config.gpio_num = RF_DATA_PIN;
  channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
  channel_config.resolution_hz = RMT_RESOLUTION_HZ;
  channel_config.mem_block_symbols = RX_SYMBOL_CAPACITY;
  channel_config.intr_priority = 0;
  channel_config.flags.invert_in = 0;
  // Brez DMA bi 256 simbolov zahtevalo 6 blokov; ESP32-S3 ima le 4 RX bloke po 48.
  channel_config.flags.with_dma = 1;
  channel_config.flags.io_loop_back = 0;
  channel_config.flags.allow_pd = 0;

  esp_err_t error = rmt_new_rx_channel(&channel_config, &rf_receiver);
  if (error != ESP_OK) return error;

  rmt_rx_event_callbacks_t callbacks = {};
  callbacks.on_recv_done = on_receive_done;
  error = rmt_rx_register_event_callbacks(rf_receiver, &callbacks, nullptr);
  if (error != ESP_OK) return error;
  error = rmt_enable(rf_receiver);
  if (error != ESP_OK) return error;
  return arm_capture();
}

void print_capture(const rmt_symbol_word_t *symbols, size_t symbol_count, bool is_last) {
  s11log::printf("\nRAW_PACKET symbols=%u complete=%s\n",
                static_cast<unsigned>(symbol_count),
                is_last ? "yes" : "no");

  for (size_t index = 0; index < symbol_count; ++index) {
    const rmt_symbol_word_t &symbol = symbols[index];
    if (symbol.duration0 == 0) break;
    s11log::printf("%u:%u %u:%u%s",
                  static_cast<unsigned>(symbol.level0),
                  static_cast<unsigned>(symbol.duration0),
                  static_cast<unsigned>(symbol.level1),
                  static_cast<unsigned>(symbol.duration1),
                  ((index + 1) % 8 == 0 || index + 1 == symbol_count) ? "\n" : "  ");
    if (symbol.duration1 == 0) break;
  }

  s11log::println();

  if (!is_last) {
    s11log::println("WARNING: zajem ni celoten; povecaj RX_SYMBOL_CAPACITY pred sklepanjem o paketu.");
  }
  s11log::println("END_RAW_PACKET");
}

void finish_raw_recording(const char *reason) {
  // Volano samo pod raw_mutex; tukaj ni serijskega izpisa.
  raw_recording = false;
  raw_dumping = true;
  raw_dump_index = 0;
  raw_elapsed_ms = millis() - raw_started_ms;
  raw_finish_reason = reason;
  raw_report_pending = true;
}

void service_raw_capture() {
  if (ota_updating) return;
  // Najvec en ukaz na krog, da serijski vhod ne zadrzi RF obdelave.
  if (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == 'r' || command == 'R') {
      xSemaphoreTake(raw_mutex, portMAX_DELAY);
      const bool busy = raw_captures != nullptr;
      xSemaphoreGive(raw_mutex);
      if (busy) {
        s11log::println("RAW_BUSY: trenutni zajem ali izpis se se izvaja.");
      } else if (!psramFound()) {
        s11log::println("RAW_ERROR: PSRAM ni na voljo; zajem ni zagnan.");
      } else {
        RawCapture *allocated = static_cast<RawCapture *>(ps_malloc(sizeof(RawCapture) * RAW_PACKET_CAPACITY));
        if (allocated == nullptr) {
          s11log::println("RAW_ERROR: ni dovolj PSRAM; zajem ni zagnan.");
        } else {
          xSemaphoreTake(raw_mutex, portMAX_DELAY);
          raw_captures = allocated;
          raw_capture_count = 0;
          raw_started_ms = millis();
          raw_recording = true;
          xSemaphoreGive(raw_mutex);
          s11log::println("RAW_START duration_ms=20000 max_packets=512; pritiskaj po dogovorjenem testu.");
        }
      }
    } else if (command == 's' || command == 'S') {
      xSemaphoreTake(raw_mutex, portMAX_DELAY);
      if (raw_recording) finish_raw_recording("user_stop");
      xSemaphoreGive(raw_mutex);
    }
  }
  xSemaphoreTake(raw_mutex, portMAX_DELAY);
  if (raw_recording && static_cast<uint32_t>(millis() - raw_started_ms) >= RAW_WINDOW_MS) {
    finish_raw_recording("time_limit");
  }
  const bool report_recorded = raw_report_pending;
  const size_t count = raw_capture_count;
  const uint32_t elapsed_ms = raw_elapsed_ms;
  const char *reason = raw_finish_reason;
  raw_report_pending = false;
  RawCapture *dump = nullptr;
  const bool dump_done = raw_dumping && raw_dump_index == raw_capture_count;
  if (raw_dumping && !dump_done) dump = &raw_captures[raw_dump_index++];
  xSemaphoreGive(raw_mutex);

  // Izpis in alokacija nikoli ne drzita mutexa, ki ga uporablja RF task.
  if (report_recorded) {
    s11log::printf("RAW_RECORDED packets=%u elapsed_ms=%lu reason=%s; sledi izpis, med njim ne pritiskaj stikal.\n",
                  static_cast<unsigned>(count), static_cast<unsigned long>(elapsed_ms), reason);
  }
  if (dump != nullptr) {
    const RawCapture &capture = *dump;
    s11log::printf("CAPTURE uptime_ms=%lu matches=%u full=%u compact=%u flex=%u event=%s capacity_reached=%s s1_matches=%u s2_matches=%u\n",
                    static_cast<unsigned long>(capture.uptime_ms),
                    static_cast<unsigned>(capture.full_matches + capture.compact_matches + capture.flex_matches),
                    static_cast<unsigned>(capture.full_matches),
                    static_cast<unsigned>(capture.compact_matches),
                    static_cast<unsigned>(capture.flex_matches),
                    capture.new_press ? "yes" : "no",
                    capture.symbol_count == RX_SYMBOL_CAPACITY ? "yes" : "no",
                    static_cast<unsigned>(capture.switch1_matches), static_cast<unsigned>(capture.switch2_matches));
    print_capture(capture.symbols, capture.symbol_count, capture.is_last);
  } else if (dump_done) {
    s11log::printf("RAW_DONE packets=%u; povratek v tiho diagnostiko.\n",
                  static_cast<unsigned>(count));
    release_raw_capture();
  }
}

void publish_report(ReportKind kind, uint32_t uptime_ms,
                    size_t packet_full = 0, size_t packet_compact = 0, esp_err_t error = ESP_OK, uint8_t switch_id = 0, size_t packet_flex = 0) {
  const RfReport report = {
      kind, uptime_ms, press_count, capture_count, unmatched_capture_count,
      full_match_count, compact_match_count, flex_match_count, merged_capture_count, capacity_reached_count,
      rearm_max_ms, log_dropped_count, raw_lock_misses,
      receive_callback_count, rx_restarts, gpio4_changes, gpio_get_level(RF_DATA_PIN),
      static_cast<uint16_t>(packet_full), static_cast<uint16_t>(packet_compact), static_cast<uint16_t>(packet_flex), switch_id,
      test_led_on, error};
  // Plna vrsta zavrze le izpis; LED in naslednji RF zajem ne cakata.
  if (xQueueSend(rf_reports, &report, 0) != pdTRUE) ++log_dropped_count;
}

void service_rf_reports() {
  RfReport report;
  if (xQueueReceive(rf_reports, &report, 0) != pdTRUE) return;
  if (report.kind == ReportKind::Ready) {
    s11log::printf("RF_READY core=%d gpio4=%d; pritisni S11.\n", xPortGetCoreID(), report.gpio4);
  } else if (report.kind == ReportKind::Press) {
    s11log::printf("S11_PRESS count=%lu matches=%u full=%u compact=%u flex=%u led=%s led_gpio=%u uptime_ms=%lu switch=%u\n",
                  static_cast<unsigned long>(report.presses),
                  static_cast<unsigned>(report.packet_full + report.packet_compact + report.packet_flex),
                  static_cast<unsigned>(report.packet_full), static_cast<unsigned>(report.packet_compact),
                  static_cast<unsigned>(report.packet_flex),
                  report.led_on ? "on" : "off", static_cast<unsigned>(TEST_LED_PIN), static_cast<unsigned long>(report.uptime_ms),
                  static_cast<unsigned>(report.switch_id));
  } else if (report.kind == ReportKind::Repeat) {
    s11log::printf("S11_REPEAT matches=%u full=%u compact=%u flex=%u uptime_ms=%lu switch=%u\n",
                  static_cast<unsigned>(report.packet_full + report.packet_compact + report.packet_flex),
                  static_cast<unsigned>(report.packet_full), static_cast<unsigned>(report.packet_compact),
                  static_cast<unsigned>(report.packet_flex),
                  static_cast<unsigned long>(report.uptime_ms), static_cast<unsigned>(report.switch_id));
  } else if (report.kind == ReportKind::Error) {
    s11log::printf("ERROR RF task: %s (%d); OTA ostaja na voljo.\n", esp_err_to_name(report.error), report.error);
  } else {
    s11log::printf("RF_STATS uptime_ms=%lu captures=%lu unmatched=%lu full=%lu compact=%lu flex=%lu presses=%lu merged=%lu capacity_reached=%lu rearm_max_ms=%lu log_dropped=%lu raw_lock_misses=%lu callbacks=%lu rx_restarts=%lu gpio4_changes=%lu gpio4=%d\n",
                  static_cast<unsigned long>(report.uptime_ms), static_cast<unsigned long>(report.captures),
                  static_cast<unsigned long>(report.unmatched), static_cast<unsigned long>(report.full),
                  static_cast<unsigned long>(report.compact), static_cast<unsigned long>(report.flex),
                  static_cast<unsigned long>(report.presses),
                  static_cast<unsigned long>(report.merged), static_cast<unsigned long>(report.capacity_reached),
                  static_cast<unsigned long>(report.rearm_max_ms), static_cast<unsigned long>(report.log_dropped),
                  static_cast<unsigned long>(report.raw_lock_misses),
                  static_cast<unsigned long>(report.callbacks), static_cast<unsigned long>(report.rx_restarts),
                  static_cast<unsigned long>(report.gpio4_changes), report.gpio4);
  }
}

void rf_task(void *) {
  // Celoten RMT zagon in vsi nadaljnji klici imajo istega lastnika/core.
  rf_task_handle = xTaskGetCurrentTaskHandle();
  const esp_err_t startup_error = setup_rf_receiver();
  if (startup_error != ESP_OK) {
    publish_report(ReportKind::Error, millis(), 0, 0, startup_error);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  publish_report(ReportKind::Ready, millis());
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    if (ota_updating.load()) continue;
    // Vzorcenje ni stevec vseh RF robov; locuje staticen vhod od opazenih sprememb.
    const int gpio4_level = gpio_get_level(RF_DATA_PIN);
    if (last_gpio4_level >= 0 && gpio4_level != last_gpio4_level) ++gpio4_changes;
    last_gpio4_level = gpio4_level;
    if (capture_ready) {
      const size_t symbol_count = received_symbol_count;
      const bool is_last = capture_is_last;
      const uint32_t captured_at_ms = received_at_ms;
      if (symbol_count > RX_SYMBOL_CAPACITY) {
        publish_report(ReportKind::Error, millis(), 0, 0, ESP_ERR_INVALID_SIZE);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
      }
      memcpy(captured_symbols, rx_symbols, symbol_count * sizeof(rx_symbols[0]));
      const esp_err_t error = arm_capture();
      if (error != ESP_OK) {
        publish_report(ReportKind::Error, millis(), 0, 0, error);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
      }
      const uint32_t rearm_delay_ms = millis() - captured_at_ms;
      if (rearm_delay_ms > rearm_max_ms) rearm_max_ms = rearm_delay_ms;

      size_t pulse_count = 0;
      for (size_t i = 0; i < symbol_count; ++i) {
        const rmt_symbol_word_t &symbol = captured_symbols[i];
        if (symbol.duration0 == 0) break;
        captured_pulses[pulse_count++] = {
            static_cast<uint16_t>(symbol.duration0), static_cast<uint8_t>(symbol.level0)};
        if (symbol.duration1 == 0) break;
        captured_pulses[pulse_count++] = {
            static_cast<uint16_t>(symbol.duration1), static_cast<uint8_t>(symbol.level1)};
      }
      const s11::MatchCounts by_switch[2] = {
          s11::count_robust_matches(captured_pulses, pulse_count, 0),
          s11::count_robust_matches(captured_pulses, pulse_count, 1)};
      const size_t full_by_switch[2] = {by_switch[0].full, by_switch[1].full};
      const size_t compact_by_switch[2] = {by_switch[0].compact, by_switch[1].compact};
      const size_t flex_by_switch[2] = {by_switch[0].flex, by_switch[1].flex};
      const size_t full_matches = full_by_switch[0] + full_by_switch[1];
      const size_t compact_matches = compact_by_switch[0] + compact_by_switch[1];
      const size_t flex_matches = flex_by_switch[0] + flex_by_switch[1];
      const size_t matches = full_matches + compact_matches + flex_matches;
      bool new_press = false;
      ++capture_count;
      if (matches == 0) ++unmatched_capture_count;
      full_match_count += full_matches;
      compact_match_count += compact_matches;
      flex_match_count += flex_matches;
      if (symbol_count == RX_SYMBOL_CAPACITY) ++capacity_reached_count;
      for (size_t index = 0; index < 2; ++index) {
        const size_t source_matches = by_switch[index].total();
        if (press_gates[index].observe(source_matches, captured_at_ms)) {
          new_press = true;
          ++press_count;
          test_led_on = !test_led_on;
          requested_led_on.store(test_led_on);
          s11mqtt::enqueue_press(index + 1, press_count, captured_at_ms);
          publish_report(ReportKind::Press, captured_at_ms, full_by_switch[index], compact_by_switch[index], ESP_OK, index + 1, flex_by_switch[index]);
        } else if (source_matches > 0) {
          ++merged_capture_count;
          publish_report(ReportKind::Repeat, captured_at_ms, full_by_switch[index], compact_by_switch[index], ESP_OK, index + 1, flex_by_switch[index]);
        }
      }

      if (xSemaphoreTake(raw_mutex, 0) == pdTRUE) {
        if (raw_recording && static_cast<uint32_t>(captured_at_ms - raw_started_ms) < RAW_WINDOW_MS) {
          RawCapture &saved = raw_captures[raw_capture_count++];
          saved.uptime_ms = captured_at_ms;
          saved.symbol_count = symbol_count;
          saved.full_matches = full_matches;
          saved.compact_matches = compact_matches;
          saved.flex_matches = flex_matches;
          saved.switch1_matches = by_switch[0].total();
          saved.switch2_matches = by_switch[1].total();
          saved.new_press = new_press;
          saved.is_last = is_last;
          memcpy(saved.symbols, captured_symbols, symbol_count * sizeof(captured_symbols[0]));
          if (raw_capture_count == RAW_PACKET_CAPACITY) finish_raw_recording("packet_limit");
        }
        xSemaphoreGive(raw_mutex);
      } else {
        ++raw_lock_misses;
      }
    }
    const uint32_t now_ms = millis();
    if (!capture_ready && static_cast<uint32_t>(now_ms - last_rx_progress_ms) >= RX_NO_CALLBACK_MS &&
        static_cast<uint32_t>(gpio4_changes - gpio4_changes_at_arm) >= RX_RECOVERY_MIN_CHANGES) {
      // Tisina sama ne pomeni zastoja. Spremembe so vzorcene, ne vsi RF robovi.
      // Prag je varovalo obnove, ne izmerjena meja protokola S11.
      esp_err_t error = rmt_disable(rf_receiver);
      if (error == ESP_OK) error = rmt_enable(rf_receiver);
      if (error == ESP_OK) {
        ++rx_restarts;
        last_rx_progress_ms = millis();
        // Morebitni callback tik pred ustavitvijo ohranimo za naslednji krog.
        if (!capture_ready) error = arm_capture();
      }
      if (error != ESP_OK) {
        publish_report(ReportKind::Error, millis(), 0, 0, error);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }
    if (static_cast<uint32_t>(now_ms - last_rf_stats_ms) >= RF_STATS_INTERVAL_MS) {
      last_rf_stats_ms = now_ms;
      publish_report(ReportKind::Stats, now_ms);
    }
  }
}

void save_led_settings() {
  if (!led_save_pending) return;
  led_save_pending = false;
  if (!led_preferences_ready ||
      led_preferences.putUInt("rgb_level", hubcontrol::pack(led_settings)) != sizeof(uint32_t) ||
      led_preferences.putBool("enabled", led_settings.enabled) != sizeof(bool)) {
    s11log::println("LED_SETTINGS_SAVE_ERROR: nastavitev ni mogoce shraniti v NVS.");
  }
}

// Ukaze in NVS obdeluje samo glavna zanka; MQTT callback samo odda v vrsto.
void service_hub_controls() {
  if (ota_updating.load()) {
    restart_pending = false;
    hubcontrol::Command ignored;
    while (s11mqtt::take_control(ignored)) {}
    return;
  }
  hubcontrol::Command command;
  if (s11mqtt::take_control(command)) {
    if (command.kind == hubcontrol::Kind::Restart) {
      if (!restart_pending) {
        save_led_settings();
        restart_pending = true;
        restart_at_ms = millis() + 250;
        s11mqtt::set_paused(true);
        s11log::println("HUB_RESTART_REQUESTED: ponovni zagon cez 250 ms.");
      }
    } else if (!restart_pending && hubcontrol::apply(led_settings, command)) {
      led_save_pending = true;
      led_changed_ms = millis();
      s11mqtt::publish_led_settings(led_settings);
      s11log::printf("LED_SETTINGS enabled=%s brightness=%u rgb=%u,%u,%u\n",
                    led_settings.enabled ? "yes" : "no", static_cast<unsigned>(led_settings.brightness),
                    static_cast<unsigned>(led_settings.red), static_cast<unsigned>(led_settings.green),
                    static_cast<unsigned>(led_settings.blue));
    }
  }
  if (led_save_pending && static_cast<uint32_t>(millis() - led_changed_ms) >= 1'500) save_led_settings();
  if (restart_pending && static_cast<int32_t>(millis() - restart_at_ms) >= 0) ESP.restart();
}

// RGB zapis uporablja RMT TX in lahko caka; lastnik je glavna zanka, ne RF task.
void service_test_led() {
  static hubcontrol::Color applied = {0, 0, 0};
  const hubcontrol::Color color = hubcontrol::output(led_settings, requested_led_on.load());
  if (color.red == applied.red && color.green == applied.green && color.blue == applied.blue) return;
  rgbLedWrite(TEST_LED_PIN, color.red, color.green, color.blue);
  applied = color;
}

}  // namespace

void setup() {
  if (!s11log::SERIAL_LOG_ENABLED) esp_log_level_set("*", ESP_LOG_NONE);
  Serial.begin(115200);
  delay(500);
  led_preferences_ready = led_preferences.begin("s11_led", false);
  if (led_preferences_ready) {
    led_settings = hubcontrol::unpack(led_preferences.getUInt("rgb_level", hubcontrol::pack(hubcontrol::DEFAULT_LED)),
                                    led_preferences.getBool("enabled", hubcontrol::DEFAULT_LED.enabled));
  } else {
    s11log::println("LED_SETTINGS_LOAD_ERROR: uporabljene so privzete nastavitve.");
  }
  rgbLedWrite(TEST_LED_PIN, 0, 0, 0);

  s11log::println();
  s11log::printf("RX500A S11 v%s MQTT Discovery + RGB controls + restart + RGB GPIO48 + WiFi sleep off + two switches + OTA + buffered RAW\n",
                s11mqtt::FIRMWARE_VERSION);
  s11log::println("GPIO48 RGB indikator: HA nastavlja omogoceno/svetlost/barvo; S11 preklaplja prikaz.");
  s11log::println("GPIO4, RMT DMA 1 us, filter 1 us, paketni premor 15 ms");
  s11log::printf("RAW=%s, zdruzevanje ponovitev=%lu ms\n",
                RAW_OUTPUT ? "on" : "off",
                static_cast<unsigned long>(s11::REPEAT_SILENCE_MS));
  s11log::println("flex = dodatne delne oblike; tisina sama ne sprozi RX obnove.");
  s11log::println("RF_STATS vsakih 5 s: skupni stevci od zagona; S11_REPEAT je zdruzena zaznava.");
  s11log::println("RF task ne izpisuje na Serial; callbacks/rx_restarts/gpio4_changes locujejo zajem in vhod.");
  s11log::println("S11_PRESS switch=1/2; ponovitve se zdruzujejo loceno za vsako stikalo.");
  s11log::println("RAW ukazi: r = 20 s zajema v PSRAM, s = predcasen konec in izpis.");

  s11mqtt::begin(led_settings);
  rf_reports = xQueueCreate(32, sizeof(RfReport));
  raw_mutex = xSemaphoreCreateMutex();
  if (rf_reports == nullptr || raw_mutex == nullptr) fail_if_error(ESP_ERR_NO_MEM, "RF vrsta/mutex");
  if (xTaskCreatePinnedToCore(rf_task, "rf_capture", 4096, nullptr, 2, &rf_task_handle, xPortGetCoreID()) != pdPASS) {
    fail_if_error(ESP_ERR_NO_MEM, "RF task");
  }
  setup_network();
}

void loop() {
  service_hub_controls();
  service_test_led();
  service_network();
  service_rf_reports();
  s11mqtt::service_reports();
  service_raw_capture();
  delay(1);
}
