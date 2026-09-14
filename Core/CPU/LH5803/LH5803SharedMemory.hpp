#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../LH5801/LH5801.hpp"
#include "../../Connector/Ce150Card.hpp"

class PC1600Memory;
class PC1600BusArbiter;

// ── LH5803-side memory map, wired to the shared Z-80-side RAM ──────────
//
// This class forwards the LH5803's 0000-7FFF window to the SAME
// PC1600Memory instance the SC7852 side uses, so both CPUs share one
// physical RAM array (unlike the standalone LH5803Memory, which has its
// own private 16KB RAM array for testing the LH5803 core in isolation).
//
// The LH5803's own 0000-7FFF exactly aliases the Z-80's 8000-FFFF (Slot
// 1/2 at LH5803 0000-3FFF = Z-80 8000-BFFF; fixed internal RAM at LH5803
// 4000-7FFF = Z-80 C000-FFFF Bank 0) -- both ranges combine into one
// uniform `+0x8000` offset onto PC1600Memory's own address space, so no
// separate bank-vs-RAM branching is needed here; PC1600Memory's own page
// decode already resolves whichever Z-80 bank is currently switched into
// Slot 1/2.
//
//   0000-7FFF  forwarded to sharedMem.read/write(addr + 0x8000)
//   8000-BFFF  CE-158 ROM (PVOUT=1) or CE-150 ROM (PVOUT=0) window --
//              open bus, no peripheral ROM backing it
//   C000-FFFF  LH5803-private internal ROM (PC1600-LH5803-C000-FFFF.bin), fixed, loadable
//
// ME1 defaults to aliasing ME0, EXCEPT:
//   * 0xA038: `STA #(0A038H)` is the LH5803-side handoff trigger (the
//     LH5803-side alias of the SC7852's Port 38H, since the LH5803 has no
//     I/O space of its own) -- forwarded to a PC1600BusArbiter via
//     setBusArbiter() if one is attached.
//   * ME1 0xF000-0xF00F: the LH5803's own on-chip LH5811-compat PIO port
//     controller -- the analogue of the chip PC1500Memory models for the
//     PC-1500's LH5801 (see that file's top comment / its `case 0xB`).
//     Modelled here as a plain 16-byte register file: writes latch, reads
//     return the latched byte. It MUST NOT fall through to readME0(), where
//     0xF00x >= kRomBase would serve PC1600-LH5803-C000-FFFF.bin bytes --
//     the CE-150 cartridge's per-plot-point pacing poll `BII #(0xF00B),0x02`
//     (system-ROM helper at E451) depends on reading register 0xB (IF,
//     input flags) as software-only state, not ROM data. Bit 1 of IF is
//     the shared TP-edge / BREAK flag: no clear-on-read (the firmware
//     clears it itself with `ani #(0xF00B),0xFD`), and no live PC-1600
//     TP/RTC rising-edge source feeds it, so it always reads 0 during a
//     CE-150 plot loop.
//   * the TC8576F UART / LU-57813P sub-CPU register block. The LH-5803
//     drives the OFF-path clock save through it -- `rom1500 E538`:
//     `bii #(0x0023),0x20` (poll a UART status bit), `sta #(0x0021)`
//     (parallel-out = sub-CPU command), `lda #(0x0033)` (sub-CPU answer).
//     The `#(...)` addresses are literal ME1 offsets 20H-27H / 33H;
//     0xA020-0xA033 (the 30-3FH shadow at #A03xH convention, extended to
//     the UART block) are routed the same way for safety.
//     TODO(trace): pin down which of the two forms the ROM actually uses.
class LH5803SharedMemory : public LH5801Bus {
public:
    explicit LH5803SharedMemory(PC1600Memory& sharedMem) : m_shared(sharedMem) {}

    void setBusArbiter(PC1600BusArbiter* arbiter) { m_arbiter = arbiter; }

    /// Cache the LH5803's PV flip-flop state (SPV/RPV never touch the bus
    /// themselves, so pushing the post-instruction value here is enough for
    /// the next access to see it -- same contract as PC1500Memory::updatePUPV).
    /// PV gates the CE-150 ROM window in the 8000-BFFF region. PU is
    /// accepted for call-site symmetry with the LH5801 path but not cached --
    /// nothing on the LH5803 bus consumes it yet.
    void updatePUPV(bool /*pu*/, bool pv) { m_pv = pv; }
    bool pv() const { return m_pv; }

    /// Attach/detach a CE-150 plotter (the same card the PC-1500 uses;
    /// non-owning, PC1600Machine owns it). Once attached the 8000-BFFF
    /// window serves the CE-150 ROM at PV=0 and ME1 0xB008-0xB00F reaches
    /// its LH5810 -- the CE-150 firmware then runs on the LH5803 exactly as
    /// it does on a PC-1500's LH5801 (BASIC in MODE 1).
    void attachCe150(Ce150Card* card) { m_ce150 = card; }
    void detachCe150() { m_ce150 = nullptr; }
    bool ce150Attached() const { return m_ce150 != nullptr; }

    /// Loads the LH5803-private internal ROM at C000-FFFF (PC1600-LH5803-C000-FFFF.bin).
    /// Returns false (untouched) if `size` isn't exactly 16384 bytes.
    bool loadROM(const uint8_t* data, size_t size);
    bool loadROMFile(const std::string& path);

    /// Clear volatile I/O state -- the internal-PIO register file at ME1
    /// 0xF000-0xF00F. ROM/PV/card attachment are untouched (matching
    /// PC1500Memory::reset()'s scope). Called from PC1600Machine::reset*().
    void reset() { m_ioRegs.fill(0); }

    // LH5801Bus
    uint8_t readME0(uint16_t addr) override;
    void    writeME0(uint16_t addr, uint8_t value) override;
    uint8_t readME1(uint16_t addr) override;
    void    writeME1(uint16_t addr, uint8_t value) override;

private:
    static constexpr uint16_t kRomBase = 0xC000;
    static constexpr size_t   kRomSize = 0x4000; // 16384B
    static constexpr uint16_t kHandoffTriggerAddr = 0xA038;

    /// True and fills *reg (0..3, the A1:A0 UART register) / *isSubCpuAnswer
    /// when `addr` is an ME1 access into the UART / sub-CPU block (offsets
    /// 20H-27H or 33H, in either the bare or the 0xA0xx-shadowed form).
    static bool isUartShadow(uint16_t addr, uint8_t* reg, bool* isSubCpuAnswer);

    /// Build the 4-field PinState `Ce150Card` decodes on (address, forWrite,
    /// me1, PV) -- no S-block/Y strobe, so no PC1500SignalDecode dependency
    /// here. See Ce150Card.hpp's "Bus dependency" note.
    static PinState ce150Pins(uint16_t addr, bool forWrite, bool me1, bool pv) {
        PinState p;
        p.address = addr;
        p.forWrite = forWrite;
        p.me1 = me1;
        p.pin[2] = pv; // PV
        return p;
    }

    PC1600Memory& m_shared;
    PC1600BusArbiter* m_arbiter{nullptr};
    Ce150Card* m_ce150{nullptr};
    std::array<uint8_t, kRomSize> m_rom{};
    std::array<uint8_t, 16> m_ioRegs{}; // internal LH5811-compat PIO, ME1 0xF000-0xF00F
    bool m_romLoaded{false};
    bool m_pv{false};
};
