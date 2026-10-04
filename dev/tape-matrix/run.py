#!/usr/bin/env python3
"""Cassette matrix against Pocket Tools 2.1.1, per model.

  python3 dev/tape-matrix/run.py pc1600   # PC-1600, MODE 0, CE-1600P
  python3 dev/tape-matrix/run.py pc1500   # PC-1500A, CE-150

Each cell runs the model's headless CLI once and checks one direction:

  cload      bas2img -> bin2wav -> CLOAD in the emulator; the program area
             must equal bas2img's image byte for byte.
  csave      the emulator's own loader puts the .bas in memory, CSAVE
             records it; wav2bin must decode exactly the bytes in memory.
             (Whether bas2img tokenizes the listing the same way is
             reported, not judged: that's the tokenizer, not the tape.)
  cloadm     bin2wav -t bin -> CLOAD M; memory must equal the .bin.
  csavem     a BASIC POKE loop fills memory, CSAVE M records it; wav2bin
             must decode those bytes.

Every load cell runs once per WAV flavour: bin2wav's default (48 kHz on the
PC-1600, 44.1 kHz on the PC-1500) and its 16 kHz one (-l 3). bin2wav gets
-s 3 (a 3 s leader): its 0.5 s default is shorter than the ROM's motor
start-up delay plus the leader the ROM wants (see
Core/tests/pc1600_tape_tests.cpp).

Run from the repo root. Output: headless/tape-matrix/<model>/ (work files,
logs, results.md).
"""
import pathlib
import random
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = pathlib.Path.home() / "Applications" / "PocketPc"

MODELS = {
    "pc1600": {
        "cli": "pc1600_cli",
        "build": "tools/build_pc1600_cli.sh",
        "pc": "1600",
        "ext": "pc1600",
        "header": ["model: PC-1600", "plotter: CE-1600P", "keys:", "  - key: mode", "  - type: NEW0"],
        "programs": [
            "Core/tests/fixtures/tape/pc1600_tape.bas",
            "examples/basic/old-vs-new-rom.bas",
            "examples/basic/dampflok.bas",
            "examples/basic/hanoi.bas",
            "examples/plotter/ascii.bas",
            "examples/plotter/biorhythmus_1600.bas",
            "examples/plotter/globus.bas",
            "examples/plotter/lissajou-1600.bas",
        ],
        # Blocks are 256 bytes. POKE refuses part of a 2000-byte range at
        # &D000 (ERROR 19), so the largest size is only covered by cloadm.
        "ml_addr": 0xD000,
        "ml_sizes": [1, 255, 256, 257, 2000],
        "csavem_max": 1024,
        "csavem": 'CSAVE M "{name}";#0,&{start:X},&{end:X}',
    },
    "pc1500": {
        "cli": "pc1500_cli",
        "build": "tools/build_cli.sh",
        "pc": "1500",
        "ext": "pc1500a",
        "header": ["model: PC-1500A", "plotter: CE-150", "keys:", "  - key: cl", "  - type: NEW0"],
        "programs": [
            "Core/tests/fixtures/tape/pc1500_tape.bas",
            "examples/plotter/lissajou-1500.bas",
            "examples/setup/update.bas",
            "examples/setup/util_15.bas",
        ],
        # The PC-1500A's machine-language area &7C01-&7FFF; blocks are 80
        # bytes. The POKE loop stops with ERROR 19 before 1000 bytes, so
        # that size is only covered by cloadm.
        "ml_addr": 0x7C01,
        "ml_sizes": [1, 79, 80, 81, 1000],
        "csavem_max": 100,
        "csavem": 'CSAVE M "{name}";&{start:X},&{end:X}',
    },
}


def run(cmd, log):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, cwd=ROOT)
    log.write("$ " + " ".join(str(c) for c in cmd) + "\n" + r.stdout + r.stderr + "\n")
    return r


def preset(cfg, path, keys, program=None):
    lines = list(cfg["header"])
    if program:
        lines += ["program:", f"  file: {program}", "keys:"]
    lines += [f"  - {k}" for k in keys]
    path.write_text("\n".join(lines) + "\n")


def basic_area(stdout):
    """The program bytes from --dump-basic: BASPRG_ST up to BASPRG_END."""
    m = re.search(r"len = (-?\d+)\)", stdout)
    data = bytes(int(t, 16) for line in stdout.splitlines()
                 if re.match(r"^[0-9A-F]{4}: ", line)
                 for t in line[6:6 + 48].split())
    return data[:int(m.group(1))] if m and int(m.group(1)) >= 0 else None


def mem_area(stdout):
    lines = stdout.split("--- memory", 1)[-1].splitlines()[1:]
    return bytes(int(t, 16) for line in lines if re.match(r"^[0-9A-F]{4}:", line)
                 for t in line[5:].split())


def tape_name(stem):
    """A file name the keyboard can type (the PC-1500 has no '_')."""
    return re.sub(r"[^A-Za-z0-9]", "", stem)[:8].upper()


