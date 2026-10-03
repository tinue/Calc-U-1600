#!/usr/bin/env python3
"""Writes the PC-1600 loader-matrix programs and manifest.

Four sets (README.md):
  1  PC-1600 software via COM1:   LOAD / BLOAD "COM1:", MODE 0 and MODE 1
  2  PC-1500 machine code via COM1: BLOAD "COM1:" (PC-1600 header), MODE 1
  3  PC-1600 BASIC via CE-158:     CLOAD (sde --device pc1500), MODE 1
  4  PC-1500 software via CE-158:  CLOAD / CLOAD M, MODE 1
plus CE-158 refusal cells in MODE 0.

For every configuration it asks the harness (--probe) for BASPRG_ST, its
bank and MEM, then writes the programs into headless/loader-matrix/progs1600/
with `sde convert` and a manifest (headless/loader-matrix/manifest1600.tsv).
Stdlib only; run from the repo root.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import gen_pc1500 as g  # noqa: E402

OUT = g.OUT
PROGS = os.path.join(OUT, "progs1600")
HARNESS = os.path.join(OUT, "pc1600_loader_matrix")

MODE0 = [("none", "none"), ("CE-151", "none"), ("CE-155", "none"), ("CE-161", "none"), ("none", "CE-161"),
         ("CE-1600M", "none"), ("CE-1600M", "CE-1600M")]
MODE1 = [("none", "none"), ("CE-151", "none"), ("CE-155", "none"), ("CE-161", "none"), ("none", "CE-161")]
VARIABLE_PTR = 0x6F00  # LH5803 side; the probe confirms it


def probe(mode, s1, s2):
    out = subprocess.run([HARNESS, "--probe", "--mode", str(mode), "--slot1", s1, "--slot2", s2],
                         check=True, capture_output=True, text=True).stdout
    m = re.search(r"BMODE=(\w+) BASPRG_ST=(\w+) BANK=(\w+) VARIABLE_PTR=(\w+) MEM=(-?\d+) "
                  r"ADTBL=(\w+),(\w+),(\w+),(\w+),(\w+)", out)
    st, bank, vp, mem = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5))
    adtbl = [int(m.group(i), 16) for i in range(6, 11)]
    assert vp == VARIABLE_PTR, out
    return st, bank, mem, adtbl


def pc1600_line(n, i):
    """PC-1500 statements mixed with PC-1600-only keywords."""
    k = i % 6
    if k == 0:
        return f'{n} PRINT "T";TIME$;HEX$ ({i % 256})'
    if k == 1:
        return f"{n} IF A>{i} THEN B={i} ELSE B={i % 7}"
    if k == 2:
        return f"{n} C={i} MOD 7 XOR {i % 5}:PSET ({i % 150},{i % 30})"
    if k == 3:
        return f'{n} D=INSTR (A$,"{chr(65 + i % 26)}")+{i}'
    return g.basic_line(n, i)


def pc1600_listing(lines):
    return "".join(pc1600_line(10 + 10 * i, i) + "\n" for i in range(lines))


def convert(src, dst, *extra):
    if os.path.exists(dst):
        os.remove(dst)
    subprocess.run(["sde", "convert", *extra, src, dst], check=True, capture_output=True)
    return os.path.getsize(dst)


def write_basic(base, size, listing, device, header):
    """Largest listing whose tokenized payload is <= size; returns (payload, .bas, stream)."""
    bas, stream = base + ".bas", base + ".bbin"

    def payload(n):
        with open(bas, "w") as f:
            f.write(listing(n))
        return convert(bas, stream, "--device", device) - header

    lo, hi = 1, 1
    while payload(hi) <= size:
        lo, hi = hi, hi * 2
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if payload(mid) <= size:
            lo = mid
        else:
            hi = mid
    return payload(lo), bas, stream


def lfsr(size, seed):
    data, x = bytearray(), seed
    for _ in range(size):
        x = (x >> 1) ^ (0xB400 if x & 1 else 0)
        data.append(x & 0xFF)
    return data


def write_ml(base, size, start, device, seed=0xACE1):
    """Raw bytes + sde header. `start` is sde's --start-address (PC-1600: bank << 16 | Z-80 address)."""
    raw = base + ".bin"
    out = base + (".pc1600.bin" if device == "pc1600" else ".ce158.bin")
    with open(raw, "wb") as f:
        f.write(lfsr(size, seed))
    convert(raw, out, "--device", device, "--start-address", f"{start:X}")
    return out


