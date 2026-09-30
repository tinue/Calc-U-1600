# Claude Code cloud sessions

How this repository is set up for Claude Code cloud sessions
(claude.ai/code, the Claude app's Code tab, `claude --cloud`). A cloud
session runs on a fresh Ubuntu 24.04 x86_64 VM that clones this repo from
GitHub. It needs the same things as the `core-tests` CI job in
`.github/workflows/build.yml`.

## Two parts

| What | Where it lives | When it runs |
| :--- | :--- | :--- |
| Qt6 + zlib apt packages | The cloud environment's **Setup script** (claude.ai/code → environment settings) | Once, then cached for ~7 days |
| ROMs + Linux libsharpdx | `tools/cloud_session_start.sh`, run by the SessionStart hook in `.claude/settings.json` | Every cloud session start/resume (no-op locally) |

### Environment setup script

Paste this into the environment's **Setup script** field. It runs as root.

```bash
#!/bin/bash
apt-get update
apt-get install -y \
  qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev libgl1-mesa-dev \
  zlib1g-dev
```

The VM already includes GCC, Clang, cmake, ninja and git. Network access
**Trusted** (the default) covers the Ubuntu mirrors, raw.githubusercontent.com
(ROMs) and GitHub release downloads (sharpdx), so no custom allowed domains
are needed.

## Building and testing in a session

Same as `core-tests`:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

No display, so the GUI app builds but can't be driven interactively. Use
the headless CLIs (`pc1500_cli`, `pc1600_cli`) for anything that needs to
run the emulator.

## Limitations

- `Core/Basic/vendor/sharpdx/sharpdx.h` may show as modified after the
  sharpdx fetch. Don't commit that change.
- Your user-level `~/.claude/CLAUDE.md` and auto-memory stay on your
  machine and don't reach cloud sessions. Only what's committed here does.
