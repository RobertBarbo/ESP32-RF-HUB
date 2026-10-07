#include "s11_detector.h"

// Prevajalnik izvede teste dejanske C++ logike brez priklopljene naprave.
constexpr bool test_matching() {
  s11::Pulse pulses[s11::PATTERN_LENGTH * 2 + 1] = {};
  pulses[0] = {2000, 0};  // Vzorec ni nujno na zacetku zajema.
  for (size_t i = 0; i < s11::PATTERN_LENGTH * 2; ++i) {
    pulses[i + 1] = {s11::PATTERN_US[i % s11::PATTERN_LENGTH],
                     static_cast<uint8_t>(1 - i % 2)};
  }
  if (s11::count_matches(pulses, s11::PATTERN_LENGTH * 2 + 1) != 2) return false;
  if (s11::count_matches(pulses + 1, s11::PATTERN_LENGTH - 1) != 0) return false;
  pulses[1].level = 0;  // Napacen nivo zavrne prvo ponovitev.
  if (s11::count_matches(pulses, s11::PATTERN_LENGTH * 2 + 1) != 1) return false;
  pulses[1].level = 1;
  pulses[3].duration = 1000;  // Napacno trajanje ni veljavna koda.
  if (s11::count_matches(pulses, s11::PATTERN_LENGTH * 2 + 1) != 1) return false;
  pulses[1].duration = 0;  // Veljavni podatki za terminatorjem ne stejejo.
  return s11::count_matches(pulses, s11::PATTERN_LENGTH * 2 + 1) == 0;
}

constexpr bool test_repeat_gate() {
  s11::PressGate gate;
  if (gate.observe(0, 0)) return false;
  if (!gate.observe(3, 0)) return false;
  if (gate.observe(2, 20)) return false;
  if (gate.observe(0, 20 + s11::REPEAT_SILENCE_MS - 1)) return false;  // Ozadje ne podaljsa zapore.
  if (gate.observe(1, 20 + s11::REPEAT_SILENCE_MS - 1)) return false;
  if (!gate.observe(4, 20 + 2 * s11::REPEAT_SILENCE_MS - 1)) return false;
  if (!gate.observe(2, 5000)) return false;
  s11::PressGate wrap;
  if (!wrap.observe(1, UINT32_MAX - 20)) return false;
  if (wrap.observe(1, 20)) return false;
  return wrap.observe(1, 20 + s11::REPEAT_SILENCE_MS);
}

static_assert(test_matching(), "S11 matching failed");
static_assert(test_repeat_gate(), "S11 repeat suppression failed");

constexpr bool test_fast_press_capture() {
  s11::PressGate gate;
  // Dejanska zaznava, ki je bila pri 500 ms zdruzena kot S11_REPEAT.
  if (!gate.observe(3, 26183)) return false;
  if (!gate.observe(3, 26519)) return false;
  // Bliznja ponovitev se se vedno zdruzi in podaljsa okno od zadnje zaznave.
  if (gate.observe(2, 26583)) return false;
  if (gate.observe(0, 26882)) return false;
  return gate.observe(1, 26883);
}

static_assert(test_fast_press_capture(), "S11 fast press capture failed");

constexpr bool test_299_ms_capture() {
  s11::PressGate gate;
  // Dejanska zaporedna ujemanja iz izpisa z mejo 300 ms.
  if (!gate.observe(3, 150260)) return false;
  if (!gate.observe(2, 150578)) return false;
  if (!gate.observe(3, 150909)) return false;
  if (!gate.observe(2, 151208)) return false;
  // Meja: 99 ms se zdruzi, 100 ms od zadnjega ujemanja se sprejme.
  if (gate.observe(1, 151307)) return false;
  return gate.observe(1, 151407);
}

static_assert(test_299_ms_capture(), "S11 299 ms capture failed");

constexpr bool test_rapid_press_chain() {
  s11::PressGate gate;
  // Dejanska veriga, ki je pri 250 ms ostala brez novih preklopov LED.
  constexpr uint32_t times[] = {
      148835, 148981, 149126, 149277, 149416, 149569, 149713, 149861,
      150007, 150148, 150292, 150445, 150597, 150740, 150892, 151037,
      151191, 151332, 151482, 151636, 151788, 151931, 152085, 152229};
  for (uint32_t now : times) {
    if (!gate.observe(3, now)) return false;
  }
  // Telegrami iste kratke oddaje se se vedno zdruzijo.
  if (gate.observe(1, 152234)) return false;
  return !gate.observe(1, 152239);
}

