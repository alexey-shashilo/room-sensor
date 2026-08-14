#!/usr/bin/env python3
"""Real JSON-parser validation of schema-v3 telemetry serialization.

Reads the output of the test_telemetry_json host binary on stdin and validates
every serialized payload with a real JSON parser (json.loads). Asserts the
decoded values and the CO2 finite/range invalid semantics.

Usage: test_telemetry_json.py <path-to-test_telemetry_json-binary>  (or read via
run/CTest)  -- the binary path is argv[1]; default "test_telemetry_json".
"""
import json
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "test_telemetry_json"

def case_block(out):
    """Yield (case_num, json_text) for each 'CASE n' block."""
    blocks = {}
    cur = None
    buf = []
    for line in out.splitlines(keepends=True):
        if line.startswith("CASE "):
            cur = line.strip().split()[1]
            buf = []
        else:
            buf.append(line)
        blocks[cur] = "".join(buf) if cur else ""
    return blocks

def main():
    proc = subprocess.run([BIN], capture_output=True, text=True)
    if proc.returncode != 0:
        print("FATAL: host binary failed rc=%d\n%s" % (proc.returncode, proc.stderr))
        return 1
    out = proc.stdout
    failures = 0
    cases = {}

    # split into CASE blocks by scanning lines
    cur = None
    cur_buf = []
    for line in out.splitlines(keepends=True):
        if line.startswith("CASE "):
            if cur is not None:
                cases[cur] = "".join(cur_buf)
            cur = line.strip().split()[1]
            cur_buf = []
        else:
            cur_buf.append(line)
    if cur is not None:
        cases[cur] = "".join(cur_buf)

    def check(cond, msg):
        nonlocal failures
        if cond:
            print("  PASS: %s" % msg)
        else:
            print("  FAIL: %s" % msg)
            failures += 1

    def parse_case(n):
        raw = cases.get(str(n))
        if raw is None:
            check(False, "case %d present" % n)
            return None
        if raw.strip() == "SERIALIZE_ERROR":
            check(False, "case %d serialized OK" % n)
            return None
        try:
            return json.loads(raw)
        except Exception as e:
            check(False, "case %d parses as JSON (%s)" % (n, e))
            return None

    print("=== JSON validity + parsed values ===")

    # Case 1: golden, all valid
    d = parse_case(1)
    if d is not None:
        check(d.get("schema") == 3, "case1 schema==3")
        bid = d.get("boot_id")
        check(isinstance(bid, str) and len(bid) == 16, "case1 boot_id length==16")
        check(isinstance(bid, str) and all(c in "0123456789abcdef" for c in bid),
              "case1 boot_id chars in [0-9a-f]")
        check(bid == "0123456789abcdef", "case1 boot_id == 0123456789abcdef")
        room = d.get("room", {})
        check(room.get("co2_ppm", {}).get("value") == 1006, "case1 co2.value==1006")
        check(room.get("co2_ppm", {}).get("state") == "valid", "case1 co2.state valid")
        check(abs(room.get("scd41_temperature_c", {}).get("value") - 27.1) < 0.01,
              "case1 scd41_temperature_c.value==27.1")
        check(abs(room.get("scd41_humidity_pct", {}).get("value") - 49.0) < 0.01,
              "case1 scd41_humidity_pct.value==49.0")
        check(abs(room.get("illuminance_lux", {}).get("value") - 72.4) < 0.01,
              "case1 illuminance_lux.value==72.4")
        check("value" in room.get("scd41_humidity_pct", {}), "case1 humidity has value")

    # Case 2: all invalid
    d = parse_case(2)
    if d is not None:
        room = d.get("room", {})
        check(room.get("co2_ppm", {}).get("state") == "invalid", "case2 co2 invalid")
        check("value" not in room.get("co2_ppm", {}), "case2 co2 has NO value (never 0)")
        check(room.get("scd41_temperature_c", {}).get("state") == "invalid", "case2 T invalid")
        check(room.get("scd41_humidity_pct", {}).get("state") == "invalid", "case2 RH invalid")
        check("value" not in room.get("scd41_temperature_c", {}), "case2 T no value")
        check("value" not in room.get("scd41_humidity_pct", {}), "case2 RH no value")

    # Case 3: mixed
    d = parse_case(3)
    if d is not None:
        room = d.get("room", {})
        check(room.get("co2_ppm", {}).get("value") == 500, "case3 co2==500")
        check(room.get("scd41_temperature_c", {}).get("state") == "invalid", "case3 T invalid")
        check(room.get("scd41_humidity_pct", {}).get("value") == 30, "case3 RH==30")

    # Case 4: max legal CO2
    d = parse_case(4)
    if d is not None:
        check(d.get("room", {}).get("co2_ppm", {}).get("value") == 40000, "case4 co2==40000 valid")
        check(d.get("room", {}).get("co2_ppm", {}).get("state") == "valid", "case4 co2 state valid")

    # Cases 5-9: out-of-range / non-finite CO2 must be invalid, never numeric 0
    for n, nm in [(5, "co2>40000"), (6, "co2 negative"), (7, "co2 NaN"),
                  (8, "co2 +Inf"), (9, "co2 -Inf")]:
        d = parse_case(n)
        if d is not None:
            cp = d.get("room", {}).get("co2_ppm", {})
            check(cp.get("state") == "invalid", "case%d %s -> invalid" % (n, nm))
            check("value" not in cp, "case%d %s -> no numeric value" % (n, nm))

    # Case 10: full-mask boot_id
    d = parse_case(10)
    if d is not None:
        bid = d.get("boot_id")
        check(isinstance(bid, str) and len(bid) == 16, "case10 boot_id length==16")
        check(bid == "ffffffffffffffff", "case10 boot_id == ffffffffffffffff")
        check(all(c in "0123456789abcdef" for c in bid), "case10 boot_id chars in [0-9a-f]")

    # Golden parsed assertions
    if parse_case(1) is not None:
        print("  golden parsed: schema=%d co2=%s state=%s" % (
            parse_case(1).get("schema"),
            parse_case(1).get("room", {}).get("co2_ppm", {}).get("value"),
            parse_case(1).get("room", {}).get("co2_ppm", {}).get("state")))

    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()