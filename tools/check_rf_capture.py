"""Primerja shranjen RAW izpis z referenco iz firmware (brez naprave)."""

import argparse
import pathlib
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--expect", help="Pricakovana ujemanja po blokih, locena z vejico")
    parser.add_argument("--expect-compact", help="Pricakovana ujemanja krajse oblike po blokih")
    parser.add_argument("--expect-events", type=int, help="Pricakovano stevilo dogodkov z uporabo casov CAPTURE")
    parser.add_argument("--cpp-check", type=pathlib.Path, help="Ustvari static_assert teste dejanske C++ logike za vse bloke")
    args = parser.parse_args()
    header = (pathlib.Path(__file__).resolve().parents[1] / "include/s11_detector.h").read_text(encoding="utf-8")
    reference = [int(x) for x in re.findall(r"\d+", re.search(r"PATTERN_US\[\]\s*=\s*\{([^}]+)", header).group(1))]
    limits = {name: int(value) for name, value in re.findall(r"constexpr uint16_t (COMPACT_\w+_US) = (\d+);", header)}

    def within(value, name):
        return limits[f"COMPACT_{name}_MIN_US"] <= value <= limits[f"COMPACT_{name}_MAX_US"]

    def compact_match(pulses):
        values = [duration for _, duration in pulses]
        return (all(level == 1 - i % 2 for i, (level, _) in enumerate(pulses))
                and within(values[0], "SYNC_HIGH") and within(values[1], "SYNC_LOW")
                and all(within(values[i], "DATA_HIGH") for i in (2, 4, 6, 8))
                and all(within(values[i] + values[i + 1], "PAIR") for i in (2, 4, 6))
                and within(values[9], "GAP") and within(sum(values), "PERIOD"))

    def partial_compact_match(pulses):
        values = [duration for _, duration in pulses]
        return (all(level == 1 - i % 2 for i, (level, _) in enumerate(pulses))
                and within(values[9], "PARTIAL_FIRST_LOW")
                and within(values[10], "PARTIAL_HIGH")
                and within(values[11], "PARTIAL_LAST_LOW")
                and compact_match(pulses[:9] + [(0, sum(values[9:]))]))
    switch2_reference = [int(x) for x in re.findall(r"\d+", re.search(r"SWITCH2_PATTERN_US\[\]\s*=\s*\{([^}]+)", header).group(1))]
    switch2_compact = [int(x) for x in re.findall(r"\d+", re.search(r"SWITCH2_COMPACT_US\[\]\s*=\s*\{([^}]+)", header).group(1))]
    switch2_limits = {name: int(value) for name, value in re.findall(r"constexpr uint16_t (SWITCH2_\w+_US) = (\d+);", header)}

    def near_reference(pulses, reference):
        return all(level == 1 - i % 2 and duration > 0
                   and abs(duration - expected) <= max(30, (duration + expected) * 0.1)
                   for i, ((level, duration), expected) in enumerate(zip(pulses, reference)))

    def switch2_period(pulses):
        return switch2_limits["SWITCH2_PERIOD_MIN_US"] <= sum(d for _, d in pulses) <= switch2_limits["SWITCH2_PERIOD_MAX_US"]

    def switch2_compact_match(pulses):
        return (near_reference(pulses, switch2_compact)
                and switch2_limits["SWITCH2_GAP_MIN_US"] <= pulses[9][1] <= switch2_limits["SWITCH2_GAP_MAX_US"]
                and switch2_period(pulses))

    def switch2_partial_match(pulses):
        return (near_reference(pulses[:9], switch2_compact[:9])
                and pulses[9][0] == 0 and 1500 <= pulses[9][1] <= 1650
                and all(pulses[i][0] == 1 and pulses[i + 1][0] == 0
                        and 20 <= pulses[i][1] <= 60 and 70 <= pulses[i + 1][1] <= 115
                        and 115 <= pulses[i][1] + pulses[i + 1][1] <= 150 for i in range(10, 30, 2))
                and switch2_compact_match(pulses[:9] + [(0, sum(d for _, d in pulses[9:]))]))

    def flexible_length(pulses, reference):
        def near(actual, expected, ratio):
            return abs(actual - expected) <= 30 or abs(actual - expected) * ratio <= actual + expected

        observed = skipped = period = expected = 0
        while expected < len(reference):
            if (observed + 2 > len(pulses) or pulses[observed][0] != 1
                    or pulses[observed + 1][0] != 0 or pulses[observed][1] == 0
                    or pulses[observed + 1][1] == 0 or not near(pulses[observed][1], reference[expected], 10)):
                return 0
            period += pulses[observed][1] + pulses[observed + 1][1]
            expected += 1
            low = reference[expected]
            actual_low = pulses[observed + 1][1]
            while not near(actual_low, low, 100):
                if actual_low < low or expected + 2 >= len(reference) or reference[expected + 1] > 80:
                    return 0
                low += reference[expected + 1] + reference[expected + 2]
                expected += 2
                skipped += 1
            expected += 1
            observed += 2
        return observed if skipped and within(period, "PERIOD") else 0

    def robust_counts(pulses, source):
        ref = reference if source == 0 else switch2_reference
        partial_length = 12 if source == 0 else 30
        full = compact = flex = start = 0
        while start + 10 <= len(pulses):
            remaining = pulses[start:]
            consumed = 0
            if (len(remaining) >= len(ref) and near_reference(remaining[:len(ref)], ref)
                    and (source == 0 or switch2_period(remaining[:len(ref)]))):
                full += 1
                consumed = len(ref)
            elif (compact_match(remaining[:10]) if source == 0 else switch2_compact_match(remaining[:10])):
                compact += 1
                consumed = 10
            elif (len(remaining) >= partial_length and
                  (partial_compact_match(remaining[:12]) if source == 0 else switch2_partial_match(remaining[:30]))):
                compact += 1
                consumed = partial_length
            else:
                consumed = flexible_length(remaining, ref)
                flex += int(consumed > 0)
            start += consumed or 1
        return (full, compact, flex)

    raw = args.capture.read_text(encoding="utf-8-sig")
    clean = re.sub(r"^\s*\d{2}:\d{2}:\d{2}:\d{3}\s*->\s*", "", raw, flags=re.M)
    results = []
    compact_results = []
    switch2_results = []
    robust_results = []
    cpp_lines = ['#include "s11_detector.h"']
    for block in re.finditer(r"RAW_PACKET symbols=\d+ complete=\w+\s*([\s\S]*?)END_RAW_PACKET", clean):
        pulses = []
        for level, duration in re.findall(r"\b([01]):(\d+)\b", block.group(1)):
            if int(duration) == 0:
                break
            pulses.append((int(level), int(duration)))
        start = matches = 0
        while start + len(reference) <= len(pulses):
            # Neodvisna primerjava z dokumentirano toleranco; ne izvaja C++ kode.
            if all(level == 1 - i % 2 and abs(duration - expected) <= max(30, 0.2 * (duration + expected) / 2)
                   for i, (expected, (level, duration)) in enumerate(zip(reference, pulses[start:]))):
                matches += 1
                start += len(reference)
            else:
                start += 1
        results.append(matches)
        start = compact = 0
        while start + 10 <= len(pulses):
            if compact_match(pulses[start:start + 10]):
                compact += 1
                start += 10
            elif start + 12 <= len(pulses) and partial_compact_match(pulses[start:start + 12]):
                compact += 1
                start += 12
            else:
                start += 1
        compact_results.append(compact)
        start = full2 = compact2 = 0
        while start + 10 <= len(pulses):
            if (start + 42 <= len(pulses) and near_reference(pulses[start:start + 42], switch2_reference)
                    and switch2_period(pulses[start:start + 42])):
                full2 += 1
                start += 42
            elif switch2_compact_match(pulses[start:start + 10]):
                compact2 += 1
                start += 10
            elif start + 30 <= len(pulses) and switch2_partial_match(pulses[start:start + 30]):
                compact2 += 1
                start += 30
            else:
                start += 1
        switch2_results.append((full2, compact2))
        robust = [robust_counts(pulses, source) for source in (0, 1)]
        robust_results.append(robust)
        if args.cpp_check is not None and pulses:
            name = f"capture_{len(results)}"
            pairs = ",".join(f"{{{duration},{level}}}" for level, duration in pulses)
            cpp_lines.extend([
                f"constexpr s11::Pulse {name}[] = {{{pairs}}};",
                f'static_assert(s11::count_matches({name}, {len(pulses)}) == {matches}, "full block {len(results)}");',
                f'static_assert(s11::count_compact_matches({name}, {len(pulses)}) == {compact}, "compact block {len(results)}");',
                f'static_assert(s11::count_switch2_matches({name}, {len(pulses)}).full == {full2}, "switch2 full block {len(results)}");',
                f'static_assert(s11::count_switch2_matches({name}, {len(pulses)}).compact == {compact2}, "switch2 compact block {len(results)}");',
            ])
            for source, counts in enumerate(robust):
                for field, value in zip(("full", "compact", "flex"), counts):
                    cpp_lines.append(f'static_assert(s11::count_robust_matches({name}, {len(pulses)}, {source}).{field} == {value}, "robust source {source} {field} block {len(results)}");')
    if not results:
        raise SystemExit("ERROR: ni popolnih blokov RAW_PACKET")
    print(f"blocks={len(results)} matches={results} total={sum(results)} matching_blocks={sum(x > 0 for x in results)}")
    print(f"compact={compact_results} total={sum(compact_results)} accepted_blocks={sum(a > 0 or b > 0 for a, b in zip(results, compact_results))}")
    print("switch2 positive blocks=" + str([(i, f, c) for i, (f, c) in enumerate(switch2_results, 1) if f + c]))
    for source in (0, 1):
        print(f"robust switch{source + 1} totals=" + str(tuple(sum(r[source][i] for r in robust_results) for i in range(3))))
        print(f"flex switch{source + 1} positive blocks=" + str([(i, r[source][2]) for i, r in enumerate(robust_results, 1) if r[source][2]]))
    if args.expect is not None:
        expected = [int(x) for x in args.expect.split(",")]
        if results != expected:
            raise SystemExit(f"FAIL: expected={expected}")
        print("PASS")
    if args.expect_compact is not None:
        expected = [int(x) for x in args.expect_compact.split(",")]
        if compact_results != expected:
            raise SystemExit(f"FAIL compact: expected={expected}")
        print("PASS compact")
    if args.expect_events is not None:
        times = [int(x) for x in re.findall(r"CAPTURE uptime_ms=(\d+)", raw)]
        if len(times) != len(results):
            raise SystemExit("FAIL: casovne oznake CAPTURE se ne ujemajo s stevilom blokov")
        silence = int(re.search(r"REPEAT_SILENCE_MS = (\d+)", header).group(1))
        last = [None, None]
        events = [0, 0]
        cpp_lines.extend(["constexpr unsigned replay_events() {", "s11::PressGate gates[2]; unsigned events = 0;"])
        for robust, now in zip(robust_results, times):
            for source, matches in enumerate(map(sum, robust)):
                if matches:
                    if last[source] is None or ((now - last[source]) & 0xFFFFFFFF) >= silence:
                        events[source] += 1
                    last[source] = now
                cpp_lines.append(f"if (gates[{source}].observe({matches}, {now}u)) ++events;")
        cpp_lines.extend(["return events; }", f'static_assert(replay_events() == {args.expect_events}, "event replay failed");'])
        if sum(events) != args.expect_events:
            raise SystemExit(f"FAIL events: expected={args.expect_events}, actual={sum(events)}")
        print(f"PASS events={sum(events)} switch1={events[0]} switch2={events[1]}")
    if args.cpp_check is not None:
        args.cpp_check.write_text("\n".join(cpp_lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
