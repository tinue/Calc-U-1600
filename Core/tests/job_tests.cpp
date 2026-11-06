// Core/Job.hpp: the coroutine job driver, and the equivalence it promises
// -- a preset run a slice at a time (random budgets, fixed seed) leaves the
// machine exactly as the same preset run straight through: same step log
// (which tags every step with the CPU's PC and the LCD), same result, same
// memory, PC and LCD. ROM-gated parts SKIP without roms/.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "../HostClock.hpp"
#include "../Job.hpp"
#include "../PC1500/PC1500LcdText.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../PC1600/PC1600LcdText.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600PresetLoader.hpp"
#include "../Preset/PresetFile.hpp"
#include "../Resources/BundledRomCatalog.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// A fake machine: records every runCycles() call.
struct FakeMachine {
    std::vector<std::uint64_t> calls;
    std::uint64_t runCycles(std::uint64_t n) {
        calls.push_back(n);
        return n + 1;  // overshoot by one, like a real instruction boundary
    }
};

job::Task<std::uint64_t> inner(FakeMachine& m, int n) {
    std::uint64_t total = 0;
    for (int i = 0; i < n; ++i) total += co_await job::run(m, 10 + i);
    co_return total;
}

job::Task<std::uint64_t> outer(FakeMachine& m) {
    std::uint64_t a = co_await inner(m, 3);
    co_await job::run(m, 100);
    std::uint64_t b = co_await inner(m, 2);
    co_return a + b;
}

job::Task<void> throws(FakeMachine& m) {
    co_await job::run(m, 1);
    throw std::runtime_error("boom");
}

void test_driver() {
    FakeMachine straight;
    const std::uint64_t r1 = job::runToCompletion(outer(straight));
    CHECK(r1 == (11 + 12 + 13) + (11 + 12));
    CHECK((straight.calls == std::vector<std::uint64_t>{10, 11, 12, 100, 10, 11}));

    // Sliced: a request is never split, whatever the budget.
    FakeMachine sliced;
    job::Job<std::uint64_t> j(outer(sliced));
    int steps = 0;
    while (!j.step(5)) ++steps;
    CHECK(steps == 5);  // one request per step at budget 5 (each overshoots it)
    CHECK(sliced.calls == straight.calls);
    CHECK(j.result() == r1);

    // A task that never runs the machine completes on the first step.
    job::Job<int> none([]() -> job::Task<int> { co_return 7; }());
    CHECK(none.step(0));
    CHECK(none.result() == 7);

    // Exceptions come out of result().
    FakeMachine m;
    job::Job<void> bad(throws(m));
    while (!bad.step(1000)) {
    }
    bool caught = false;
    try {
        bad.result();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);

    // A blocking wrapper called inside a job runs to completion inline.
    FakeMachine nested;
    job::Job<std::uint64_t> wrap([](FakeMachine& fm) -> job::Task<std::uint64_t> {
        const std::uint64_t inside = job::runToCompletion(inner(fm, 2));
        co_await job::run(fm, 50);
        co_return inside;
    }(nested));
    while (!wrap.step(1)) {
    }
    CHECK(wrap.result() == 11 + 12);
    CHECK((nested.calls == std::vector<std::uint64_t>{10, 11, 50}));
}

// ---- Equivalence over real presets ----

std::uint64_t fnv(std::uint64_t h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t fnv(std::uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = fnv(h, c);
    return h;
}

struct Outcome {
    bool ok = false;
    std::string error;
    std::vector<std::string> log;
    std::uint64_t fingerprint = 0;
};

std::uint64_t fingerprint(PC1600Machine& m) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t a = 0; a < 0x10000; ++a) h = fnv(h, m.memory().peek(static_cast<std::uint16_t>(a)));
    h = fnv(h, m.sc7852().pc());
    h = fnv(h, m.lh5803().pc());
    h = fnv(h, pc1600LcdText(m).text());
    h = fnv(h, m.ce1600pPlotRevision());
    return h;
}

std::uint64_t fingerprint(PC1500Machine& m) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t a = 0; a < 0x10000; ++a) h = fnv(h, m.memory().peek(static_cast<std::uint16_t>(a)));
    h = fnv(h, m.debugPC());
    h = fnv(h, pc1500LcdText(m).text());
    h = fnv(h, m.ce150PlotRevision());
    return h;
}

