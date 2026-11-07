#pragma once
#include <cstdint>

#include "../Connector/SystemBusCard.hpp"

// ── What the PC-1600 puts on its 60-pin contacts ──────────────────────────
//
// One connector, two CPUs: the SC7852 drives it while the Z-80 runs; while
// ELH̄ is asserted the LH5803 owns the bus, and the SC7852 pins carry its
// signals (Service Manual SC7852 pin table, printed pp. 20-21). These
// functions build the contacts of one cycle for PC1600Memory's
// systemBus(); which cycles reach the plug at all is the callers' address
// decode. Contacts whose level isn't known are left inactive and listed in
// TODO.md, "Expansion connectors".
namespace PC1600BusDrive {

/// An LH5803 cycle (ELH̄ asserted).
/// - PU (15) is one line shared with the SC7852's PU output; the LH5803's PV
///   leaves through PVIN -> PVOUT (16). Ref/Shared/Expansion-Connectors.md
///   §2.2b.
/// - ME0 is MREQ (49). DME0 (56) is asserted too: the TRM names the contact
///   DME0 like the PC-1500's, and the PC-1500 peripherals (CE-150, CE-158)
///   select their ROMs on it. Assumed, not measured.
/// - ME1 is IORQ (26). IOE (59) is the SC7852's decoded strobe for LH5803
///   ME1 accesses to xx00-xx0F and 8000-FFFF -- the PC-1500 peripherals'
///   ME1 contact.
inline SystemBusPins lh5803Pins(uint16_t addr, bool forWrite, bool me1, bool pu, bool pv) {
    SystemBusPins pins;
    pins.address = addr;
    pins.forWrite = forWrite;
    pins.pin[Contact60::kElh] = true;
    pins.pin[Contact60::kPU] = pu;
    pins.pin[Contact60::kPV] = pv;
    if (me1) {
        pins.pin[Contact60::kIorq] = true;
        pins.pin[Contact60::kIoe] = (addr & 0x00F0) == 0 || addr >= 0x8000;
    } else {
        pins.pin[Contact60::kMreq] = true;
        pins.pin[Contact60::kDme0] = true;
    }
    return pins;
}

/// A Z-80 memory cycle: MREQ, and the bank number of the page being
/// accessed on PT / PU / PVOUT, MSB first (TRM §7.2.1; for page 1 Port 31H
/// b3/b2/b1). Not driven: M1 (instruction fetches look like data reads) and
/// DME0 -- unknown on Z-80 cycles.
inline SystemBusPins z80MemPins(uint16_t addr, bool forWrite, uint8_t bank) {
    SystemBusPins pins;
    pins.address = addr;
    pins.forWrite = forWrite;
    pins.pin[Contact60::kMreq] = true;
    pins.pin[Contact60::kPT] = (bank & 4) != 0;
    pins.pin[Contact60::kPU] = (bank & 2) != 0;
    pins.pin[Contact60::kPV] = (bank & 1) != 0;
    return pins;
}

/// A Z-80 I/O cycle (`IN`/`OUT`): IORQ, the port on A0-A7. A8-A15 carry the
/// B register on a real Z-80 and stay 0 here; PT / PU / PVOUT are unknown
/// on I/O cycles and stay low.
inline SystemBusPins z80IoPins(uint8_t port, bool forWrite) {
    SystemBusPins pins;
    pins.address = port;
    pins.forWrite = forWrite;
    pins.pin[Contact60::kIorq] = true;
    return pins;
}

} // namespace PC1600BusDrive