static_assert(test_rapid_press_chain(), "S11 rapid press chain failed");

constexpr bool test_compact_matching() {
  // Dejanski zajem ob 12:23:23.763, ki ga v1 ni prepoznal.
  s11::Pulse pulses[] = {
      {561, 1}, {106, 0}, {162, 1}, {237, 0}, {162, 1},
      {239, 0}, {162, 1}, {237, 0}, {163, 1}, {2637, 0}};
  if (s11::count_compact_matches(pulses, 10) != 1) return false;
  if (s11::count_compact_matches(pulses, 9) != 0) return false;
  pulses[4].level = 0;
  if (s11::count_compact_matches(pulses, 10) != 0) return false;
  pulses[4].level = 1;
  pulses[3].duration += 100;
  pulses[9].duration -= 100;  // Isto obdobje, napacni razmiki podatkov.
  if (s11::count_compact_matches(pulses, 10) != 0) return false;
  pulses[3].duration -= 100;
  pulses[9].duration += 300;  // Napacno obdobje in zakljucni premor.
  if (s11::count_compact_matches(pulses, 10) != 0) return false;
  pulses[9].duration = 2637;
  pulses[2].duration = 0;
  return s11::count_compact_matches(pulses, 10) == 0;
}

static_assert(test_compact_matching(), "S11 compact matching failed");

constexpr bool test_partial_compact_matching() {
  // Dve dejanski ponovitvi prej zavrnjenega bloka pri uptime 328188 ms.
  s11::Pulse pulses[] = {
      {568, 1}, {101, 0}, {165, 1}, {235, 0}, {166, 1}, {233, 0},
      {166, 1}, {235, 0}, {165, 1}, {1569, 0}, {30, 1}, {1036, 0},
      {568, 1}, {97, 0}, {170, 1}, {230, 0}, {170, 1}, {232, 0},
      {166, 1}, {234, 0}, {167, 1}, {1568, 0}, {29, 1}, {1036, 0}};
  if (s11::count_compact_matches(pulses, 24) != 2) return false;
  if (s11::count_compact_matches(pulses, 11) != 0) return false;
  pulses[10].level = 0;
  if (s11::count_compact_matches(pulses, 12) != 0) return false;
  pulses[10].level = 1;
  pulses[10].duration += 50;
  pulses[11].duration -= 50;  // Isto obdobje, veljaven LOW, nedovoljen dodatni HIGH.
  if (s11::count_compact_matches(pulses, 12) != 0) return false;
  pulses[10].duration -= 50;
  pulses[11].duration += 50;
  pulses[9].duration -= 500;
  pulses[11].duration += 500;  // Isto obdobje, drugacna razdelitev premora.
  if (s11::count_compact_matches(pulses, 12) != 0) return false;
  pulses[9].duration += 500;
  pulses[11].duration -= 500;
  pulses[3].duration += 50;
  pulses[11].duration -= 50;  // Isto obdobje, veljaven zadnji LOW, napacen razmik podatkov.
  if (s11::count_compact_matches(pulses, 12) != 0) return false;
  pulses[3].duration -= 50;
  pulses[11].duration += 50;
  pulses[0].duration = 0;
  return s11::count_compact_matches(pulses, 24) == 0;
}

static_assert(test_partial_compact_matching(), "S11 partial compact matching failed");

