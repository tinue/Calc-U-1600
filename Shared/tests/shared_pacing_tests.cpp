// Shared/Pacer, Shared/AudioPump.

#include <cstdio>
#include <vector>

#include "Audio/PiezoSampler.hpp"
#include "Shared/AudioPump.hpp"
#include "Shared/Pacer.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

constexpr std::int64_t kMs = 1000000;

void test_absolute_deadline() {
    std::int64_t now = 0;
    shared::Pacer pacer([&] { return now; });
    const double hz = 1000000.0;  // 1 MHz: 1000 cycles per ms
    CHECK(pacer.cyclesDue(now, hz) == 0);  // first sight of a clock rate rebases
    // Jittery ticks (13, 19, 16, 17, 15 ms) still add up to exactly the
    // elapsed time: no per-tick rounding or re-measuring builds up.
    std::uint64_t total = 0;
    for (std::int64_t step : {13, 19, 16, 17, 15}) {
        now += step * kMs;
        total += pacer.cyclesDue(now, hz);
    }
    CHECK(total == 80000);
    // Sub-cycle steps carry over instead of being lost.
    std::uint64_t small = 0;
    for (int i = 0; i < 1000; ++i) {
        now += 700;  // 0.7 cycles
        small += pacer.cyclesDue(now, hz);
    }
    CHECK(small == 700);
}

void test_stall_and_restart() {
    std::int64_t now = 0;
    shared::Pacer pacer([&] { return now; });
    const double hz = 1000000.0;
    pacer.cyclesDue(now, hz);
    now += 2000 * kMs;  // a 2 s stall: one capped tick, the rest dropped
    CHECK(pacer.cyclesDue(now, hz) == 100000);
    now += 16 * kMs;
    CHECK(pacer.cyclesDue(now, hz) == 16000);
    // restart(): time spent elsewhere isn't caught up.
    now += 500 * kMs;
    pacer.restart();
    now += 10 * kMs;
    CHECK(pacer.cyclesDue(now, hz) == 10000);
    // A clock-rate change (model switch) rebases too.
    now += 10 * kMs;
    CHECK(pacer.cyclesDue(now, 2000000.0) == 0);
    now += 10 * kMs;
    CHECK(pacer.cyclesDue(now, 2000000.0) == 20000);
}

struct FakeSink : shared::AudioSink {
    bool open = false;
    int starts = 0;
    std::size_t roomLeft = 100000;
    std::vector<std::int16_t> written;
    bool start(int, int) override {
        open = true;
        ++starts;
        return true;
    }
    void stop() override { open = false; }
    std::size_t room() const override { return roomLeft; }
    void write(const std::int16_t* s, std::size_t n) override {
        written.insert(written.end(), s, s + n);
        roomLeft -= std::min(roomLeft, n);
    }
    bool ranDry() const override { return false; }
};

void test_audio_pump() {
    FakeSink sink;
    std::vector<std::string> warnings;
    shared::AudioPump pump(sink, [&](const std::string& w) { warnings.push_back(w); });
    const std::vector<std::int16_t> silence(800, 0);
    const std::vector<std::int16_t> tone(800, 5000);

    pump.feed(silence.data(), silence.size());
    CHECK(!sink.open);  // silence alone never opens the device
    pump.feed(tone.data(), tone.size());
    CHECK(sink.open && sink.starts == 1);
    // Primed with 40 ms of silence, then the tone.
    const std::size_t prime = static_cast<std::size_t>(PiezoSampler::kDefaultSampleRate * 40 / 1000);
    CHECK(sink.written.size() == prime + tone.size());

    // No room: the backlog is trimmed once it passes 50 ms.
    sink.roomLeft = 0;
    for (int i = 0; i < 4; ++i) pump.feed(tone.data(), tone.size());
    CHECK(!warnings.empty());

    // ~1 s of silence closes the device.
    sink.roomLeft = 1000000;
    const std::vector<std::int16_t> longSilence(static_cast<std::size_t>(PiezoSampler::kDefaultSampleRate), 0);
    pump.feed(longSilence.data(), longSilence.size());
    CHECK(!sink.open);
}

} // namespace

int run_shared_pacing_tests() {
    test_absolute_deadline();
    test_stall_and_restart();
    test_audio_pump();
    std::printf("shared_pacing_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
