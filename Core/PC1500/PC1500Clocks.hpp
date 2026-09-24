#pragma once
#include <cstdint>

// The PC-1500/1500A's CPU clock: the ~2.6 MHz crystal divided by 2 -- the
// LH5801's cycle rate and this machine's canonical unit of emulated time
// (PC1500Machine re-exports it as kCpuHz). Everything that counts emulated
// time on the PC-1500 side derives from this, like kPC1600TStateHz on the
// PC-1600 (Core/PC1600/PC1600Clocks.hpp).
inline constexpr uint32_t kPC1500CpuHz = 1300000;