constexpr bool test_switch2_matching() {
  // Dejanske oblike iz desetih pritiskov drugega stikala.
  s11::Pulse full[] = {{322, 1}, {79, 0}, {188, 1}, {79, 0}, {50, 1}, {83, 0}, {188, 1}, {80, 0}, {320, 1}, {79, 0}, {50, 1}, {83, 0}, {188, 1}, {80, 0}, {49, 1}, {84, 0}, {49, 1}, {84, 0}, {50, 1}, {84, 0}, {50, 1}, {1016, 0}, {51, 1}, {82, 0}, {51, 1}, {83, 0}, {51, 1}, {82, 0}, {51, 1}, {82, 0}, {52, 1}, {82, 0}, {51, 1}, {81, 0}, {53, 1}, {81, 0}, {53, 1}, {80, 0}, {52, 1}, {82, 0}, {52, 1}, {81, 0}};
  s11::Pulse compact[] = {{306, 1}, {103, 0}, {165, 1}, {235, 0}, {165, 1}, {102, 0}, {298, 1}, {236, 0}, {164, 1}, {2901, 0}};
  s11::Pulse partial[] = {{304, 1}, {96, 0}, {170, 1}, {231, 0}, {170, 1}, {96, 0}, {303, 1}, {231, 0}, {170, 1}, {1565, 0}, {31, 1}, {102, 0}, {31, 1}, {103, 0}, {29, 1}, {104, 0}, {30, 1}, {102, 0}, {31, 1}, {103, 0}, {31, 1}, {102, 0}, {32, 1}, {101, 0}, {32, 1}, {102, 0}, {31, 1}, {102, 0}, {32, 1}, {98, 0}};
  if (s11::count_switch2_matches(full, 42).full != 1) return false;
  if (s11::count_switch2_matches(full, 41).full != 0) return false;
  if (s11::count_matches(full, 42) != 0 || s11::count_compact_matches(full, 42) != 0) return false;
  if (s11::count_switch2_matches(compact, 10).compact != 1) return false;
  if (s11::count_switch2_matches(compact, 9).compact != 0) return false;
  if (s11::count_compact_matches(compact, 10) != 0) return false;
  if (s11::count_switch2_matches(partial, 30).compact != 1) return false;
  if (s11::count_switch2_matches(partial, 29).compact != 0) return false;
  full[0].level = 0;
  if (s11::count_switch2_matches(full, 42).full != 0) return false;
  full[0].level = 1;
  full[0].duration = 0;
  if (s11::count_switch2_matches(full, 42).full != 0) return false;
  const uint16_t saved = compact[2].duration;
  compact[2].duration = compact[6].duration;
  compact[6].duration = saved;  // Isto obdobje, napacna dolga HIGH na podatkovnih mestih.
  if (s11::count_switch2_matches(compact, 10).compact != 0) return false;
  partial[10].duration += 50;
  partial[11].duration -= 50;
  if (s11::count_switch2_matches(partial, 30).compact != 0) return false;
  s11::Pulse first[s11::PATTERN_LENGTH] = {};
  for (size_t i = 0; i < s11::PATTERN_LENGTH; ++i) {
    first[i] = {s11::PATTERN_US[i], static_cast<uint8_t>(1 - i % 2)};
  }
  const auto first_result = s11::count_switch2_matches(first, s11::PATTERN_LENGTH);
  return first_result.full == 0 && first_result.compact == 0;
}

constexpr bool test_two_switch_gates() {
  s11::PressGate gates[2];
  bool led = false;
  unsigned presses = 0;
  if (gates[0].observe(3, 1000)) { led = !led; ++presses; }
  if (gates[0].observe(2, 1010)) return false;
  // Drugo stikalo preklopi skupno LED tudi med zaporo ponovitve prvega.
  if (gates[1].observe(4, 1020)) { led = !led; ++presses; }
  if (led || presses != 2) return false;
  if (gates[1].observe(2, 1030)) return false;
  if (gates[0].observe(3, 1200)) { led = !led; ++presses; }
  if (!led || presses != 3) return false;
  if (gates[1].observe(3, 1220)) { led = !led; ++presses; }
  return !led && presses == 4;
}

static_assert(test_switch2_matching(), "S11 switch2 forms or isolation failed");
static_assert(test_two_switch_gates(), "S11 independent gates / shared LED failed");

