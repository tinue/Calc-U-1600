# Packaging TODO

Where the macOS packaging / ROM-sourcing work stands, and what's left.

1. ~~Dump the CE-150 ROM from real hardware, add it to a repo, and wire
   it into `tools/fetch_roms.sh`.~~ Done: `tinue/PC-1500-ROM`
   (`dumps/CE-150.BIN`), verified byte-identical to the copy previously
   bundled here; all 13 ROMs now fetch via `tools/fetch_roms.sh` (see
   `roms/README.md`).
2. Document and test a "no ROM" build: a local build with no ROMs bundled
   into the binary, signed and stamped as usual, that instead fetches
   ROMs from a documented local path at runtime. Not currently planned to
   switch to this, but be prepared for it.
3. ~~Re-architect the GitHub Actions builds to first fetch ROMs
   (`tools/fetch_roms.sh`), then fan out into the per-platform build jobs.~~
   Done: every job (linux-x86_64, linux-arm64, macos, windows) now runs a
   "Fetch ROMs" step before Configure. Before this, CI stayed green while
   silently shipping ROM-less binaries.
4. ~~Test the macOS build end to end, including signing/notarization/DMG
   stapling, against a real Apple Developer ID.~~ Done: CI run 34827074447
   (2026-09-14) passed clean, with the DMG genuinely signed, notarized,
   and stapled — not yet downloaded and smoke-tested on a Mac by hand,
   worth doing once before calling this final.
5. Decide on packaging for the other platforms (Linux, Windows) one by
   one, and execute it.
   - ~~Linux: AppImage.~~ Done: `linux-x86_64`/`linux-aarch64` jobs build
     via linuxdeploy + its Qt plugin. Two real bugs found and fixed along
     the way: (a) `.desktop`'s `Exec=` went stale after the Calc-U-1600
     rename, breaking the build outright; (b) `linuxdeploy-plugin-qt`
     hardcodes `libqxcb.so` as the only platform plugin unless told
     otherwise (`linuxdeploy-plugin-qt#160`, open upstream) — needed
     `EXTRA_PLATFORM_PLUGINS` plus a manual `wayland-graphics-
     integration-client` copy + second `linuxdeploy --deploy-deps-only`
     pass (verified recipe from `MMetze/DMHelper#210`) to get genuine
     Wayland/HiDPI support, not just XWayland fallback. CI run 34836070547
     (2026-09-14) passed clean with the full Wayland plugin set confirmed
     bundled by inspecting the built AppImage directly — not yet smoke-
     tested on a real Wayland desktop by hand (the Raspberry Pi report
     that started this was against the pre-fix build).
     A third bug: `--plugin=qt` never bundles the `platformthemes` plugin
     category (only `platforms`), which is what queries the desktop for
     light/dark mode and feeds Qt an updated `QPalette` — without it the
     AppImage can't detect OS theme switches at all, on both x86_64 and
     the Raspberry Pi arm64 build, unlike a native build using the system
     Qt install's own `platformthemes` plugin. Fixed by installing
     `qt6-gtk-platformtheme`, copying its `platformthemes` plugin dir into
     the AppImage the same way as the Wayland plugin, and wrapping the
     generated `AppRun` to export `QT_QPA_PLATFORMTHEME=gtk3` (only if not
     already set, so an existing desktop-session value like KDE's `kde` is
     left alone). Not yet verified against a real desktop by hand.
   - Windows: still undecided/unexecuted.
