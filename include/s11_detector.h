#pragma once

#include <stddef.h>
#include <stdint.h>

namespace s11 {

struct Pulse {
  uint16_t duration;
  uint8_t level;
};

// Izmerjeni vzorec, ne splosen dekoder protokola ali potrjen ID naprave.
constexpr uint16_t PATTERN_US[] = {
    581, 86, 181, 86, 49, 84, 181, 86,
    48, 85, 181, 87, 47, 85, 181, 87,
    47, 85, 49, 85, 48, 85, 49, 1017,
    51, 83, 50, 83, 50, 84, 48, 85,
    50, 84, 48, 85, 49, 85, 48, 84};
constexpr size_t PATTERN_LENGTH = sizeof(PATTERN_US) / sizeof(PATTERN_US[0]);
// Hitri pritiski v novem izpisu imajo razmike 132-245 ms.
constexpr uint32_t REPEAT_SILENCE_MS = 100;
constexpr size_t COMPACT_LENGTH = 10;
constexpr size_t PARTIAL_COMPACT_LENGTH = 12;
constexpr uint16_t COMPACT_SYNC_HIGH_MIN_US = 450;
constexpr uint16_t COMPACT_SYNC_HIGH_MAX_US = 650;
constexpr uint16_t COMPACT_SYNC_LOW_MIN_US = 60;
constexpr uint16_t COMPACT_SYNC_LOW_MAX_US = 150;
constexpr uint16_t COMPACT_DATA_HIGH_MIN_US = 90;
constexpr uint16_t COMPACT_DATA_HIGH_MAX_US = 210;
constexpr uint16_t COMPACT_PAIR_MIN_US = 370;
constexpr uint16_t COMPACT_PAIR_MAX_US = 430;
constexpr uint16_t COMPACT_GAP_MIN_US = 2500;
constexpr uint16_t COMPACT_GAP_MAX_US = 2800;
constexpr uint16_t COMPACT_PERIOD_MIN_US = 4587;
constexpr uint16_t COMPACT_PERIOD_MAX_US = 4747;
// Izmerjena delna oblika: dolg premor je razdeljen z enim kratkim HIGH.
constexpr uint16_t COMPACT_PARTIAL_FIRST_LOW_MIN_US = 1500;
constexpr uint16_t COMPACT_PARTIAL_FIRST_LOW_MAX_US = 1650;
constexpr uint16_t COMPACT_PARTIAL_HIGH_MIN_US = 20;
constexpr uint16_t COMPACT_PARTIAL_HIGH_MAX_US = 60;
constexpr uint16_t COMPACT_PARTIAL_LAST_LOW_MIN_US = 950;
constexpr uint16_t COMPACT_PARTIAL_LAST_LOW_MAX_US = 1120;

constexpr bool in_range(uint32_t value, uint32_t minimum, uint32_t maximum) {
  return value >= minimum && value <= maximum;
}

// Locena izmerjena oblika, kjer kratki HIGH impulzi niso vidni.
// Preverimo vse nivoje, stiri podatkovne HIGH, razmike in celotno obdobje.
constexpr bool compact_matches_at(const Pulse *pulses) {
  uint32_t period = 0;
  for (size_t i = 0; i < COMPACT_LENGTH; ++i) {
    if (pulses[i].duration == 0 || pulses[i].level != (1 - i % 2)) return false;
    period += pulses[i].duration;
  }
  if (!in_range(pulses[0].duration, COMPACT_SYNC_HIGH_MIN_US, COMPACT_SYNC_HIGH_MAX_US) ||
      !in_range(pulses[1].duration, COMPACT_SYNC_LOW_MIN_US, COMPACT_SYNC_LOW_MAX_US) ||
      !in_range(pulses[9].duration, COMPACT_GAP_MIN_US, COMPACT_GAP_MAX_US) ||
      !in_range(period, COMPACT_PERIOD_MIN_US, COMPACT_PERIOD_MAX_US)) return false;
  for (size_t i = 2; i <= 8; i += 2) {
    if (!in_range(pulses[i].duration, COMPACT_DATA_HIGH_MIN_US, COMPACT_DATA_HIGH_MAX_US)) return false;
    if (i < 8 && !in_range(static_cast<uint32_t>(pulses[i].duration) + pulses[i + 1].duration,
                          COMPACT_PAIR_MIN_US, COMPACT_PAIR_MAX_US)) return false;
  }
  return true;
}

constexpr bool partial_compact_matches_at(const Pulse *pulses) {
  for (size_t i = 0; i < PARTIAL_COMPACT_LENGTH; ++i) {
    if (pulses[i].duration == 0 || pulses[i].level != (1 - i % 2)) return false;
  }
  if (!in_range(pulses[9].duration, COMPACT_PARTIAL_FIRST_LOW_MIN_US, COMPACT_PARTIAL_FIRST_LOW_MAX_US) ||
      !in_range(pulses[10].duration, COMPACT_PARTIAL_HIGH_MIN_US, COMPACT_PARTIAL_HIGH_MAX_US) ||
      !in_range(pulses[11].duration, COMPACT_PARTIAL_LAST_LOW_MIN_US, COMPACT_PARTIAL_LAST_LOW_MAX_US)) return false;
  const uint32_t gap = static_cast<uint32_t>(pulses[9].duration) +
      pulses[10].duration + pulses[11].duration;
  if (!in_range(gap, COMPACT_GAP_MIN_US, COMPACT_GAP_MAX_US)) return false;
  Pulse compact[COMPACT_LENGTH] = {};
  for (size_t i = 0; i < COMPACT_LENGTH - 1; ++i) compact[i] = pulses[i];
  compact[9] = {static_cast<uint16_t>(gap), 0};
  // Ohranjeni sync, vsi stiri dolgi HIGH, razmiki in celotno obdobje.
  return compact_matches_at(compact);
}

constexpr size_t count_compact_matches(const Pulse *pulses, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (pulses[i].duration == 0) {
      count = i;
      break;
    }
  }
  size_t matches = 0;
  for (size_t start = 0; start + COMPACT_LENGTH <= count;) {
    if (compact_matches_at(pulses + start)) {
      ++matches;
      start += COMPACT_LENGTH;
    } else if (start + PARTIAL_COMPACT_LENGTH <= count && partial_compact_matches_at(pulses + start)) {
      ++matches;
      start += PARTIAL_COMPACT_LENGTH;
    } else {
      ++start;
    }
  }
  return matches;
}