constexpr bool test_variable_partial_1() {
  // Dejanski RAW 3cd73dce-8a36-4754-af72-c061774a2100, blok 686, interval 386.
  s11::Pulse pulses[] = {{578, 1}, {90, 0}, {176, 1}, {224, 0}, {176, 1}, {223, 0}, {177, 1}, {94, 0}, {28, 1}, {102, 0}, {176, 1}, {360, 0}, {30, 1}, {103, 0}, {30, 1}, {1036, 0}, {33, 1}, {100, 0}, {32, 1}, {102, 0}, {32, 1}, {101, 0}, {33, 1}, {100, 0}, {33, 1}, {101, 0}, {32, 1}, {102, 0}, {32, 1}, {101, 0}, {32, 1}, {99, 0}};
  const auto accepted = s11::count_robust_matches(pulses, 32, 0);
  if (accepted.full != 0 || accepted.compact != 0 || accepted.flex != 1) return false;
  if (s11::count_robust_matches(pulses, 32, 1).total() != 0) return false;
  if (s11::count_robust_matches(pulses, 31, 0).total() != 0) return false;
  pulses[2].level = 0;
  if (s11::count_robust_matches(pulses, 32, 0).total() != 0) return false;
  pulses[2].level = 1;
  pulses[31].duration += 200;
  if (s11::count_robust_matches(pulses, 32, 0).total() != 0) return false;
  pulses[31].duration -= 200;
  // Izgubljeni DOLGI HIGH: enako obdobje, ne sme biti sprejet.
  s11::Pulse missing_long[30] = {};
  missing_long[0] = pulses[0];
  missing_long[1] = {static_cast<uint16_t>(pulses[1].duration + pulses[2].duration + pulses[3].duration), 0};
  for (size_t i = 2; i < 30; ++i) missing_long[i] = pulses[i + 2];
  if (s11::count_robust_matches(missing_long, 30, 0).total() != 0) return false;
  pulses[2].duration = 0;
  return s11::count_robust_matches(pulses, 32, 0).total() == 0;
}

static_assert(test_variable_partial_1(), "S11 variable partial 1 failed");

constexpr bool test_variable_partial_2() {
  // Dejanski RAW fa78efeb-bb47-41eb-80cb-c04856d998ba, blok 152, interval 62.
  s11::Pulse pulses[] = {{309, 1}, {92, 0}, {173, 1}, {228, 0}, {172, 1}, {95, 0}, {306, 1}, {228, 0}, {171, 1}, {365, 0}, {29, 1}, {103, 0}, {30, 1}, {1036, 0}, {34, 1}, {99, 0}, {33, 1}, {101, 0}, {32, 1}, {101, 0}, {33, 1}, {100, 0}, {33, 1}, {101, 0}, {33, 1}, {100, 0}, {33, 1}, {101, 0}, {32, 1}, {101, 0}, {33, 1}, {101, 0}, {33, 1}, {97, 0}};
  const auto accepted = s11::count_robust_matches(pulses, 34, 1);
  if (accepted.full != 0 || accepted.compact != 0 || accepted.flex != 1) return false;
  if (s11::count_robust_matches(pulses, 34, 0).total() != 0) return false;
  if (s11::count_robust_matches(pulses, 33, 1).total() != 0) return false;
  pulses[2].level = 0;
  if (s11::count_robust_matches(pulses, 34, 1).total() != 0) return false;
  pulses[2].level = 1;
  pulses[33].duration += 200;
  if (s11::count_robust_matches(pulses, 34, 1).total() != 0) return false;
  pulses[33].duration -= 200;
  // Izgubljeni DOLGI HIGH: enako obdobje, ne sme biti sprejet.
  s11::Pulse missing_long[32] = {};
  missing_long[0] = pulses[0];
  missing_long[1] = {static_cast<uint16_t>(pulses[1].duration + pulses[2].duration + pulses[3].duration), 0};
  for (size_t i = 2; i < 32; ++i) missing_long[i] = pulses[i + 2];
  if (s11::count_robust_matches(missing_long, 32, 1).total() != 0) return false;
  pulses[2].duration = 0;
  return s11::count_robust_matches(pulses, 34, 1).total() == 0;
}

static_assert(test_variable_partial_2(), "S11 variable partial 2 failed");

constexpr bool test_robust_priority() {
  for (uint8_t source = 0; source < 2; ++source) {
    const uint16_t *reference = source == 0 ? s11::PATTERN_US : s11::SWITCH2_PATTERN_US;
    const size_t length = source == 0 ? s11::PATTERN_LENGTH : s11::SWITCH2_PATTERN_LENGTH;
    s11::Pulse pulses[2 * s11::SWITCH2_PATTERN_LENGTH] = {};
    for (size_t i = 0; i < length * 2; ++i) {
      pulses[i] = {reference[i % length], static_cast<uint8_t>(1 - i % 2)};
    }
    const auto matches = s11::count_robust_matches(pulses, length * 2, source);
    if (matches.full != 2 || matches.compact != 0 || matches.flex != 0) return false;
    if (s11::count_robust_matches(pulses, length * 2, 2).total() != 0) return false;
  }
  return true;
}

static_assert(test_robust_priority(), "S11 repeats counted more than once");
