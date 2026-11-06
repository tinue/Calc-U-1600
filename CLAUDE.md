# Calc-U-1600

Before removing, simplifying or "fixing" code in a review or cleanup, read `docs/background/Decisions.md`. It lists deliberate choices that look wrong at first sight. When a recurring review question gets settled, add it there.

## Test builds

Run the test suite optimized: `tools/run_tests.sh` (`-O1`) or the `CoreTests` target in `build/` (`-O1` in Debug). When chasing a bug — a debugger, stepping, inspecting variables — first build a full-debug tree and work there, leaving `build/` as it is:

```sh
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCALCU_TESTS_DEBUG=ON
cmake --build build-debug --target CoreTests
```

## Core, Shared, frontends

Three layers (docs/developer/Shared-Architecture.md): `Core/` is the hardware, `Shared/` all app logic that isn't drawing, `Qt6/app/` the desktop GUI (a WebAssembly GUI will be the second frontend).

- Non-GUI logic goes in `Shared/` (or `Core/`), never in `Qt6/app/`; Qt classes translate types and show what Shared decides.
- Core and Shared: portable C++20, standard library only, single-threaded, never blocking. Long machine work is a resumable job (`Core/Job.hpp`: `co_await job::run(machine, n)`); coroutine parameters by value.
- A change to `Core/` or `Shared/` passes `tools/run_tests.sh` and `tools/build_wasm_check.sh`, and the Qt app builds.

