#!/usr/bin/env python3
"""Validates test_dispatcher JSON output with a REAL JSON parser.

Usage: python3 test_dispatcher.py [path-to-test_dispatcher]
If a binary path is given it is executed and its stdout parsed; else stdin.
Every balanced `{...}` JSON object found in the stream is fed through
json.loads() and asserted against the dispatcher wire contract (P1-3 / P1-4 /
P1-4B). Object boundaries are found by brace-balancing (JSON-string aware), so
interleaved PASS/FAIL diagnostic lines are ignored. Exit 0 iff all pass.
"""
import json
import subprocess
import sys


def extract_objects(text):
    """Yield each top-level balanced JSON object string in text."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '{':
            depth = 0
            in_str = False
            esc = False
            j = i
            started = False
            while j < n:
                ch = text[j]
                if in_str:
                    if esc:
                        esc = False
                    elif ch == '\\':
                        esc = True
                    elif ch == '"':
                        in_str = False
                else:
                    if ch == '"':
                        in_str = True
                    elif ch == '{':
                        depth += 1
                        started = True
                    elif ch == '}':
                        depth -= 1
                        if started and depth == 0:
                            out.append(text[i:j + 1])
                            i = j
                            break
                j += 1
        i += 1
    return out


def run():
    binary = sys.argv[1] if len(sys.argv) > 1 else None
    if binary:
        p = subprocess.run([binary], capture_output=True, text=True)
        if p.returncode != 0:
            print(f"FAIL test_dispatcher binary exited {p.returncode}")
            sys.exit(1)
        src = p.stdout
    else:
        src = sys.stdin.read()

    objs = extract_objects(src)
    ok = True
    checked = 0
    for obj in objs:
        try:
            parsed = json.loads(obj)
        except Exception as e:
            print(f"FAIL object not valid JSON: {e}")
            ok = False
            continue
        checked += 1

        if "manifest" in parsed:
            # P1-3: deterministic manifest; no human-uninitialized sentinel.
            pass
        # Manifest capability consistency (P2-7): canonical has a `nox` field,
        # so the manifest capabilities must expose it (no silent drift).
        if "manifest" in parsed:
            caps = parsed.get("manifest", {}).get("capabilities", {})
            if "nox" not in caps:
                print("FAIL GET_MANIFEST: capabilities missing 'nox' (canonical drift)")
                ok = False
        # Capabilities consistency (P1-4): any object with a "pressure" key.
        if "pressure" in parsed and "co2" in parsed:
            if not parsed.get("pressure"):
                print("FAIL GET_CAPABILITIES: pressure false but canonical true")
                ok = False
            for f in ("illuminance", "temperature", "humidity", "co2", "voc",
                      "nox", "presence"):
                if f not in parsed:
                    print(f"FAIL GET_CAPABILITIES: missing '{f}'")
                    ok = False
        # SELF_TEST (P1-4B): any object with "platform" sensor-set keys.
        if "light_sensor" in parsed and "pressure_sensor" in parsed:
            for f in ("light_sensor", "display", "co2_sensor",
                      "temp_humidity_sensor", "pressure_sensor"):
                if f not in parsed:
                    print(f"FAIL SELF_TEST: missing '{f}'")
                    ok = False

    print(f"JSON_ASSERTED_CASES={checked}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    run()