const std::vector<std::string> kRomDirs = {"roms"};
const std::string kCardDir = "Qt6/resources/cards";

// `budgetSeed` 0: straight through; otherwise sliced at random budgets.
template <class Machine, class MakeJob>
Outcome runPreset(Machine& m, MakeJob makeJob, unsigned budgetSeed, double frameCycles) {
    Outcome out;
    auto log = [&out](const std::string& line) { out.log.push_back(line); };
    job::Job<PresetLoadResult> j(makeJob(m, log));
    if (budgetSeed == 0) {
        while (!j.step(UINT64_MAX)) {
        }
    } else {
        std::mt19937 rng(budgetSeed);
        std::uniform_int_distribution<std::uint64_t> budget(1, static_cast<std::uint64_t>(frameCycles * 2));
        while (!j.step(budget(rng))) {
        }
    }
    const PresetLoadResult r = j.result();
    out.ok = r.ok;
    out.error = r.error;
    out.fingerprint = fingerprint(m);
    return out;
}

void checkEquivalent(const char* name, const Outcome& a, const Outcome& b) {
    const bool same = a.ok == b.ok && a.error == b.error && a.log == b.log && a.fingerprint == b.fingerprint;
    CHECK(same);
    if (!same) std::fprintf(stderr, "  %s: sliced run differs from the straight one\n", name);
    CHECK(a.ok);
    if (!a.ok) std::fprintf(stderr, "  %s: %s\n", name, a.error.c_str());
}

void equivalence_pc1600(const char* presetPath) {
    PresetFile preset;
    std::string err;
    if (!parsePresetFile(presetPath, &preset, &err)) {
        CHECK(false);
        std::fprintf(stderr, "  %s: %s\n", presetPath, err.c_str());
        return;
    }
    auto make = [&](PC1600Machine& m, PresetLogFn log) {
        return applyPC1600PresetJob(m, preset, std::move(log), ".", kCardDir, {}, kRomDirs);
    };
    Outcome results[2];
    for (int i = 0; i < 2; ++i) {
        PC1600Machine m;
        CHECK(BundledRoms::loadPC1600RomSet(m, kRomDirs, preset.romVariant == "old" ? "old" : "new", &err));
        results[i] = runPreset(m, make, i == 0 ? 0u : 4711u, PC1600Machine::kTStateHz / 60.0);
    }
    checkEquivalent(presetPath, results[0], results[1]);
}

void equivalence_pc1500(const char* presetPath) {
    PresetFile preset;
    std::string err;
    if (!parsePresetFile(presetPath, &preset, &err)) {
        CHECK(false);
        std::fprintf(stderr, "  %s: %s\n", presetPath, err.c_str());
        return;
    }
    auto make = [&](PC1500Machine& m, PresetLogFn log) {
        return applyPC1500PresetJob(m, preset, std::move(log), ".", kCardDir, {}, kRomDirs);
    };
    Outcome results[2];
    for (int i = 0; i < 2; ++i) {
        PC1500Machine m(preset.variant);
        results[i] = runPreset(m, make, i == 0 ? 0u : 4711u, PC1500Machine::kCpuHz / 60.0);
    }
    checkEquivalent(presetPath, results[0], results[1]);
}

} // namespace

int run_job_tests() {
    test_driver();
    if (std::ifstream("roms/PC1600-P0-B0-new.bin").good() && std::ifstream("roms/PC-1500_A04.ROM").good()) {
        // A pinned wall clock: `- syncclock:` must seed the same time both runs.
        hostClockOverride() = [] { return std::chrono::system_clock::time_point(std::chrono::hours(500000)); };
        equivalence_pc1600("Core/tests/fixtures/presets/startup-pc1600.pc1600");
        equivalence_pc1600("Core/tests/fixtures/presets/maxed-out-mem.pc1600");   // typed program, keys, slot cards
        equivalence_pc1600("Core/tests/fixtures/presets/hanoi.pc1600");            // BASIC file + RUN
        equivalence_pc1500("Core/tests/fixtures/presets/startup-pc1500a.pc1500a");
        equivalence_pc1500("Core/tests/fixtures/presets/memtest_stock.pc1500");  // machine code + typed CALL
        hostClockOverride() = nullptr;
    } else {
        std::printf("job_tests: preset equivalence SKIP (no ROMs in roms/)\n");
    }
    std::printf("job_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
