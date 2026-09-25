#pragma once
#include <cstdint>

// The PC-1600's two master clocks, shared by every block that counts
// emulated time (PC1600Machine re-exports them as kTStateHz/kLH5803Hz).
// SC-7852 T-states at 3.58 MHz (TRM 7.5) -- this machine's canonical unit
// of emulated time.
inline constexpr uint32_t kPC1600TStateHz = 3580000;
// phi-OS, 1.3 MHz: the LH-5803's clock, also divided down for the LCD
// clock and the F-register buzzer modulator.
inline constexpr uint32_t kPC1600PhiOsHz = 1300000;
