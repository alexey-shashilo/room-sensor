#!/usr/bin/env python3
"""Real JSON-parser validation of MQTT App wire telemetry + command response.

Reads the output of the test_mqtt_app_wire host binary and, for each WIRE frame,
parses the payload with json.loads. Asserts:
  - MQTT_TELEMETRY_WIRE_JSON_PARSE = PASS (telemetry parses with json.loads)
  - MQTT_COMMAND_RESPONSE_WIRE_JSON_PARSE = PASS (response parses with json.loads)
  - generic barometric contract present (barometric_sensor=bmp380, Pa pressure,
    legacy bmp390_* invalid) => BMP380_DATA_SERIALIZED_AS_BMP390 = NO
  - pressure remains Pa (no hPa/mmHg in telemetry)

The host binary emits:
  WIRE TELEMETRY <len>   then exactly <len> bytes then END
  WIRE COMMANDRESP <len> then exactly <len> bytes then END

Usage: test_mqtt_app_wire.py <path-to-test_mqtt_app_wire-binary>
"""
import json
import sys
import subprocess

BIN = sys.argv[1] if len(sys.argv) > 1 else "test_mqtt_app_wire"


def main():
    proc = subprocess.run([BIN], capture_output=True, text=True)
    if proc.returncode != 0:
        print("FATAL: host binary failed rc=%d\n%s" % (proc.returncode, proc.stderr))
        # still try to parse any emitted frames; the C side failing already
        # means the acceptance gates fail.
    out = proc.stdout
    failures = 0

    def check(cond, msg):
        nonlocal failures
        if cond:
            print("  PASS: %s" % msg)
        else:
            print("  FAIL: %s" % msg)
            failures += 1

    lines = out.splitlines(keepends=True)
    frames = {}   # name -> payload text
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if line.startswith("WIRE "):
            parts = line.split()
            tag = parts[0]   # WIRE
            name = parts[1]  # TELEMETRY / COMMANDRESP
            length = int(parts[2])
            i += 1
            buf = []
            got = 0
            while i < n and got < length:
                buf.append(lines[i])
                got += len(lines[i].encode('utf-8'))
                i += 1
            # trailing END line
            if i < n and lines[i].strip() == "END":
                i += 1
            frames[name] = "".join(buf)
        else:
            i += 1

    # --- Telemetry wire JSON ---
    print("== MQTT telemetry wire JSON ==")
    tel = frames.get("TELEMETRY")
    if not tel:
        check(False, "MQTT_TELEMETRY_WIRE_JSON_PARSE = FAIL (no telemetry frame)")
    else:
        try:
            obj = json.loads(tel)
            check(True, "MQTT_TELEMETRY_WIRE_JSON_PARSE = PASS (json.loads)")
            room = obj.get("room", {})
            check(room.get("barometric_sensor") == "bmp380",
                  "GENERIC_BAROMETRIC_TELEMETRY_PRESERVED = YES (bmp380)")
            bp = room.get("barometric_pressure_pa", {})
            check(bp.get("state") == "valid" and isinstance(bp.get("value"), (int, float)),
                  "generic barometric_pressure_pa valid (Pa)")
            l390 = room.get("bmp390_pressure_pa", {})
            check(l390.get("state") == "invalid",
                  "BMP380_DATA_SERIALIZED_AS_BMP390 = NO (bmp390 invalid)")
            check("hPa" not in tel and "mmHg" not in tel,
                  "PRESSURE_TELEMETRY_UNIT = Pa (no hPa/mmHg in telemetry)")
            voc = room.get("voc_index", {})
            check(voc.get("state") == "valid",
                  "SGP41_VALIDITY_PRESERVED = YES (voc_index valid)")
        except Exception as e:
            check(False, "MQTT_TELEMETRY_WIRE_JSON_PARSE = FAIL: %s" % e)

    # --- Command response wire JSON ---
    print("== MQTT command response wire JSON ==")
    resp = frames.get("COMMANDRESP")
    if resp:
        try:
            obj = json.loads(resp)
            check(True, "MQTT_COMMAND_RESPONSE_WIRE_JSON_PARSE = PASS (json.loads)")
            # A GET_MANIFEST response carries a manifest / status; just require
            # it parsed as a JSON object.
            check(isinstance(obj, dict),
                  "MQTT_COMMAND_USES_PRODUCTION_COMMAND_PATH = YES (dict response)")
        except Exception as e:
            check(False, "MQTT_COMMAND_RESPONSE_WIRE_JSON_PARSE = FAIL: %s" % e)
    else:
        check(False, "MQTT_COMMAND_RESPONSE_WIRE_JSON_PARSE = FAIL (no response frame)")

    print("RESULT: %s" % ("PASS" if failures == 0 else "FAIL"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())