def tiers(free, over=True):
    t = [("S", 64), ("M", free // 3), ("L", free * 2 // 3), ("XL", free - 100)]
    return t + [("OVER", free + 256)] if over else t


def first_bank(st_bank, adtbl):
    """Global bank of the first program segment, or None for internal RAM."""
    v = adtbl[st_bank - 1] if 1 <= st_bank <= 5 else 0
    return (v >> 4) & 3 if v else None


def main():
    os.makedirs(PROGS, exist_ok=True)
    rows = []

    def add(set_, mode, s1, s2, transport, kind, target, tier, size, stream, loader, refuse=False):
        rows.append((set_, mode, s1, s2, transport, kind, target, tier, size, stream, loader, refuse))

    for mode, configs in ((0, MODE0), (1, MODE1)):
        for s1, s2 in configs:
            st, st_bank, mem, adtbl = probe(mode, s1, s2)
            tag = f"m{mode}_{s1}_{s2}".replace("-", "")
            print(f"MODE {mode} {s1}/{s2}: BASPRG_ST=&{st:04X} bank-entry={st_bank} ADTBL={adtbl} MEM={mem}",
                  file=sys.stderr)

            # Set 1: PC-1600 BASIC via COM1:. MODE 0 loads the listing (our
            # tokenizer); MODE 1 the .bbin -- a listing would be tokenized with
            # the PC-1500 table there (Decisions.md, "A listing is tokenized by MODE").
            for tier, want in tiers(mem):
                p, bas, bbin = write_basic(os.path.join(PROGS, f"{tag}_s1_basic_{tier}"), want,
                                           pc1600_listing, "pc1600", 16)
                add(1, mode, s1, s2, "com1", "basic", "-", tier, p, bbin, bas if mode == 0 else bbin,
                    tier == "OVER")

            # Set 1: PC-1600 machine code via COM1:, into internal RAM from &C0C6.
            internal = VARIABLE_PTR + 0x8000 - 0xC0C6 - 0x100  # Z-80 &C0C6..&EE00
            for tier, want in tiers(internal, over=False):
                f = write_ml(os.path.join(PROGS, f"{tag}_s1_mlint_{tier}"), want, 0xC0C6, "pc1600")
                add(1, mode, s1, s2, "com1", "ml", "internal", tier, want, f, f)

            # ... and into the first module bank, from BASPRG_ST+1 to &BFFF
            # (OVER runs on into &C000).
            bank = first_bank(st_bank, adtbl)
            if mode == 0 and bank is not None:
                z80 = st + 0x8000 + 1
                for tier, want in tiers(0xC000 - z80):
                    f = write_ml(os.path.join(PROGS, f"{tag}_s1_mlmod_{tier}"), want, bank << 16 | z80, "pc1600")
                    add(1, mode, s1, s2, "com1", "ml", f"bank{bank}", tier, want, f, f)

            if mode == 0:
                continue

            # MODE 1: PC-1500 machine code, from LH BASPRG_ST+1 up to VARIABLE_PTR.
            lh = st + 1
            span = VARIABLE_PTR - lh - 0x100
            for tier, want in tiers(span, over=False):
                base = os.path.join(PROGS, f"{tag}_ml1500_{tier}")
                ce = write_ml(base, want, lh, "pc1500")
                # Set 2: the same bytes behind a PC-1600 header, Z-80 address = LH + &8000.
                z80 = lh + 0x8000
                b = first_bank(st_bank, adtbl) if z80 < 0xC000 else 0
                pc = write_ml(base, want, (b or 0) << 16 | z80, "pc1600")
                add(2, mode, s1, s2, "com1", "ml", "lh", tier, want, pc, pc)
                add(2, mode, s1, s2, "com1", "ml", "lh-ce158file", tier, want, pc, ce)
                # Set 4: CLOAD M over the CE-158.
                add(4, mode, s1, s2, "ce158", "ml", "lh", tier, want, ce, ce)

            for tier, want in tiers(mem):
                # Set 3: PC-1600 BASIC via the CE-158 (PC-1500 keyword table).
                p, bas, bbin = write_basic(os.path.join(PROGS, f"{tag}_s3_basic_{tier}"), want,
                                           pc1600_listing, "pc1500", 27)
                add(3, mode, s1, s2, "ce158", "basic", "-", tier, p, bbin, bas, tier == "OVER")
                # Set 4: PC-1500 BASIC via the CE-158.
                p, bas, bbin = write_basic(os.path.join(PROGS, f"{tag}_s4_basic_{tier}"), want,
                                           g.listing, "pc1500", 27)
                add(4, mode, s1, s2, "ce158", "basic", "-", tier, p, bbin, bas, tier == "OVER")

    # The CE-158 in MODE 0: the ROM refuses CLOAD with ERROR 110, the loader a PC-1500 file.
    small = os.path.join(PROGS, "m1_none_none_s4_basic_S")
    add(4, 0, "none", "none", "ce158", "basic", "-", "S", 0, small + ".bbin", small + ".bbin", True)
    small = os.path.join(PROGS, "m1_none_none_ml1500_S.ce158.bin")
    add(4, 0, "none", "none", "ce158", "ml", "lh", "S", 64, small, small, True)

    with open(os.path.join(OUT, "manifest1600.tsv"), "w") as f:
        for r in rows:
            f.write("\t".join(str(v) for v in r) + "\n")
    print(f"{len(rows)} runs -> {OUT}/manifest1600.tsv", file=sys.stderr)


if __name__ == "__main__":
    main()
