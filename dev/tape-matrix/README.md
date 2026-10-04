# Tape matrix

A one-time check of cassette `CLOAD` / `CSAVE` against Pocket Tools 2.1.1
(`bas2img`, `bin2wav`, `wav2bin`, in `~/Applications/PocketPc/`). Each cell
runs `headless/pc1600_cli` once with `--tape-in` or `--tape-out`. This
directory is not part of `tools/run_tests.sh` or CoreTests; those have
their own tape tests (`Core/tests/pc1600_tape_tests.cpp`).

## PC-1600, MODE 0, CE-1600P

```sh
python3 dev/tape-matrix/run_pc1600.py    # about 1 minute; builds the CLI if needed
```

Output goes to `headless/tape-matrix/pc1600/`: the WAVs, presets and
decoded images, `matrix.log` and `results.md`.

| Cell | Direction | Passes when |
|---|---|---|
| `cload` | `bas2img` → `bin2wav` → `CLOAD` | the program area equals `bas2img`'s image |
| `csave` | fast loader → `CSAVE` → `wav2bin` | `wav2bin` decodes the bytes in memory |
| `cloadm` | `bin2wav -t bin` → `CLOAD M` | memory at &D000 equals the `.bin` |
| `csavem` | BASIC `POKE` loop → `CSAVE M` → `wav2bin` | `wav2bin` decodes the poked pattern |

No cell may leave `ERROR` on the display. Load cells run with both WAV
flavours `bin2wav` makes: the 48 kHz default and 16 kHz (`-l 3`). Programs:
the test fixture, the PC-1600 examples in `examples/basic/` and
`examples/plotter/`. Machine code: 1, 255, 256, 257 and 2000 random bytes
(blocks are 256 bytes).

### Result (2026-10-04): 38/38 pass

- The ROM's tones measure 3029 Hz ("0") and 1222 Hz ("1") in the
  emulator, against `bin2wav`'s 3000 / 1200 Hz. Both decoders accept
  either.
- **`bin2wav` needs `-s 3`.** Its default leader (0.5 s, 6406 cycles) is
  too short when the recorder runs on the remote. `CLOAD` pulses the relay,
  then waits ~0.6 s for the motor (`CASMOTOR` 642EH, `DELAYF0`), and only
  then counts 5000 leader cycles (`CMSYNC`, F1AAH). The ROM's own `CSAVE`
  writes 10000. A real recorder on the remote would lose the same stretch.
- **`csave`: four programs come out a few bytes longer from the emulator's
  tokenizer than from `bas2img`** (`pc1600_tape`, `old-vs-new-rom`,
  `ascii`, `biorhythmus_1600`). That's the listing tokenizer, not the tape:
  the cell compares `wav2bin`'s output with memory, which match.
- `csavem` skips 2000 bytes: the BASIC `POKE` loop that fills memory stops
  with `ERROR 19 IN 10` there.