constexpr size_t count_matches(const Pulse *pulses, size_t count) {
  // Nicla zakljuci zajem; morebitne ostanke za njo ignoriramo.
  for (size_t i = 0; i < count; ++i) {
    if (pulses[i].duration == 0) {
      count = i;
      break;
    }
  }

  size_t matches = 0;
  for (size_t start = 0; start + PATTERN_LENGTH <= count;) {
    bool match = true;
    for (size_t i = 0; i < PATTERN_LENGTH; ++i) {
      const Pulse &pulse = pulses[start + i];
      const uint32_t expected = PATTERN_US[i];
      const uint32_t actual = pulse.duration;
      const uint32_t difference = actual > expected ? actual - expected : expected - actual;
      // Ista toleranca kot pri analizi: max(30 us, 20 % povprecja).
      if (pulse.level != (1 - i % 2) ||
          (difference > 30 && difference * 10 > actual + expected)) {
        match = false;
        break;
      }
    }
    if (match) {
      ++matches;
      start += PATTERN_LENGTH;
    } else {
      ++start;
    }
  }
  return matches;
}

// Drugo stikalo: izmerjena 42-intervalna oblika iz RAW desetih pritiskov.
constexpr uint16_t SWITCH2_PATTERN_US[] = {
    322, 79, 188, 78, 50, 83, 188, 79, 321, 80, 50, 83, 188, 80,
    49, 84, 50, 84, 50, 84, 50, 1016,
    52, 80, 52, 83, 51, 82, 51, 82, 51, 82, 51, 82,
    52, 82, 52, 82, 52, 82, 51, 82};
