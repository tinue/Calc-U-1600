#pragma once
#include <ctime>

// Seeds a machine's real-time clock from the host's local wall-clock time.
// Works for both PC1500Machine and PC1600Machine (same seedClock()
// signature, no shared base). Used by the `- syncclock:` preset step
// (PC1500PresetLoader / PC1600PresetLoader): a preset load runs the
// emulator flat out, so the free-running RTC ends up ahead of real time;
// re-seeding at the end of the script puts it back. Returns the time
// that was seeded (for the loader's log line).
template <typename Machine>
std::tm seedClockFromHostTime(Machine& machine) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    machine.seedClock(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                      local.tm_sec);
    return local;
}
