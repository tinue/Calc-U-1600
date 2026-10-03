#!/usr/bin/env python3
"""BASIC capacity edge: loads listings of exactly free-6 .. free+2 bytes of
payload (padded with REM lines) through the ROM and through the loader, and
prints what each path did. free = MEM after NEW0 (gen_pc1500.py prints it).

  python3 dev/loader-matrix/edge_pc1500.py <model> <card|none> <free>

Needs the harness (dev/loader-matrix/build.sh). Stdlib only; run from the
repo root.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import gen_pc1500 as g  # noqa: E402

OUT = os.path.join(g.OUT, "edge")
BAS, BBIN = os.path.join(OUT, "e.bas"), os.path.join(OUT, "e.bbin")


def size(text):
    with open(BAS, "w") as f:
        f.write(text)
    return g.convert(BAS, BBIN)


def make(n):
    """Writes a listing whose tokenized payload is exactly n bytes."""
    lines = 1
    while size(g.listing(lines + 1)) <= n - 100:
        lines += 1
    text, ln = g.listing(lines), 10 + 10 * lines
    while size(text) < n - 80:
        text += f"{ln} REM " + "Y" * 10 + "\n"
        ln += 10
    for k in range(75):
        p = size(text + f"{ln} REM " + "X" * k + "\n")
        if p == n:
            return True
        if p > n:
            return False
    return False


def main():
    model, card, free = sys.argv[1], sys.argv[2], int(sys.argv[3])
    os.makedirs(OUT, exist_ok=True)
    for n in range(free - 6, free + 3):
        if not make(n):
            print(n, "no exact listing")
            continue
        r = subprocess.run([g.HARNESS, "--model", model, "--card", card, "--kind", "basic",
                            "--stream", BBIN, "--loader-file", BAS], capture_output=True, text=True).stdout
        lines = r.splitlines()
        print(f"payload {n} (free {free}): {lines[0][:40]} | {lines[1][:110]}")


if __name__ == "__main__":
    main()