constexpr size_t SWITCH2_PATTERN_LENGTH = sizeof(SWITCH2_PATTERN_US) / sizeof(SWITCH2_PATTERN_US[0]);
constexpr uint16_t SWITCH2_COMPACT_US[] = {322, 79, 188, 212, 188, 80, 320, 212, 188, 2879};
constexpr size_t SWITCH2_COMPACT_LENGTH = 10;
constexpr size_t SWITCH2_PARTIAL_LENGTH = 30;
constexpr uint16_t SWITCH2_GAP_MIN_US = 2800;
constexpr uint16_t SWITCH2_GAP_MAX_US = 2980;
constexpr uint16_t SWITCH2_PERIOD_MIN_US = 4587;
constexpr uint16_t SWITCH2_PERIOD_MAX_US = 4747;

constexpr bool switch2_reference_matches(const Pulse *pulses, const uint16_t *reference, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const uint32_t actual = pulses[i].duration;
    const uint32_t expected = reference[i];
    const uint32_t difference = actual > expected ? actual - expected : expected - actual;
    if (actual == 0 || pulses[i].level != (1 - i % 2) ||
        (difference > 30 && difference * 10 > actual + expected)) return false;
  }
  return true;
}

constexpr bool switch2_period_matches(const Pulse *pulses, size_t count) {
  uint32_t period = 0;
  for (size_t i = 0; i < count; ++i) period += pulses[i].duration;
  return in_range(period, SWITCH2_PERIOD_MIN_US, SWITCH2_PERIOD_MAX_US);
}

constexpr bool switch2_compact_matches_at(const Pulse *pulses) {
  return switch2_reference_matches(pulses, SWITCH2_COMPACT_US, SWITCH2_COMPACT_LENGTH) &&
      in_range(pulses[9].duration, SWITCH2_GAP_MIN_US, SWITCH2_GAP_MAX_US) &&
      switch2_period_matches(pulses, SWITCH2_COMPACT_LENGTH);
}

constexpr bool switch2_partial_matches_at(const Pulse *pulses) {
  // Izmerjeni premor: LOW okoli 1565 us, nato deset ohranjenih kratkih HIGH/LOW.
  // Preverimo konkretno obliko, ne odstranjujemo splosno kratkih impulzov.
  if (!switch2_reference_matches(pulses, SWITCH2_COMPACT_US, 9) ||
      pulses[9].level != 0 || !in_range(pulses[9].duration, 1500, 1650)) return false;
  uint32_t gap = pulses[9].duration;
  for (size_t i = 10; i < SWITCH2_PARTIAL_LENGTH; i += 2) {
    if (pulses[i].level != 1 || pulses[i + 1].level != 0 ||
        !in_range(pulses[i].duration, 20, 60) || !in_range(pulses[i + 1].duration, 70, 115) ||
        !in_range(static_cast<uint32_t>(pulses[i].duration) + pulses[i + 1].duration, 115, 150)) return false;
    gap += pulses[i].duration + pulses[i + 1].duration;
  }
  Pulse compact[SWITCH2_COMPACT_LENGTH] = {};
  for (size_t i = 0; i < 9; ++i) compact[i] = pulses[i];
  if (!in_range(gap, SWITCH2_GAP_MIN_US, SWITCH2_GAP_MAX_US)) return false;
  compact[9] = {static_cast<uint16_t>(gap), 0};
  return switch2_compact_matches_at(compact);
}

struct Switch2Matches {
  size_t full = 0;
  size_t compact = 0;
};

constexpr Switch2Matches count_switch2_matches(const Pulse *pulses, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (pulses[i].duration == 0) { count = i; break; }
  }
  Switch2Matches matches;
  for (size_t start = 0; start + SWITCH2_COMPACT_LENGTH <= count;) {
    if (start + SWITCH2_PATTERN_LENGTH <= count &&
        switch2_reference_matches(pulses + start, SWITCH2_PATTERN_US, SWITCH2_PATTERN_LENGTH) &&
        switch2_period_matches(pulses + start, SWITCH2_PATTERN_LENGTH)) {
      ++matches.full;
      start += SWITCH2_PATTERN_LENGTH;
    } else if (switch2_compact_matches_at(pulses + start)) {
      ++matches.compact;
      start += SWITCH2_COMPACT_LENGTH;
    } else if (start + SWITCH2_PARTIAL_LENGTH <= count && switch2_partial_matches_at(pulses + start)) {
      ++matches.compact;
      start += SWITCH2_PARTIAL_LENGTH;
    } else {
      ++start;
    }
  }
  return matches;
}

