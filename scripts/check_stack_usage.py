#!/usr/bin/env python3
"""Project-owned stack-usage gate for -fstack-usage .su files.

Each GCC .su line has the form:

    <source>:<line>:<col>:<function>\t<bytes>\t<static|dynamic>

This script reports the top-N project-owned functions by static (frame-only)
stack usage and enforces a maximum static-frame threshold. Vendor HAL/BSP noise
(anything whose source path is NOT under Application/User) is excluded.

Exit status:
  0  gate satisfied (project-owned frames found and all below threshold)
  2  FAIL-CLOSED: no project-owned .su frames found (measurement unavailable)
  1  a project-owned static frame exceeds the threshold

DOCUMENTED LIMITATION: per-function STATIC usage is the frame only. It is NOT
the worst-case call-chain total (interrupt + call depth). For a stack budget
treat the largest call-chain sum or ISR+main sum, not this number alone.

No external dependencies (standard library only).
"""

import argparse
import os
import re
import sys

# GCC static field is the third tab-separated token of a non-dynamic row.
_LINE_RE = re.compile(r"^(.*?):\d+:\d+:([^\t]+)\t(\d+)\tstatic$")

PROJECT_MARKER = "Application/User"


def iter_rows(su_files):
    """Yield (source, function, static_bytes) for project-owned static rows.

    Rows without a usable format are ignored (not counted as a measurement);
    a completely empty result triggers fail-closed.
    """
    for path in su_files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    m = _LINE_RE.match(line.rstrip("\n"))
                    if not m:
                        continue
                    src, func, bytes_s = m.group(1), m.group(2), m.group(3)
                    # Slash normalization so Windows CI (backslash) still matches.
                    if PROJECT_MARKER not in src.replace("\\", "/"):
                        continue
                    yield src, func, int(bytes_s)
        except OSError:
            continue


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+", help="*.su files to analyse")
    ap.add_argument("-t", "--threshold", type=int, default=2048,
                    help="max static frame (bytes) per project-owned function")
    ap.add_argument("-n", "--top", type=int, default=15,
                    help="number of top entries to print")
    args = ap.parse_args(argv)

    rows = sorted(iter_rows(args.files), key=lambda r: r[2], reverse=True)
    if not rows:
        print("FAIL-CLOSED: no project-owned .su static frames found "
              "(stack-usage measurement unavailable)", file=sys.stderr)
        return 2

    print(f"== top-{args.top} project-owned static stack usage "
          f"(bytes static = frame only) ==")
    for src, func, b in rows[: args.top]:
        print(f"{b:6d}  {func}  ({os.path.basename(src)})")

    worst = rows[0][2]
    print(f"worst static frame = {worst} B")
    if worst > args.threshold:
        print(f"ERROR: project-owned static frame of {worst} B exceeds "
              f"threshold {args.threshold} B", file=sys.stderr)
        return 1
    print(f"PASS: worst project-owned static frame = {worst} B "
          f"(<= {args.threshold} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())