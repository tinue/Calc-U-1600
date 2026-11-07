#pragma once
#include <cstdio>

// Assert-and-tally checks for the Shared/ tests, same style as Core/tests.
// Each test file keeps its own counters in an anonymous namespace and
// returns its failure count from run_shared_<name>_tests().
#define SHARED_CHECK(pass, fail, cond) do { \
    if (cond) { (pass)++; } \
    else { (fail)++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
