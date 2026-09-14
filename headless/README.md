# headless/

Scratch space for the headless CLI harnesses and throw-away probes built
from `tools/` (`build_cli.sh`, `build_pc1600_cli.sh`,
`build_pc1600_power_probe.sh`, `build_pc1600_uart_probe.sh`,
`build_pc1600_plotter_probe.sh`) and for any debug output they or an ad hoc
CLI/preset run produce — ROM/RAM image dumps, format-utility test images,
`TRACE.bin`-style trace captures, disassembly scratch, and similar.

Everything in here except this file is gitignored: it's all regenerable
(rebuild the tool) or disposable (rerun the probe). Nothing durable should
live here — a reusable example preset belongs in `examples/`, a written-up
investigation belongs in `docs/`, and a permanent finding belongs in this
project's memory.

When building or running a headless tool, write its binary and any scratch
output here rather than the repo root — the build scripts already do this
via `-o headless/<name>`.
