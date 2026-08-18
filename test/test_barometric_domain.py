#!/usr/bin/env python3
"""Barometric domain telemetry JSON validation (Phase 17.7B).

Reads the output of the test_barometric_domain host binary and parses each
'CASE n' telemetry block with a real JSON parser (json.loads), asserting:
  - generic barometric_sensor naming bmp390/bmp380/none
  - generic numeric validity follows the provider
  - legacy bmp390_* valid ONLY under provider BMP390
  - BMP380 data NEVER serialized under bmp390 names (critical negative control)
  - provider NONE emits no fabricated numeric values

Usage: test_barometric_domain.py <path-to-binary>
"""
import json, re, sys, subprocess

BIN = sys.argv[1] if len(sys.argv) > 1 else "test_barometric_domain"

def check(cond, name):
    if cond:
        sys.stdout.write("  PASS: %s\n" % name)
    else:
        sys.stdout.write("  FAIL: %s\n" % name)
        sys.exit(1)

def cases(output):
    blocks = re.split(r'CASE (\d+)\n', output)
    out = {}
    for i in range(1, len(blocks), 2):
        out[int(blocks[i])] = blocks[i+1]
    return out

def parse(cs, n):
    if str(n) not in {str(k) for k in cs}:
        check(False, "case %d present" % n)
    raw = cs[n]
    # Trim trailing PASS/FAIL harness lines and strip; keep only the JSON object.
    raw = raw[:raw.rstrip().rfind('}')+1].strip()
    try:
        return json.loads(raw)
    except Exception as e:
        check(False, "case %d parses as JSON (%s)" % (n, e))

def main():
    proc = subprocess.run([BIN], capture_output=True, text=True)
    if proc.returncode != 0:
        check(False, "binary exits 0 (stderr=%s)" % proc.stderr.strip())
    cs = cases(proc.stdout)

    # Case A: BMP390 provider
    d = parse(cs, 1); room = d["room"]
    check(room["barometric_sensor"] == "bmp390", "case1 sensor==bmp390")
    check(room["barometric_pressure_pa"]["state"] == "valid", "case1 pressure valid")
    check(room["barometric_temperature_c"]["state"] == "valid", "case1 temp valid")
    check(room["barometric_pressure_pa"]["value"] == 101325.0, "case1 pressure value")
    check(room["bmp390_pressure_pa"]["state"] == "valid", "case1 legacy bmp390 valid")
    check(room["bmp390_temperature_c"]["state"] == "valid", "case1 legacy bmp390 temp valid")

    # Case B: BMP380 only -> legacy bmp390 INVALID (critical negative)
    d = parse(cs, 2); room = d["room"]
    check(room["barometric_sensor"] == "bmp380", "case2 sensor==bmp380")
    check(room["barometric_pressure_pa"]["state"] == "valid", "case2 pressure valid")
    check(room["barometric_temperature_c"]["state"] == "valid", "case2 temp valid")
    check(room["bmp390_pressure_pa"]["state"] == "invalid", "case2 bmp390 INVALID (no BMP380 as BMP390)")
    check(room["bmp390_temperature_c"]["state"] == "invalid", "case2 bmp390 temp INVALID")
    check("value" not in room["bmp390_pressure_pa"], "case2 BMP380 not under bmp390 name")

    # Case C: neither -> provider none, no fabricated zeros
    d = parse(cs, 3); room = d["room"]
    check(room["barometric_sensor"] == "none", "case3 sensor==none")
    check(room["barometric_pressure_pa"]["state"] == "invalid", "case3 pressure invalid")
    check(room["barometric_temperature_c"]["state"] == "invalid", "case3 temp invalid")
    check("value" not in room["barometric_pressure_pa"], "case3 no fabricated numeric pressure")
    check("value" not in room["barometric_temperature_c"], "case3 no fabricated numeric temp")

    # Case D: BMP390 provider (deterministic win)
    d = parse(cs, 4); room = d["room"]
    check(room["barometric_sensor"] == "bmp390", "case4 sensor==bmp390 (win)")
    check(room["bmp390_pressure_pa"]["state"] == "valid", "case4 bmp390 legacy valid")

    # Case E: fallback to BMP380 (BMP390 stale)
    d = parse(cs, 5); room = d["room"]
    check(room["barometric_sensor"] == "bmp380", "case5 sensor==bmp380 (fallback)")
    check(room["bmp390_pressure_pa"]["state"] == "invalid", "case5 legacy bmp390 invalid")

    # Case F: BMP390 recovered -> preferred again
    d = parse(cs, 6); room = d["room"]
    check(room["barometric_sensor"] == "bmp390", "case6 sensor==bmp390 (recovered)")
    check(room["bmp390_pressure_pa"]["state"] == "valid", "case6 legacy bmp390 valid again")

    sys.stdout.write("RESULT: barometric JSON validations passed\n")

if __name__ == "__main__":
    main()