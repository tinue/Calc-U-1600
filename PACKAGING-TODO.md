# Packaging TODO

Where the macOS packaging / ROM-sourcing work stands, and what's left.
Work is paused here to go dump the CE-150 ROM.

1. Dump the CE-150 ROM from real hardware, add it to a repo (mirroring
   `tinue/PC-1600-ROM`'s own setup), and add it to `tools/fetch_roms.sh`
   the same way as the other 12 ROMs (see `roms/README.md`).
2. Document and test a "no ROM" build: a local build with no ROMs bundled
   into the binary, signed and stamped as usual, that instead fetches
   ROMs from a documented local path at runtime. Not currently planned to
   switch to this, but be prepared for it.
3. Re-architect the GitHub Actions builds to first fetch ROMs
   (`tools/fetch_roms.sh`), then fan out into the per-platform build jobs.
4. Test the macOS build end to end, including signing/notarization/DMG
   stapling, against a real Apple Developer ID (the six repo secrets are
   already set — see `.github/macos-signing.md`).
5. Decide on packaging for the other platforms (Linux, Windows) one by
   one, and execute it.