// Manjkajo lahko samo znani kratki HIGH; njihove case pricakujemo v LOW.
// Dolgih HIGH ne izpuscamo. Vsota LOW ima ozjo toleranco od posameznih HIGH.
constexpr bool near_duration(uint32_t actual, uint32_t expected, uint32_t ratio) {
  const uint32_t difference = actual > expected ? actual - expected : expected - actual;
  return difference <= 30 || difference * ratio <= actual + expected;
}

constexpr size_t flexible_matches_at(const Pulse *pulses, size_t count,
                                     const uint16_t *reference, size_t reference_count) {
  size_t observed = 0;
  size_t skipped = 0;
  uint32_t period = 0;
  for (size_t expected = 0; expected < reference_count;) {
    if (observed + 2 > count || pulses[observed].level != 1 ||
        pulses[observed + 1].level != 0 || pulses[observed].duration == 0 ||
        pulses[observed + 1].duration == 0 ||
        !near_duration(pulses[observed].duration, reference[expected], 10)) return 0;
    period += pulses[observed].duration + pulses[observed + 1].duration;
    ++expected;
    uint32_t low = reference[expected];
    const uint32_t actual_low = pulses[observed + 1].duration;
    while (!near_duration(actual_low, low, 100)) {
      if (actual_low < low || expected + 2 >= reference_count || reference[expected + 1] > 80) return 0;
      low += reference[expected + 1] + reference[expected + 2];
      expected += 2;
      ++skipped;
    }
    ++expected;
    observed += 2;
  }
  return skipped > 0 && in_range(period, COMPACT_PERIOD_MIN_US, COMPACT_PERIOD_MAX_US) ? observed : 0;
}

struct MatchCounts {
  size_t full = 0;
  size_t compact = 0;
  size_t flex = 0;
  constexpr size_t total() const { return full + compact + flex; }
};

// En prehod na izvor: iste ponovitve ne stejemo v vec oblikah.
constexpr MatchCounts count_robust_matches(const Pulse *pulses, size_t count, uint8_t source) {
  MatchCounts matches;
  if (source > 1) return matches;
  for (size_t i = 0; i < count; ++i) {
    if (pulses[i].duration == 0) { count = i; break; }
  }
  const uint16_t *reference = source == 0 ? PATTERN_US : SWITCH2_PATTERN_US;
  const size_t full_length = source == 0 ? PATTERN_LENGTH : SWITCH2_PATTERN_LENGTH;
  const size_t partial_length = source == 0 ? PARTIAL_COMPACT_LENGTH : SWITCH2_PARTIAL_LENGTH;
  for (size_t start = 0; start + COMPACT_LENGTH <= count;) {
    const Pulse *frame = pulses + start;
    size_t consumed = 0;
    const bool full = start + full_length <= count &&
        (source == 0 ? count_matches(frame, full_length) == 1 :
         switch2_reference_matches(frame, reference, full_length) && switch2_period_matches(frame, full_length));
    if (full) {
      ++matches.full;
      consumed = full_length;
    } else if (source == 0 ? compact_matches_at(frame) : switch2_compact_matches_at(frame)) {
      ++matches.compact;
      consumed = COMPACT_LENGTH;
    } else if (start + partial_length <= count &&
               (source == 0 ? partial_compact_matches_at(frame) : switch2_partial_matches_at(frame))) {
      ++matches.compact;
      consumed = partial_length;
    } else {
      consumed = flexible_matches_at(frame, count - start, reference, full_length);
      if (consumed > 0) ++matches.flex;
    }
    start += consumed > 0 ? consumed : 1;
  }
  return matches;
}

class PressGate {
 public:
  constexpr bool observe(size_t matches, uint32_t now_ms) {
    if (matches == 0) {
      return false;
    }
    const bool new_press = !seen_ ||
        static_cast<uint32_t>(now_ms - last_match_ms_) >= REPEAT_SILENCE_MS;
    seen_ = true;
    last_match_ms_ = now_ms;
    return new_press;
  }

 private:
  bool seen_ = false;
  uint32_t last_match_ms_ = 0;
};

}  // namespace s11
