#pragma once
#include <cstdint>

#include "ExpansionCard.hpp"

// ── The 60-pin system-bus plug: contacts and card interface ──────────────
//
// The PC-1500/1500A and the PC-1600 have the same 60-pin plug. They agree on
// every signal both CPUs have (address, data, PU/PV, INHIBIT, CMTIN/CMTOUT,
// DME0, BFO/φOS) and differ only where the CPUs do
// (Ref/Shared/Expansion-Connectors.md §2.2a, §4.0). A card decodes the
// contacts, never the host: each host drives every contact with what its own
// CPU (or, on the PC-1600, the SC7852 gate array) puts there
// (PC1500SignalDecode::systemBusPins, PC1600BusDrive). The same card
// therefore works on both machines exactly as far as the real one does.

/// Contact numbers. Where the hosts put different signals on one contact,
/// both names are given; a card uses the name its own schematic uses.
namespace Contact60 {
constexpr int kM1   = 10; // PC-1600 M1̄ (PC-1500: PC7, unused)
constexpr int kPT   = 14; // PC-1600 PT, page bank bit 2 (PC-1500: NC)
constexpr int kPU   = 15; // PU; PC-1600: page bank bit 1 (measured, §2.2b)
constexpr int kPV   = 16; // PV; PC-1600 PVOUT, page bank bit 0 (measured)
constexpr int kInh  = 25; // INHIBIT / INH, driven by a card (InhibitSource)
constexpr int kIorq = 26; // PC-1600 IORQ (PC-1500: WEX, a card input)
constexpr int kMreq = 49; // PC-1600 MREQ (PC-1500: NC)
constexpr int kDme0 = 56; // DME0, ME0 area
constexpr int kDme1 = 58; // PC-1500 DME1, ME1 area ...
constexpr int kElh  = 58; // ... PC-1600 ELH̄: the LH5803 owns the bus
constexpr int kMe1  = 59; // PC-1500 ME1 ...
constexpr int kIoe  = 59; // ... PC-1600 IOE: the LH5803's ME1 I/O strobe
} // namespace Contact60

/// One bus cycle as the 60-pin contacts carry it. Strobes are `true` when
/// asserted, whatever their electrical polarity (ELH̄ asserted = LH5803
/// running); PT/PU/PV are plain levels. The address bus is `address`, the
/// data bus the read return value / write argument, and RD/WR (R/W on the
/// PC-1500) is `forWrite`, as in PinState.
struct SystemBusPins {
    uint16_t address = 0;
    bool forWrite = false;
    bool pin[61] = {}; // 1-indexed (index 0 unused)
};

/// How the cards built for the PC-1600's SC7852 (CE-1600P, CE-1600F, a
/// page-1 ROM module) read the contacts. They ignore every cycle while ELH̄
/// is asserted: the CE-1600P gate array qualifies its ROM select CSNO with
/// ELH (Ref/PC-1600/PC-1600-Peripherals-Hardware.md §1.2.2); that its I/O
/// decode does the same is assumed (TODO.md, "Expansion connectors").
namespace Sc7852Decode {
/// A Z-80 memory cycle in page 1 (4000-7FFF), where peripheral ROMs sit.
inline bool page1Memory(const SystemBusPins& p) {
    return p.pin[Contact60::kMreq] && !p.pin[Contact60::kElh] && (p.address & 0xC000) == 0x4000;
}
/// The page's bank number on PT / PU / PVOUT, MSB first.
inline int bank(const SystemBusPins& p) {
    return (p.pin[Contact60::kPT] ? 4 : 0) | (p.pin[Contact60::kPU] ? 2 : 0) | (p.pin[Contact60::kPV] ? 1 : 0);
}
/// A Z-80 I/O cycle (not an interrupt acknowledge); the port is the low
/// address byte.
inline bool io(const SystemBusPins& p) {
    return p.pin[Contact60::kIorq] && !p.pin[Contact60::kM1] && !p.pin[Contact60::kElh];
}
inline uint8_t port(const SystemBusPins& p) { return uint8_t(p.address); }
} // namespace Sc7852Decode

/// A card on the 60-pin plug.
class SystemBusCard : public CardBase {
public:
    /// Return true and set outValue if this card responds to this access.
    virtual bool respondsToRead(const SystemBusPins& pins, uint8_t& outValue) const = 0;
    /// Whether this card claims this write, and whether it stored it.
    virtual WriteResult respondsToWrite(const SystemBusPins& pins, uint8_t value) = 0;

    /// Whether reading this access would change the card's state (a UART
    /// data register), so a debugger view must not read it.
    virtual bool readHasSideEffects(const SystemBusPins& /*pins*/) const { return false; }

    /// Cassette lines (CMTOUT 29, CMTIN 27). The main unit drives CMTOUT;
    /// a card with a tape interface passes it to its MIC jack.
    virtual void cmtOut(bool /*level*/) {}
    /// Return true and set `level` if this card drives CMTIN.
    virtual bool cmtIn(bool& /*level*/) const { return false; }

    /// Elapsed host CPU clocks (LH5801 cycles on the PC-1500, SC7852
    /// T-states on the PC-1600), for the recorder behind a tape interface.
    /// The card only passes them on: the machine's TapeDeck is built with
    /// that host's clock rate.
    virtual void advanceCassette(uint32_t /*clocks*/) {}
};
