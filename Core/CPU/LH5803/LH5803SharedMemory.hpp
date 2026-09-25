#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../LH5801/LH5801.hpp"
#include "LH5803Rom.hpp"
#include "../../Connector/Ce150Card.hpp"
#include "../../Connector/Ce158Card.hpp"

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
//   8000-BFFF  peripheral ROM window, selected by the LH5803's own PV:
//              CE-150 ROM (PV=0, A000-BFFF) or CE-158 ROM (PV=1,
//              8000-9FFF, PU picks its 8 KB bank). Open bus where no
//              attached card answers. The LH5803 ROM sets PV itself from
//              CALLH's PARBAN (E224: RPV, then SPV if (700EH) bit 0).
//   C000-FFFF  LH5803-private internal ROM (PC1600-LH5803-C000-FFFF-new.bin), fixed, loadable
//
// ME1 defaults to aliasing ME0, EXCEPT:
//   * ME1 0x8000-0xBFFF: an I/O cycle on the bus (the LH5803's ME1 is the
//     SC7852's IORQ), so it never selects the ME0 peripheral-ROM window.
//     Passed to the cards with me1=true -- the CE-150's LH5810 answers at
//     0xB008-0xB00F -- and open bus otherwise.
//   * 0xA038: `STA #(0A038H)` is the LH5803-side handoff trigger (the
//     LH5803-side alias of the SC7852's Port 38H, since the LH5803 has no
//     I/O space of its own) -- forwarded to a PC1600BusArbiter via
//     setBusArbiter() if one is attached.
//   * ME1 0xF000-0xF00F: the LH5803's own on-chip LH5811-compat PIO port
//     controller -- the analogue of the chip PC1500Memory models for the
//     PC-1500's LH5801 (see that file's top comment / its `case 0xB`).
//     Modelled here as a plain 16-byte register file: writes latch, reads
//     return the latched byte. It MUST NOT fall through to readME0(), where
//     0xF00x >= kRomBase would serve PC1600-LH5803-C000-FFFF-new.bin bytes --
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

    /// Cache the LH5803's PU/PV flip-flop state (SPV/RPV/SPU/RPU never touch
    /// the bus themselves, so pushing the post-instruction value here is
    /// enough for the next access to see it -- same contract as
    /// PC1500Memory::updatePUPV). PV selects between the CE-150 and CE-158
    /// halves of the 8000-BFFF window; PU picks the CE-158's ROM bank.
    void updatePUPV(bool pu, bool pv) { m_pu = pu; m_pv = pv; }
    bool pv() const { return m_pv; }

    /// Attach/detach a CE-150 plotter (the same card the PC-1500 uses;
    /// non-owning, PC1600Machine owns it). Once attached the 8000-BFFF
    /// window serves the CE-150 ROM at PV=0 and ME1 0xB008-0xB00F reaches
    /// its LH5810 -- the CE-150 firmware then runs on the LH5803 exactly as
    /// it does on a PC-1500's LH5801 (BASIC in MODE 1).
    void attachCe150(Ce150Card* card) { m_ce150 = card; }
    void detachCe150() { m_ce150 = nullptr; }
    bool ce150Attached() const { return m_ce150 != nullptr; }

    /// Attach/detach a CE-158 interface (the PC-1500's card, non-owning).
    /// Its ROM answers in 8000-9FFF at PV=1; its LH5811 (ME1 0xD000-0xD1FF),
    /// UART (0xD200-0xD3FF) and interrupt-ID register (0xDE00-0xDFFF) are
    /// routed to it. Coexists with a CE-150.
    void attachCe158(Ce158Card* card) { m_ce158 = card; }
    void detachCe158() { m_ce158 = nullptr; }
    bool ce158Attached() const { return m_ce158 != nullptr; }

    /// Loads the LH5803-private internal ROM at C000-FFFF (PC1600-LH5803-C000-FFFF-new.bin).
    /// Returns false (untouched) if `size` isn't exactly 16384 bytes.
    bool loadROM(const uint8_t* data, size_t size) { return m_rom.load(data, size); }
    bool loadROMFile(const std::string& path) { return m_rom.loadFile(path); }

    /// Clear volatile I/O state -- the internal-PIO register file at ME1
    /// 0xF000-0xF00F. ROM/PV/card attachment are untouched (matching
    /// PC1500Memory::reset()'s scope). Called from PC1600Machine::reset*().
    void reset() { m_ioRegs.fill(0); }

    /// Debugger view of the LH5803's ME0/ME1 without bus side effects.
    /// The CE-158's registers, the UART / sub-CPU block and the cards' ME1
    /// I/O windows can't be read without disturbing them: `*readable` is
    /// false there and 0xFF is returned. The internal PIO reads as its
    /// latched register file.
    uint8_t debugPeek(uint16_t addr, bool me1, bool* readable) const;

    // LH5801Bus
    uint8_t readME0(uint16_t addr) override;
    void    writeME0(uint16_t addr, uint8_t value) override;
    uint8_t readME1(uint16_t addr) override;
    void    writeME1(uint16_t addr, uint8_t value) override;

