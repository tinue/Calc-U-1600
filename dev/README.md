# dev/

Developer material that is not shipped with the release (the release zips
only `examples/`). User-facing demos and learning material belong in
`examples/`; regenerable scratch output belongs in `headless/`.

- `hardware-checks/` — small machine-code programs (source, assembled
  `.bin`, and a preset that loads them) written to settle emulator questions
  against real hardware: ADR flag preservation, DRL/DRR nibble rotates,
  LCD all-pixels-on sizing. Each `.asm` header explains the background and
  the expected results.
- `presets/` — presets used while debugging or regression-checking the
  emulator itself: preset-loader / memory-card tests, the superRAM card
  setup, the Up/Down-key iterator, and the `- trace:` step demo.
