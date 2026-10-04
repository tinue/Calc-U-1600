# Calc-U-1600

Before removing, simplifying or "fixing" code in a review or cleanup, read `docs/background/Decisions.md`. It lists deliberate choices that look wrong at first sight. When a recurring review question gets settled, add it there.

## Test builds

Run the test suite optimized: `tools/run_tests.sh` (`-O1`) or the `CoreTests` target in `build/` (`-O1` in Debug). When chasing a bug — a debugger, stepping, inspecting variables — first build a full-debug tree and work there, leaving `build/` as it is:

```sh
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCALCU_TESTS_DEBUG=ON
cmake --build build-debug --target CoreTests
```
