# Developer documentation

For people who build Calc-U-1600 from source or work on the emulator
itself. Using the emulator (including writing PC-1500 / PC-1600 programs
with its debugger) is covered by the user documentation, starting at the
[main README](../../README.md). Why things are the way they are — design
decisions, plans, handoffs — is in [../background/README.md](../background/README.md).

## Build and test

- [Building.md](Building.md) — prerequisites, `libsharpdx`, getting the
  ROMs, per-platform builds, CLion, running the tests, packaging.
- [roms/README.md](../../roms/README.md) — where each ROM image comes from
  and its checksum; `tools/fetch_roms.sh` downloads them.
- [Cloud-Sessions.md](Cloud-Sessions.md) — how the repository is set up for
  Claude Code cloud sessions; [CLAUDE.md](../../CLAUDE.md) holds the project
  instructions for Claude Code.
- [.github/macos-signing.md](../../.github/macos-signing.md) — how the CI
  signs and notarizes the macOS app.
- [TODO.md](../../TODO.md) — known issues, open work and the code-cleanup
  backlog.
- [CHANGELOG.md](../../CHANGELOG.md) — what changed in each release.

## Repository map

| Folder | What's in it |
|---|---|
| `Core/` | The emulator, free of Qt: CPUs, machines, connectors and cards, loaders, presets, the debug server core; `Core/tests/` holds the test suite |
| `Qt6/` | The desktop app (`Qt6/app/`) and its bundled resources: cards, faceplates, platform files (`Qt6/resources/`) |
| `firmware/` | Our own ROMs, e.g. the host-drive driver ROM (`firmware/pc1600-hostdrive/`) |
| `vscode/` | The VS Code debugger extension ([README](../../vscode/calcu1600-debug/README.md)); `tools/install_vscode_extension.sh` packages and installs it |
| `tools/` | Build scripts, the headless CLIs (`pc1500_cli`, `pc1600_cli`), probes, `fetch_roms.sh`, `fetch_sharpdx.sh` / `refresh_sharpdx.sh`, `run_tests.sh`, `dap_smoke.py`, `read_trace.py`, `make_screenshots.sh` |
| `examples/` | User-facing samples, shipped as `Calc-U-1600-examples.zip` ([README](../../examples/README.md)); `examples/setup/` prepares memory cards ([README](../../examples/setup/README.md)) |
| `dev/` | Developer material that is not shipped: hardware checks and their presets ([README](../../dev/README.md)) |
| `headless/` | Gitignored scratch output of the CLIs and probes ([README](../../headless/README.md)) |
| `bin/` | The release script |

## How the emulator works

- [PC1600-Core-Limitations.md](PC1600-Core-Limitations.md) — the deliberate
  shortcuts, unmodelled parts and open assumptions in the PC-1600 core.
- [PC1600-Serial-Port.md](PC1600-Serial-Port.md) — the RS-232C port on a
  host pseudo-terminal: transport, line model, probe tooling.
- [PC1600-Host-Drive-Internals.md](PC1600-Host-Drive-Internals.md) — the
  host-drive ROM, its I/O protocol and FILE functions.
- [PC1500-BASIC-Variable-Layout.md](PC1500-BASIC-Variable-Layout.md) — how
  the PC-1500 ROM BASIC stores variables in RAM.
- [LCD-Text.md](LCD-Text.md) — reading the display as text: the ROM fonts,
  the cursor, the encoding, and where to use it (`--lcd-text`, `expect:`,
  `calcu1600/screen`).

## Sources

Comments and docs cite the original sources: the Technical Reference
Manual (TRM), the Service Manuals, data sheets and the ROMs (by bank and
address). Hardware facts collected from these sources are written up in
the research repository
[Sharp1500-1600-Ref](https://github.com/tinue/Sharp1500-1600-Ref). Comments
cite it as `Ref/<path>`, a path inside that repository, e.g.
`Ref/PC-1600/PC-1600-Keyboard.md §5`.

## File formats

- [Floppy-Image-Format.md](Floppy-Image-Format.md) — the `*.floppy.yaml`
  CE-1600F diskette format, for other tools that read or write it.
- Memory cards (`*.card.yaml`) are user-level:
  [Memory-Card-Definition-Spec.md](../Memory-Card-Definition-Spec.md) and
  [Memory-Card-Definition-Format.md](../Memory-Card-Definition-Format.md).
- Presets are described in the User Guide,
  [8. Presets](../User-Guide.md#8-presets).

## User Guide screenshots

- [screenshots/README.md](screenshots/README.md) — the scripted screenshot
  scenarios behind `docs/images/guide/`, and how to regenerate them with
  `tools/make_screenshots.sh`.
