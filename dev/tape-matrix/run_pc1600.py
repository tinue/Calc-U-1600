#!/usr/bin/env python3
"""PC-1600 (MODE 0, CE-1600P) cassette matrix against Pocket Tools 2.1.1.

Each cell runs headless/pc1600_cli once and checks one direction:

  cload      bas2img -> bin2wav -> CLOAD in the emulator; the program area
             must equal bas2img's image byte for byte.
  csave      the emulator's own loader puts the .bas in memory, CSAVE
             records it; wav2bin must decode exactly the bytes in memory.
             (Whether bas2img tokenizes the listing the same way is
             reported, not judged: that's the tokenizer, not the tape.)
  cloadm     bin2wav -t bin -> CLOAD M; memory must equal the .bin.
  csavem     a BASIC POKE loop fills memory, CSAVE M records it; wav2bin
             must decode those bytes.

Every load cell runs once per WAV flavour: bin2wav's 48 kHz default and its
16 kHz one (-l 3). bin2wav gets -s 3 (a 3 s leader): its 0.5 s default is
shorter than the ROM's motor start-up delay plus the 5000 leader cycles
CMSYNC wants (see Core/tests/pc1600_tape_tests.cpp).

Run from the repo root: python3 dev/tape-matrix/run_pc1600.py
Output: headless/tape-matrix/pc1600/ (work files, logs, results.md).
"""
import pathlib
import random
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "headless" / "tape-matrix" / "pc1600"
CLI = ROOT / "headless" / "pc1600_cli"
TOOLS = pathlib.Path.home() / "Applications" / "PocketPc"

PROGRAMS = [
    "Core/tests/fixtures/tape/pc1600_tape.bas",
    "examples/basic/old-vs-new-rom.bas",
    "examples/basic/dampflok.bas",
    "examples/basic/hanoi.bas",
    "examples/plotter/ascii.bas",
    "examples/plotter/biorhythmus_1600.bas",
    "examples/plotter/globus.bas",
    "examples/plotter/lissajou-1600.bas",
]
ML_SIZES = [1, 255, 256, 257, 2000]
ML_ADDR = 0xD000
FLAVOURS = {"48k": [], "16k": ["-l", "3"]}


def run(cmd, log):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, cwd=ROOT)
    log.write("$ " + " ".join(str(c) for c in cmd) + "\n" + r.stdout + r.stderr + "\n")
    return r


def preset(path, keys, program=None):
    lines = ["model: PC-1600", "plotter: CE-1600P", "keys:", "  - key: mode", "  - type: NEW0"]
    if program:
        lines += ["program:", f"  file: {program}", "keys:"]
    lines += [f"  - {k}" for k in keys]
    path.write_text("\n".join(lines) + "\n")


def basic_area(stdout):
    """The program bytes from --dump-basic: BASPRG_ST up to BASPRG_END."""
    m = re.search(r"len = (\d+)\)", stdout)
    data = bytes(int(t, 16) for line in stdout.splitlines()
                 if re.match(r"^[0-9A-F]{4}: ", line)
                 for t in line[6:6 + 48].split())
    return data[:int(m.group(1))] if m else None


def mem_area(stdout):
    lines = stdout.split("--- memory", 1)[-1].splitlines()[1:]
    return bytes(int(t, 16) for line in lines if re.match(r"^[0-9A-F]{4}:", line)
                 for t in line[5:].split())


def screen_error(stdout):
    m = re.search(r"ERROR \d+", stdout)
    return m.group(0) if m else ""


