#!/usr/bin/env python3
"""Writes the PC-1500(A) loader-matrix programs and manifest.

For every model x card it asks the harness (--probe) where BASIC starts and
where RAM ends, then writes a BASIC listing and a machine-code block per size
tier into headless/loader-matrix/progs/, each turned into what `sde put`
sends by `sde convert`. The manifest (headless/loader-matrix/manifest.tsv)
lists one harness run per line. Stdlib only; run from the repo root.
"""

import os
import re
import subprocess
import sys

OUT = "headless/loader-matrix"
PROGS = os.path.join(OUT, "progs")
HARNESS = os.path.join(OUT, "pc1500_loader_matrix")
HEADER = 27

MODELS = ["PC-1500", "PC-1500A"]
CARDS = ["none", "CE-151", "CE-155", "CE-161"]
TIERS = ["S", "M", "L", "XL", "OVER"]


def probe(model, card):
    out = subprocess.run([HARNESS, "--probe", "--model", model, "--card", card],
                         check=True, capture_output=True, text=True).stdout
    m = re.search(r"BASPRG_ST=([0-9A-F]+) .* RAM_END=([0-9A-F]+) span=(\d+)", out)
    return int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))


def target(tier, free):
    return {"S": 64, "M": free // 3, "L": free * 2 // 3, "XL": free - 100, "OVER": free + 256}[tier]


def basic_line(n, i):
    """Line n, the i-th generated statement: a deterministic mix."""
    k = i % 10
    word = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26]
    if k == 0:
        return f'{n} PRINT "LINE {i} {word * 8}";A$'
    if k == 1:
        return f"{n} FOR I=1 TO {i % 50 + 2}:A=A+I*{i % 7 + 1}:NEXT I"
    if k == 2:
        return f"{n} IF A>{i} THEN B=B-1:GOTO {n + 10}"
    if k == 3:
        return f'{n} A$="{word * 5}"+STR$ B:B=LEN A$+{i}'
    if k == 4:
        return f'{n} DATA {i},{i * 3},{-i},"{word}{word}",1.5E{i % 9}'
    if k == 5:
        return f"{n} REM GENERATED {word * 12} {i}"
    if k == 6:
        return f"{n} C=SIN ({i})+COS {i % 90}*SQR {i}-INT ({i}/3)"
    if k == 7:
        return f'{n} B$=MID$ (A$,{i % 5 + 1},2)+LEFT$ ("{word * 4}",{i % 4 + 1})'
    if k == 8:
        return f"{n} POKE &{0x7900 + i % 16:04X},{i % 256}:D=PEEK &{0x7900 + i % 16:04X}"
    return f"{n} WAIT {i % 64}:BEEP 1,{i % 255},{i % 99 + 1}"


def listing(lines):
    return "".join(basic_line(10 + 10 * i, i) + "\n" for i in range(lines))


def convert(src, dst, *extra):
    if os.path.exists(dst):
        os.remove(dst)
    subprocess.run(["sde", "convert", *extra, src, dst], check=True, capture_output=True)
    return os.path.getsize(dst) - HEADER


def write_basic(base, size):
    """Largest listing whose tokenized payload is <= size (at least one line)."""
    bas, bbin = base + ".bas", base + ".bbin"
    lo, hi = 1, 1
    def payload(n):
        with open(bas, "w") as f:
            f.write(listing(n))
        return convert(bas, bbin)
    while payload(hi) <= size:
        lo, hi = hi, hi * 2
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if payload(mid) <= size:
            lo = mid
        else:
            hi = mid
    return payload(lo), bas, bbin


def write_ml(base, size, load):
    raw, ce = base + ".bin", base + ".ce158.bin"
    data, x = bytearray(), 0xACE1
    for _ in range(size):  # 16-bit Galois LFSR
        x = (x >> 1) ^ (0xB400 if x & 1 else 0)
        data.append(x & 0xFF)
    with open(raw, "wb") as f:
        f.write(data)
    if os.path.exists(ce):
        os.remove(ce)
    subprocess.run(["sde", "convert", "--start-address", f"{load:04X}", raw, ce], check=True, capture_output=True)
    return size, ce, ce


def main():
    os.makedirs(PROGS, exist_ok=True)
    rows = []
    for model in MODELS:
        for card in CARDS:
            st, ram_end, span = probe(model, card)
            free = span - 1  # MEM after NEW0
            for tier in TIERS:
                want = target(tier, free)
                tag = f"{model}_{card}_{tier}".replace("-", "")
                size, loader, stream = write_basic(os.path.join(PROGS, tag + "_basic"), want)
                rows.append((model, card, "basic", tier, size, stream, loader, tier == "OVER"))
                ml_size = want - 1 if tier != "S" else 64  # from BASPRG_ST+1
                _, loader, stream = write_ml(os.path.join(PROGS, tag + "_ml"), ml_size, st + 1)
                rows.append((model, card, "ml", tier, ml_size, stream, loader, tier == "OVER"))
            print(f"{model} {card}: BASPRG_ST=&{st:04X} RAM_END=&{ram_end:04X} free={free}", file=sys.stderr)
    with open(os.path.join(OUT, "manifest.tsv"), "w") as f:
        for r in rows:
            f.write("\t".join(str(v) for v in r) + "\n")
    print(f"{len(rows)} runs -> {OUT}/manifest.tsv", file=sys.stderr)


if __name__ == "__main__":
    main()
