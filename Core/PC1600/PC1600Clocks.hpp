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
// CL1/CL2, the sub-CPU's 1.2288 MHz ceramic resonator. The gate array also
// passes CL2 on as the TC8576F's XCLK (baud and handshake timing).
// PC-1600 Service Manual §9-3; PC-1600-CPC-TC8576.md §5.3.
inline constexpr uint32_t kPC1600Cl2Hz = 1228800;