def main():
    if not CLI.exists():
        subprocess.run(["tools/build_pc1600_cli.sh"], cwd=ROOT, check=True)
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    log = (OUT / "matrix.log").open("w")

    def cell(kind, name, flavour, ok, note):
        rows.append((kind, name, flavour, "pass" if ok else "**FAIL**", note))
        print(f"{'ok  ' if ok else 'FAIL'} {kind:7} {flavour:4} {name}  {note}", flush=True)

    for prog in PROGRAMS:
        src = ROOT / prog
        stem = src.stem
        img = OUT / f"{stem}.img"
        r = run([TOOLS / "bas2img", "-p", "1600", src, img], log)
        if r.returncode != 0 or not img.exists():
            cell("cload", stem, "-", False, "bas2img failed")
            continue
        ref = img.read_bytes()

        for flavour, opts in FLAVOURS.items():
            wav = OUT / f"{stem}-{flavour}.wav"
            run([TOOLS / "bin2wav", "-p", "1600", "-s", "3", *opts, f"-n{stem[:8].upper()}", img, wav], log)
            p = OUT / f"cload-{stem}.pc1600"
            preset(p, ["type: CLOAD", "wait:"])
            r = run([CLI, "--preset", p, "--tape-in", wav, "--dump-basic", "--lcd-text", "-"], log)
            got = basic_area(r.stdout)
            err = screen_error(r.stdout)
            cell("cload", stem, flavour, got == ref and not err,
                 f"{len(ref)} bytes" + (f", {err}" if err else "") +
                 ("" if got == ref else f", got {len(got or b'')} bytes"))

        wav = OUT / f"{stem}-csave.wav"
        p = OUT / f"csave-{stem}.pc1600"
        preset(p, [f'type: CSAVE "{stem[:8].upper()}"', "wait:"], program=src)
        r = run([CLI, "--preset", p, "--tape-out", wav, "--dump-basic", "--lcd-text", "-"], log)
        mem = basic_area(r.stdout)
        dec = OUT / f"{stem}-csave.img"
        dec.unlink(missing_ok=True)
        run([TOOLS / "wav2bin", "-p", "1600", "-t", "img", wav, dec], log)
        got = dec.read_bytes() if dec.exists() else b""
        err = screen_error(r.stdout)
        cell("csave", stem, "48k", bool(mem) and got == mem and not err,
             f"{len(mem or b'')} bytes" + (f", {err}" if err else "") +
             ("" if got == mem else f", wav2bin gave {len(got)} bytes") +
             ("" if mem == ref else ", emulator tokenizes differently from bas2img"))

    rng = random.Random(1600)
    for size in ML_SIZES:
        data = bytes(rng.randrange(256) for _ in range(size))
        name = f"ml{size}"
        binf = OUT / f"{name}.bin"
        binf.write_bytes(data)
        for flavour, opts in FLAVOURS.items():
            wav = OUT / f"{name}-{flavour}.wav"
            run([TOOLS / "bin2wav", "-p", "1600", "-t", "bin", "-a", hex(ML_ADDR), "-s", "3", *opts,
                 f"-n{name.upper()}", binf, wav], log)
            p = OUT / f"cloadm-{name}.pc1600"
            preset(p, ["type: CLOAD M", "wait:"])
            r = run([CLI, "--preset", p, "--tape-in", wav, "--dump-mem", f"{ML_ADDR:#x},{size}",
                     "--lcd-text", "-"], log)
            got = mem_area(r.stdout)
            err = screen_error(r.stdout)
            cell("cloadm", name, flavour, got == data and not err, f"{size} bytes" + (f", {err}" if err else ""))

        # CSAVE M of a pattern a BASIC loop pokes in. POKE itself refuses
        # part of the 2000-byte range (ERROR 19 IN 10), so the largest size
        # is only covered by cloadm.
        if size > 1024:
            continue
        pattern = bytes((i * 37 + 11) & 255 for i in range(size))
        end = ML_ADDR + size - 1
        wav = OUT / f"{name}-csavem.wav"
        p = OUT / f"csavem-{name}.pc1600"
        # (A program line: PRO-mode direct input takes only one statement.)
        preset(p, [f"type: 10 FOR I=0 TO {size - 1}:POKE {ML_ADDR}+I,(I*37+11) AND 255:NEXT I",
                   "key: mode", "type: RUN", "wait:",
                   f'type: CSAVE M "{name.upper()}";#0,&{ML_ADDR:X},&{end:X}', "wait:"])
        r = run([CLI, "--preset", p, "--tape-out", wav, "--dump-mem", f"{ML_ADDR:#x},{size}",
                 "--lcd-text", "-"], log)
        mem = mem_area(r.stdout)
        dec = OUT / f"{name}-csavem.bin"
        dec.unlink(missing_ok=True)
        run([TOOLS / "wav2bin", "-p", "1600", "-t", "img", wav, dec], log)
        got = dec.read_bytes() if dec.exists() else b""
        err = screen_error(r.stdout)
        cell("csavem", name, "48k", mem == pattern and got == pattern and not err,
             f"{size} bytes" + (f", {err}" if err else "") +
             ("" if got == pattern else f", wav2bin gave {len(got)} bytes"))

    passed = sum(1 for r in rows if r[3] == "pass")
    with (OUT / "results.md").open("w") as f:
        f.write(f"# PC-1600 tape matrix: {passed}/{len(rows)} pass\n\n")
        f.write("| Cell | Program | WAV | Result | Notes |\n|---|---|---|---|---|\n")
        for row in rows:
            f.write("| " + " | ".join(row) + " |\n")
    print(f"{passed}/{len(rows)} pass -> {OUT / 'results.md'}")
    return 0 if passed == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
