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
- **`bin2wav` needs `-s 3` (or `-l 0x400`).** Its default PC-1600 leader
  (its minimum for the model: ~2.1 s, 6406 cycles, no gap before it) is
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
- **`bin2wav` needs `-s 3` here too:** its default PC-1500 leader is 0.5 s
  (156 bits). The ROM's own `CSAVE` writes about 8 s, as does `bin2wav -l
  0x400` (2504 bits), which also loads.
- `csave`: `lissajou-1500` and `update` come out a byte longer from the
  emulator's tokenizer than from `bas2img` (the space after `REM` again).
- `csavem` skips 1000 bytes: the `POKE` loop stops with `ERROR 19` there.


## How `bin2wav` builds its WAVs, compared with the emulator's recordings

From `bin2wav_211c1d2.c` (Pocket Tools 2.1.1, in
`pocket-pc-tools/archive/POCKTOOL/Sources/`):

| | `bin2wav` | Emulator `CSAVE` (`TapeDeck`) |
|---|---|---|
| Source of the waveform | Drawn per bit from tables (`bit3_15` PC-1500, `bitE3` PC-1600): characters for full, ¾, ⅜ level and mid | The calculator's own output line, box-filtered to 48 kHz |
| Format | 8-bit unsigned; 44.1 kHz (PC-1500), 48 kHz (PC-1600); 16 kHz with `-l 3` | 16-bit, 48 kHz |
| Level | PC-1500 ±0.70 with ~6% peak overshoot (`AMP_HIGH` DAH / `AMP_LOW` 26H, `^`/`v`); PC-1600 ±0.97 (FCH / 04H) | ±0.45, edges overshoot to ~0.6 (up to 0.9 on the first edge after a gap) through the 300 Hz coupling |
| Edges | Pre-rounded: ramps through ¾ and mid level, near-sine on the PC-1500 | Sharp (one sample), then AC-coupling droop |
| DC / silence | Symmetric around 80H by construction; silence = 80H | Parked line → silence through the coupling; tone settles in ~1 ms |
| End of transmission | `*` shutdown: an exponential settle with a small overshoot, imitating a coupled signal floating back to mid | Same effect from the coupling itself |
| PC-1500 tones | 144 samples per bit at 44.1 kHz: 2450 / 1225 Hz | LH5811: 2539 / 1270 Hz (3.6% faster) |
| PC-1600 tones | 16 / 40 samples: 3000 / 1200 Hz, symmetric halves | ROM loops: 3029 / 1222 Hz, high half a little longer (30 vs 23 loops for "0") |
| Polarity | PC-1500 mirrored (`bitMirroring`: each cycle starts below mid) | Line high = positive. Both read fine: the ROMs and `wav2bin` go by zero crossings and accept either half |
| Gap and leader | Default: PC-1500 0.5 s; PC-1600 its minimum ~2.1 s (6406 cycles), no gap. `-l 0x400` ("like the original"): PC-1500 ~8 s (2504 bits), PC-1600 an 8 s gap, then 10000 cycles | What the ROM writes: PC-1600 an 8 s gap (`CMGAP`, line low), then 10000 cycles; PC-1500 ~8 s |

So `bin2wav` draws an idealised, band-limited tape signal, while the
emulator records the logic line through a model of the interface's output
coupling. Both are centred, settle at the edges of a transmission and keep
the zero crossings every decoder uses, and each reads the other's files.
