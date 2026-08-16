#!/usr/bin/env python3
"""Phase 15 command-path-in-simulation validation.

The C harness test_sim_command boots the production portable core under virtual
time, injects GET_MANIFEST / GET_CAPABILITIES / SELF_TEST through the production
software command path, and emits each response as:

    RSP <n>
    <response JSON>
    END

This script parses every response with a REAL JSON parser and asserts the
expected shape per command, proving the software command path produces valid
responses inside a whole-device simulation. Physical command ingress (UART/
MQTT) is NOT implemented in this phase.
"""
import json
import subprocess
import sys


def parse_stream(text):
    frames = []
    cur = None
    buf = []
    for raw in text.splitlines():
        line = raw.rstrip("\n")
        if line.startswith("RSP "):
            cur = line.split(" ", 1)[1].strip()
            buf = []
        elif line == "END":
            frames.append((cur, "\n".join(buf)))
            cur = None
            buf = []
        else:
            buf.append(line)
    return frames


def main():
    if len(sys.argv) < 2:
        print("usage: test_command_sim.py <harness-exe>")
        return 1
    proc = subprocess.run([sys.argv[1]], capture_output=True, text=True, cwd=".")
    frames = parse_stream(proc.stdout)
    if len(frames) < 3:
        print("ERROR: expected >=3 command responses, got %d" % len(frames))
        return 1

    seen_manifest = seen_capabilities = seen_self_test = False
    for _, payload in frames:
        doc = json.loads(payload)  # strict: duplicate keys raise
        assert isinstance(doc, dict), "command response must be a JSON object"
        assert doc.get("status") == "ok", "command response status must be ok"
        if "manifest" in doc:
            m = doc["manifest"]
            assert isinstance(m, dict) and "device_id" in m and "device_type" in m, \
                "manifest response malformed"
            seen_manifest = True
        if "command_schema" in doc and "co2" in doc and "voc" in doc:
            seen_capabilities = True
        if "platform" in doc and "i2c" in doc and "co2_sensor" in doc:
            seen_self_test = True

    if not (seen_manifest and seen_capabilities and seen_self_test):
        print("ERROR: missing one or more command responses "
              "(manifest=%s caps=%s selftest=%s)" %
              (seen_manifest, seen_capabilities, seen_self_test))
        return 1
    print("PASS: GET_MANIFEST / GET_CAPABILITIES / SELF_TEST responses "
          "parse as valid JSON (%d frames)" % len(frames))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())