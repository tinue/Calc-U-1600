#!/usr/bin/env python3
"""PC-1600 BASIC capacity edge: loads listings of exactly free-6 .. free+2
bytes of payload (padded with REM lines) through the ROM and through the
loader, and prints what each path did. free = MEM after NEW0
(gen_pc1600.py prints it).

  python3 dev/loader-matrix/edge_pc1600.py <mode> <slot1> <slot2> <com1|ce158> <free>

COM1: a PC-1600 listing (sde --device pc1600); CE-158: a PC-1500 one.
Needs the harness (dev/loader-matrix/build_pc1600.sh). Stdlib only; run
from the repo root.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import gen_pc1500 as g  # noqa: E402
import gen_pc1600 as g16  # noqa: E402

OUT = os.path.join(g.OUT, "edge1600")
BAS, BBIN = os.path.join(OUT, "e.bas"), os.path.join(OUT, "e.bbin")


def main():
    mode, s1, s2, via, free = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
    device, header = ("pc1600", 16) if via == "com1" else ("pc1500", 27)
    listing = g16.pc1600_listing if via == "com1" else g.listing
    os.makedirs(OUT, exist_ok=True)

    def size(text):
        with open(BAS, "w") as f:
            f.write(text)
        return g16.convert(BAS, BBIN, "--device", device) - header

    def make(n):
        lines = 1
        while size(listing(lines * 2)) <= n - 100:
            lines *= 2
        while size(listing(lines + 1)) <= n - 100:
            lines += 1
        text, ln = listing(lines), 10 + 10 * lines
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

    loader = BBIN if (mode == "1" and via == "com1") else BAS
    for n in range(free - 6, free + 3):
        if not make(n):
            print(n, "no exact listing")
            continue
        r = subprocess.run([g16.HARNESS, "--mode", mode, "--slot1", s1, "--slot2", s2, "--transport", via,
                            "--kind", "basic", "--stream", BBIN, "--loader-file", loader],
                           capture_output=True, text=True).stdout
        lines = r.splitlines()
        res = next((l for l in lines if l.startswith("RESULT")), "RESULT ?")
        print(f"payload {n} (free {free}): {lines[0][:40]} | {lines[1][:100]} | {res}")


if __name__ == "__main__":
    main()