private:
    static constexpr uint16_t kRomBase = LH5803Rom::kBase;
    static constexpr uint16_t kHandoffTriggerAddr = 0xA038;

    /// True and fills *reg (0..3, the A1:A0 UART register) / *isSubCpuAnswer
    /// when `addr` is an ME1 access into the UART / sub-CPU block (offsets
    /// 20H-27H or 33H, in either the bare or the 0xA0xx-shadowed form).
    static bool isUartShadow(uint16_t addr, uint8_t* reg, bool* isSubCpuAnswer);

    /// Build the PinState the 60-pin peripheral cards decode on (address,
    /// forWrite, me1, PV, PU) -- no S-block/Y strobe, so no
    /// PC1500SignalDecode dependency here. See Ce150Card.hpp's "Bus
    /// dependency" note.
    PinState peripheralPins(uint16_t addr, bool forWrite, bool me1) const {
        PinState p;
        p.address = addr;
        p.forWrite = forWrite;
        p.me1 = me1;
        p.pin[2] = m_pv; // PV
        p.pin[3] = m_pu; // PU
        return p;
    }

    /// Offers an access to the attached cards, CE-158 first; 0xFF (open
    /// bus) / ignored when none claims it.
    uint8_t cardRead(uint16_t addr, bool me1) const {
        if (!m_ce158 && !m_ce150) return 0xFF;
        uint8_t v = 0xFF;
        const PinState p = peripheralPins(addr, /*forWrite=*/false, me1);
        if (m_ce158 && m_ce158->respondsToRead(p, v)) return v;
        if (m_ce150 && m_ce150->respondsToRead(p, v)) return v;
        return 0xFF;
    }
    void cardWrite(uint16_t addr, bool me1, uint8_t value) {
        if (!m_ce158 && !m_ce150) return;
        const PinState p = peripheralPins(addr, /*forWrite=*/true, me1);
        if (m_ce158 && m_ce158->respondsToWrite(p, value)) return;
        if (m_ce150) m_ce150->respondsToWrite(p, value);
    }

    /// The CE-158's ME1 register blocks: LH5811 + UART 0xD000-0xD3FF,
    /// interrupt-ID 0xDE00-0xDFFF (see Ce158Card).
    static bool isCe158Io(uint16_t addr) {
        return (addr >= Ce158Card::kPioBase && addr <= Ce158Card::kUartEnd) ||
               (addr >= Ce158Card::kIntIdBase && addr <= Ce158Card::kIntIdEnd);
    }

    PC1600Memory& m_shared;
    PC1600BusArbiter* m_arbiter{nullptr};
    Ce150Card* m_ce150{nullptr};
    Ce158Card* m_ce158{nullptr};
    LH5803Rom m_rom;
    std::array<uint8_t, 16> m_ioRegs{}; // internal LH5811-compat PIO, ME1 0xF000-0xF00F
    bool m_pv{false};
    bool m_pu{false};
};
