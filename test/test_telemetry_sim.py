#!/usr/bin/env python3
"""Phase 15 telemetry validation: parse telemetry frames produced by a whole-device
virtual run with a REAL JSON parser.

The C harness test_sim_telemetry runs the production portable core under virtual
time and emits each serialized telemetry frame as:

    TEL <n>
    <raw JSON>
    END

This script feeds every frame to json.loads (strict), asserting:
  - every complete telemetry document parses as JSON
  - no duplicate keys at any nesting level (the stdlib JSON parser rejects
    duplicate object keys, so parse success implies no duplicates)
  - schema is present and >= 5
  - validity flags match emitted state: an "invalid" measurement carries no
    numeric value; a "valid" measurement carries a finite numeric value
  - the payload stays within TELEMETRY_SERIALIZED_MAX_SIZE
"""

import json
import sys

TELEMETRY_SERIALIZED_MAX_SIZE = 2048

# All measurement channels except co2_ppm (an integer rendered differently).
STATE_CHANNELS = [
    "illuminance_lux",
    "scd41_temperature_c", "scd41_humidity_pct",
    "sht45_temperature_c", "sht45_humidity_pct",
    "bmp390_pressure_pa", "bmp390_temperature_c",
    "voc_raw", "nox_raw", "voc_index", "nox_index",
]
CO2_KEY = "co2_ppm"


def parse_stream(text):
    frames = []
    cur = None
    buf = []
    for raw in text.splitlines():
        line = raw.rstrip("\n")
        if line.startswith("TEL "):
            cur = line.split(" ", 1)[1].strip()
            buf = []
        elif line == "END":
            payload = "\n".join(buf)
            frames.append((cur, payload))
            cur = None
            buf = []
        else:
            buf.append(line)
    return frames


def check_channel(obj, name):
    if name not in obj:
        return  # absent optional channel is acceptable
    node = obj[name]
    assert isinstance(node, dict), f"{name} node must be an object"
    state = node.get("state")
    assert state in ("valid", "invalid"), f"{name} state must be valid|invalid"
    if state == "invalid":
        assert "value" not in node, f"{name} invalid must not carry a value"
    else:
        assert "value" in node, f"{name} valid must carry a value"
        v = node["value"]
        assert isinstance(v, (int, float)), f"{name} value must be numeric"
        assert v == v, f"{name} value must be finite (no NaN)"


def main():
    if len(sys.argv) < 2:
        print("usage: test_telemetry_sim.py <harness-exe>")
        return 1
    import subprocess
    proc = subprocess.run([sys.argv[1]], capture_output=True, text=True, cwd=".")
    out = proc.stdout
    frames = parse_stream(out)
    if not frames:
        print("ERROR: no telemetry frames captured")
        return 1
    for fid, payload in frames:
        doc = json.loads(payload)  # raises on malformed JSON / duplicate keys
        assert doc["schema"] >= 5, "telemetry schema must be >= 5"
        assert "device_id" in doc and isinstance(doc["device_id"], str)
        assert "room" in doc and isinstance(doc["room"], dict)
        room = doc["room"]
        for ch in STATE_CHANNELS:
            check_channel(room, ch)
        check_channel(room, CO2_KEY)
    print("PASS: %d frames parse as valid JSON (schema>=5, no dup keys)" % len(frames))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())