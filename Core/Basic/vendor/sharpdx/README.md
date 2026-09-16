# Vendored `libsharpdx` (SharpDataExchangeRust C ABI)

`libsharpdx.a` + `sharpdx.h` are the prebuilt static library and generated C
header of the **`convert`**-verb tokenizer from
[`SharpDataExchangeRust`](../../../../SharpDataExchangeRust) (crate `sharpdx`,
version 0.1.4). Calc-U-1600 links it so a `program: format: basic-binary`
preset section can point `path:` at a plain-text `.bas` listing and have it
tokenized in-process at load time (see `Core/Basic/BasicProgramSource.cpp`).

We vendor the built artifact rather than adding a Rust toolchain to the
Calc-U-1600 build. Only `sde_tokenize` / `sde_detect` / `sde_last_error` /
`sde_buf_free` are used; the library does no file I/O and never panics across
the FFI boundary.

## Architecture

`libsharpdx.a` here is **arm64 only** (matches the default Apple-Silicon
`ARCHS_STANDARD` / `ONLY_ACTIVE_ARCH` dev build). A universal build needs an
`x86_64-apple-darwin` slice `lipo`-ed in -- `tools/refresh_sharpdx.sh --universal`.

## Linking

`Core/Basic` is an Xcode file-system-synchronized group, so Xcode links
`libsharpdx.a` automatically once it is on disk here; the app target just
adds `Core/Basic/vendor/sharpdx` to `LIBRARY_SEARCH_PATHS`. The headless
CLI / test builds (`tools/*.sh`) pass the archive path on the clang++
command line.

`ld` prints a handful of `object file ... was built for newer 'macOS'
version (26.0) than being linked` warnings: those are the Rust toolchain's
**prebuilt** `std` / `core` / `alloc` objects, which carry the toolchain's
own min-OS and are not recompiled by `MACOSX_DEPLOYMENT_TARGET`. Harmless;
our own crate objects are built at 15.6 by `refresh_sharpdx.sh`.

## Refreshing

Run `tools/refresh_sharpdx.sh` from the repo root after the sibling
`SharpDataExchangeRust` checkout changes. It runs `cargo build --release`
there and copies `target/release/libsharpdx.a` + `include/sharpdx.h` over
these files. Commit the result.