def screen_error(stdout):
    m = re.search(r"ERROR \d+", stdout)
    return m.group(0) if m else ""


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in MODELS:
        print(__doc__)
        return 2
    model = sys.argv[1]
    cfg = MODELS[model]
    cli = ROOT / "headless" / cfg["cli"]
    if not cli.exists():
        subprocess.run([cfg["build"]], cwd=ROOT, check=True)
    out = ROOT / "headless" / "tape-matrix" / model
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    log = (out / "matrix.log").open("w")
    flavours = {"default": [], "16k": ["-l", "3"]}
    ext = cfg["ext"]

    def cell(kind, name, flavour, ok, note):
        rows.append((kind, name, flavour, "pass" if ok else "**FAIL**", note))
        print(f"{'ok  ' if ok else 'FAIL'} {kind:7} {flavour:7} {name}  {note}", flush=True)

    for prog in cfg["programs"]:
        src = ROOT / prog
        stem = src.stem
        img = out / f"{stem}.img"
        r = run([TOOLS / "bas2img", "-p", cfg["pc"], src, img], log)
        if r.returncode != 0 or not img.exists():
            cell("cload", stem, "-", False, "bas2img failed")
            continue
        ref = img.read_bytes()

        for flavour, opts in flavours.items():
            wav = out / f"{stem}-{flavour}.wav"
            run([TOOLS / "bin2wav", "-p", cfg["pc"], "-s", "3", *opts, f"-n{tape_name(stem)}", img, wav], log)
            p = out / f"cload-{stem}.{ext}"
            preset(cfg, p, ["type: CLOAD", "wait:"])
            r = run([cli, "--preset", p, "--tape-in", wav, "--dump-basic", "--lcd-text", "-"], log)
            got = basic_area(r.stdout)
            err = screen_error(r.stdout)
            cell("cload", stem, flavour, got == ref and not err,
                 f"{len(ref)} bytes" + (f", {err}" if err else "") +
                 ("" if got == ref else f", got {len(got or b'')} bytes"))

        wav = out / f"{stem}-csave.wav"
        p = out / f"csave-{stem}.{ext}"
        preset(cfg, p, [f'type: CSAVE "{tape_name(stem)}"', "wait:"], program=src)
        r = run([cli, "--preset", p, "--tape-out", wav, "--dump-basic", "--lcd-text", "-"], log)
        mem = basic_area(r.stdout)
        dec = out / f"{stem}-csave.img"
        dec.unlink(missing_ok=True)
        run([TOOLS / "wav2bin", "-p", cfg["pc"], "-t", "img", wav, dec], log)
        got = dec.read_bytes() if dec.exists() else b""
        err = screen_error(r.stdout)
        cell("csave", stem, "-", bool(mem) and got == mem and not err,
             f"{len(mem or b'')} bytes" + (f", {err}" if err else "") +
             ("" if got == mem else f", wav2bin gave {len(got)} bytes") +
             ("" if mem == ref else ", emulator tokenizes differently from bas2img"))

    rng = random.Random(1600)
    start = cfg["ml_addr"]
    for size in cfg["ml_sizes"]:
        data = bytes(rng.randrange(256) for _ in range(size))
        name = f"ml{size}"
        binf = out / f"{name}.bin"
        binf.write_bytes(data)
        for flavour, opts in flavours.items():
            wav = out / f"{name}-{flavour}.wav"
            run([TOOLS / "bin2wav", "-p", cfg["pc"], "-t", "bin", "-a", hex(start), "-s", "3", *opts,
                 f"-n{name.upper()}", binf, wav], log)
            p = out / f"cloadm-{name}.{ext}"
            preset(cfg, p, ["type: CLOAD M", "wait:"])
            r = run([cli, "--preset", p, "--tape-in", wav, "--dump-mem", f"{start:#x},{size}",
                     "--lcd-text", "-"], log)
            got = mem_area(r.stdout)
            err = screen_error(r.stdout)
            cell("cloadm", name, flavour, got == data and not err, f"{size} bytes" + (f", {err}" if err else ""))

        # CSAVE M of a pattern a BASIC loop pokes in. (A program line: the
        # PC-1600's PRO-mode direct input takes only one statement.)
        if size > cfg["csavem_max"]:
            continue
        pattern = bytes((i * 37 + 11) & 255 for i in range(size))
        wav = out / f"{name}-csavem.wav"
        p = out / f"csavem-{name}.{ext}"
        csave = cfg["csavem"].format(name=name.upper(), start=start, end=start + size - 1)
        preset(cfg, p, [f"type: 10 FOR I=0 TO {size - 1}:POKE {start}+I,(I*37+11) AND 255:NEXT I",
                        "key: mode", "type: RUN", "wait:", f"type: {csave}", "wait:"])
        r = run([cli, "--preset", p, "--tape-out", wav, "--dump-mem", f"{start:#x},{size}",
                 "--lcd-text", "-"], log)
        mem = mem_area(r.stdout)
        dec = out / f"{name}-csavem.bin"
        dec.unlink(missing_ok=True)
        run([TOOLS / "wav2bin", "-p", cfg["pc"], "-t", "img", wav, dec], log)
        got = dec.read_bytes() if dec.exists() else b""
        err = screen_error(r.stdout)
        cell("csavem", name, "-", mem == pattern and got == pattern and not err,
             f"{size} bytes" + (f", {err}" if err else "") +
             ("" if mem == pattern else ", POKE loop didn't fill memory") +
             ("" if got == pattern else f", wav2bin gave {len(got)} bytes"))

    passed = sum(1 for r in rows if r[3] == "pass")
    with (out / "results.md").open("w") as f:
        f.write(f"# {model} tape matrix: {passed}/{len(rows)} pass\n\n")
        f.write("| Cell | Program | WAV | Result | Notes |\n|---|---|---|---|---|\n")
        for row in rows:
            f.write("| " + " | ".join(row) + " |\n")
    print(f"{passed}/{len(rows)} pass -> {out / 'results.md'}")
    return 0 if passed == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
