#include "hub_controls.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

using namespace hubcontrol;
constexpr char TOKEN[] = "RESTART_12345678";

bool parse_text(const char *topic, const char *text, Command &command) {
  return parse(topic, reinterpret_cast<const uint8_t *>(text), strlen(text), TOKEN, command);
}

int main() {
  Command command = {Kind::Restart, 17, 18, 19};
  LedSettings settings = DEFAULT_LED;
  assert(parse_text(POWER_COMMAND, "OFF", command));
  assert(apply(settings, command) && !settings.enabled);
  assert(!apply(settings, command));
  assert(parse_text(COLOR_COMMAND, "255, 128,0", command));
  assert(apply(settings, command) && !settings.enabled);
  assert(parse_text(BRIGHTNESS_COMMAND, "64", command));
  assert(apply(settings, command) && !settings.enabled);
  Color color = output(settings, true);
  assert(color.red == 0 && color.green == 0 && color.blue == 0);
  assert(parse_text(POWER_COMMAND, "ON", command));
  assert(apply(settings, command));
  color = output(settings, true);
  assert(color.red == 64 && color.green == 32 && color.blue == 0);
  color = output(settings, false);
  assert(color.red == 0 && color.green == 0 && color.blue == 0);
  assert(equal(settings, unpack(pack(settings), settings.enabled)));

  for (const char *invalid : {"", "256", "-1", "+1", "1.5", "255x", "99999999999999999999"}) {
    assert(!parse_text(BRIGHTNESS_COMMAND, invalid, command));
  }
  for (const char *invalid : {"1,2", "1,2,", "1,2,3,4", "1,2,256", "-1,2,3", "1;2;3", "1,2,3x"}) {
    assert(!parse_text(COLOR_COMMAND, invalid, command));
  }
  assert(!parse_text(POWER_COMMAND, "on", command));
  assert(!parse_text("unknown", "ON", command));
  assert(!parse_text(RESTART_COMMAND, "RESTART_87654321", command));
  assert(!parse_text(RESTART_COMMAND, "RESTART", command));
  assert(parse_text(RESTART_COMMAND, TOKEN, command) && command.kind == Kind::Restart);
  const LedSettings before = settings;
  assert(!apply(settings, command) && equal(settings, before));
  const uint8_t unterminated[] = {'2', '5', '5'};
  assert(parse(BRIGHTNESS_COMMAND, unterminated, sizeof(unterminated), TOKEN, command));
  assert(command.first == 255);
  const uint8_t embedded_zero[] = {'O', 'N', 0, 'O', 'F', 'F'};
  assert(!parse(POWER_COMMAND, embedded_zero, sizeof(embedded_zero), TOKEN, command));
  assert(!parse(BRIGHTNESS_COMMAND, unterminated, 40, TOKEN, command));
  assert(parse_text(BRIGHTNESS_COMMAND, "0", command));
  assert(apply(settings, command) && !settings.enabled);
  assert(parse_text(POWER_COMMAND, "ON", command));
  assert(apply(settings, command) && settings.enabled && settings.brightness == 16);
  puts("PASS: veljavni/neveljavni ukazi, RGB skala, izklop indikatorja, shranjevanje in restart token.");
}
