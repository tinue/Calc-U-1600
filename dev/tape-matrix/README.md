# Tape matrix

A one-time check of cassette `CLOAD` / `CSAVE` against Pocket Tools 2.1.1
(`bas2img`, `bin2wav`, `wav2bin`, in `~/Applications/PocketPc/`). Each cell
runs the model's headless CLI once with `--tape-in` or `--tape-out`. This
directory is not part of `tools/run_tests.sh` or CoreTests; those have
their own tape tests (`Core/tests/pc1500_tape_tests.cpp`,
`pc1600_tape_tests.cpp`).

```sh
python3 dev/tape-matrix/run.py pc1600    # PC-1600, MODE 0, CE-1600P (about 1 minute)
python3 dev/tape-matrix/run.py pc1500    # PC-1500A, CE-150 (about 15 s)
```

Each builds its CLI if needed. Output goes to
`headless/tape-matrix/<model>/`: the WAVs, presets and decoded images,
`matrix.log` and `results.md`.

| Cell | Direction | Passes when |
|---|---|---|
| `cload` | `bas2img` → `bin2wav` → `CLOAD` | the program area equals `bas2img`'s image |
| `csave` | fast loader → `CSAVE` → `wav2bin` | `wav2bin` decodes the bytes in memory |
| `cloadm` | `bin2wav -t bin` → `CLOAD M` | memory equals the `.bin` |
| `csavem` | BASIC `POKE` loop → `CSAVE M` → `wav2bin` | `wav2bin` decodes the poked pattern |

No cell may leave `ERROR` on the display. Load cells run with both WAV
flavours `bin2wav` makes: its default (48 kHz for the PC-1600, 44.1 kHz for
the PC-1500) and 16 kHz (`-l 3`). Programs: the test fixtures and the
model's examples. Machine code: random bytes around the checksum block
size (256 bytes on the PC-1600, at &D000; 80 on the PC-1500A, in its
machine-language area &7C01).

## PC-1600, MODE 0, CE-1600P

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
  the emulator keeps the space after `REM` (`10 REM LISSAJOU` stores
  `F1 AB 20 4C ...`), `bas2img` drops it. The ROM's own line editor keeps
  it too (the line typed in, on both machines), so the emulator is right.
  The cell compares `wav2bin`'s output with memory, which match.
- `csavem` skips 2000 bytes: the BASIC `POKE` loop that fills memory stops
  with `ERROR 19 IN 10` there.

## PC-1500A, CE-150

### Result (2026-10-04): 26/26 pass

- The emulator's `CSAVE` bit stream is the same as `bin2wav`'s, frame for
  frame: start bit, 4 data bits, 6 stop bits per nibble. Tones 2528 /
  1288 Hz (the LH5811 modulator: 2539 / 1270 Hz).
- **`bin2wav` needs `-s 3` here too.** The ROM's own `CSAVE` writes about
  8 s of leader.
- `csave`: `lissajou-1500` and `update` come out a byte longer from the
  emulator's tokenizer than from `bas2img` (the space after `REM` again).
- `csavem` skips 1000 bytes: the `POKE` loop stops with `ERROR 19` there.

