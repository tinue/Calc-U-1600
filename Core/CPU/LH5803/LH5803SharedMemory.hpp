#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../LH5801/LH5801.hpp"
#include "LH5803Rom.hpp"
#include "../../PC1600/PC1600BusDrive.hpp"

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
//   0000-7FFF  forwarded to sharedMem.read/write(addr + 0x8000), except that
//              7400-744F / 7500-754F land on 7600-764F / 7700-774F (lha90());
//              an ME0 write to the PC-1500 display RAM 7600-764F is also
//              drawn on the LCD (the gate array's mirror, see
//              mirrorPc1500Display())
//   8000-BFFF  peripheral ROM window, offered to the cards on
//              PC1600Memory::systemBus(): CE-150 ROM (PV=0,
//              A000-BFFF) or CE-158 ROM (PV=1, 8000-9FFF, PU picks its
//              8 KB bank). Open bus where no attached card answers. The
//              LH5803 ROM sets PV itself from CALLH's PARBAN (E224: RPV,
//              then SPV if (700EH) bit 0).
//   C000-FFFF  LH5803-private internal ROM (PC1600-LH5803-C000-FFFF-new.bin), fixed, loadable
//
// ME1 defaults to aliasing ME0, EXCEPT:
//   * ME1 0x8000-0xFFFF (after the mainboard decodes below): offered to the
//     cards with me1=true, terminal when one claims it -- the CE-150's
//     LH5810 at 0xB008-0xB00F, the CE-158's LH5811 / UART / interrupt-ID
//     blocks at 0xD000-0xD3FF / 0xDE00-0xDFFF. This host doesn't know
//     which card sits where.
//   * ME1 0x8000-0xBFFF that no card claims: an I/O cycle on the bus (the
//     LH5803's ME1 is the SC7852's IORQ), so it never selects the ME0
//     peripheral-ROM window -- open bus.
//   * 0xA038: `STA #(0A038H)` is the LH5803-side handoff trigger (the
//     LH5803-side alias of the SC7852's Port 38H, since the LH5803 has no
//     I/O space of its own) -- forwarded to a PC1600BusArbiter via
//     setBusArbiter() if one is attached.
//   * the rest of ME1 0xA030-0xA03F: the SC7852's 30H-3FH control ports,
//     straight to PC1600Memory::readIO/writeIO -- P_MOD (30H), P_BANK
//     (31H), P_INT (32H), P_LHMSK2 (34H), P_CL1 (36H) in rom1500. P_BANK
//     is what MODE 1 PEEK/XPEEK depend on (P_MAPPRG, rom1500 E63C). The
//     32H read clears the cause as on the Z-80 side; only the LH5803 ISR
//     (E6B9) reads it, and no LH5803 maskable interrupt is raised yet. A
//     36H write acknowledges the SC7852's LHNMIO trap (the NMI handler,
//     C491H).
//   * ME1 0xA040-0xA05F and 0x8040-0x805F: Z-80 ports 40H-5FH, the
//     HD61102 LCD drivers (50H/58H/5BH), the same way. The LH5803 ROM
//     drives the LCD directly through both forms: LCD1500_ALL (E84AH, the
//     PC-1500 window redraw the NMI handler calls) and LCD1500_CLR (E8BFH)
//     poll the busy bit with `BII #(Y),80H` at Y = A058H/A059H;
//     LCD1500_BYTE (E7E1H, one display-RAM byte: GPRINT and the CE-158
//     terminal's received characters) at Y = 8059H/8055H. The SC7852
//     evidently ignores A8-A14 of an LH5803 I/O cycle; only the forms the
//     ROM uses are routed.
//   * ME1 0xF000-0xF00F: the SC7852's LH5810-compatible port block, the
//     same registers the Z-80 reaches at ports 10H-1FH (F00xH = port 1xH)
//     -- the PC-1500's own LH5810 address, so PC-1500 code finds it where
//     it expects. Evidence: the SC7852's PCSTB pin goes high when "the
//     Z-80 writes 18H or the LH-5803 is at F008H in ME1" (TRM §7.1.1), and
//     the LH5803 ROM's DELAY64 (EEB2H) counts the sub-CPU's 1/64 s pulse
//     on #(F00FH) bit 5 = PB5 (port 1FH). Routed to PC1600Memory::readIO/
//     writeIO, so PB5 runs, the keyboard strobes and the IF/MSK registers
//     are the real ones. It must not fall through to readME0(), where
//     0xF00x >= kRomBase would serve LH5803 ROM bytes (the CE-150 plot
//     loop's `BII #(0xF00B),0x02` pacing poll reads IF there).
//   * the TC8576F UART / LU-57813P sub-CPU register block. The LH-5803
//     drives the OFF-path clock save through it -- `rom1500 E538`:
//     `bii #(0x0023),0x20` (poll a UART status bit), `sta #(0x0021)`
//     (parallel-out = sub-CPU command), `lda #(0x0033)` (sub-CPU answer).
//     The `#(...)` addresses are literal ME1 offsets 20H-27H / 33H, and
//     that is the only form rom1500 uses for this block (its #(A03xH)
//     accesses all go to the 30-3FH control ports). 0xA020-0xA033 is
//     routed the same way by analogy with that A03xH shadow; no ROM path
//     depends on it and it is unverified on hardware.
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

    /// Loads the LH5803-private internal ROM at C000-FFFF (PC1600-LH5803-C000-FFFF-new.bin).
    /// Returns false (untouched) if `size` isn't exactly 16384 bytes.
    bool loadROM(const uint8_t* data, size_t size) { return m_rom.load(data, size); }
    bool loadROMFile(const std::string& path) { return m_rom.loadFile(path); }

    /// Nothing volatile of its own: the ME1 F000-F00F port block lives in
    /// PC1600Memory (ports 10H-1FH) and resets there. ROM/PV/card
    /// attachment are untouched. Called from PC1600Machine::reset*().
    void reset() {}

    /// Debugger view of the LH5803's ME0/ME1 without bus side effects.
    /// The UART / sub-CPU block, ME1 8000-BFFF and any card register a read
    /// would disturb (SystemBusCard::readHasSideEffects) can't be read
    /// without side effects: `*readable` is false there and 0xFF is
    /// returned. The F000-F00F port block reads as Z-80 ports 10H-1FH.
    uint8_t debugPeek(uint16_t addr, bool me1, bool* readable) const;

    /// The LH5803's ME0 0000-7FFF is the Z-80's 8000-FFFF (the shared RAM
    /// and the slot windows); false for LH5803 addresses outside it.
    static constexpr bool toZ80Address(uint16_t lhAddr, uint16_t* z80Addr) {
        if (lhAddr >= 0x8000) return false;
        *z80Addr = static_cast<uint16_t>(lhAddr + 0x8000);
        return true;
    }

    // LH5801Bus
    uint8_t readME0(uint16_t addr) override;
    void    writeME0(uint16_t addr, uint8_t value) override;
    uint8_t readME1(uint16_t addr) override;
    void    writeME1(uint16_t addr, uint8_t value) override;
    /// ITA: the keyboard's sense lines, as the Z-80 reads them at port 37H.
    /// The LH5803's IN0-IN7 (pins 66-73) are on the same net as the SC7852's
    /// KIN0-KIN7 and the key PWB (CN1-03..10): TRM §9.1(1) F.P.C. circuit
    /// diagram, printed p.264. The LH5803 ROM scans the keyboard the
    /// PC-1500 way -- strobes on #(F00EH) (port 1EH, the SC7852's PA), then
    /// ITA (ISKEY, E41AH).
    uint8_t readInputPort() override;

private:
    static constexpr uint16_t kRomBase = LH5803Rom::kBase;
    static constexpr uint16_t kHandoffTriggerAddr = 0xA038;

    /// True and fills *reg (0..3, the A1:A0 UART register) / *isSubCpuAnswer
    /// when `addr` is an ME1 access into the UART / sub-CPU block (offsets
    /// 20H-27H or 33H, in either the bare or the 0xA0xx-shadowed form).
    static bool isUartShadow(uint16_t addr, uint8_t* reg, bool* isSubCpuAnswer);

    /// ME1 A030-A03F: the SC7852's 30H-3FH control ports as the LH5803 sees
    /// them (rom1500's P_MOD..P_CPUSW). A033 is caught by isUartShadow()
    /// first and A038 by the handoff check; the rest go to readIO/writeIO.
    static bool isControlPort(uint16_t addr) { return (addr & 0xFFF0) == 0xA030; }
    /// ME1 A040-A05F / 8040-805F: Z-80 ports 40H-5FH (the LCD), also via
    /// readIO/writeIO.
    static bool isLcdPort(uint16_t addr) { return (addr & 0xDFE0) == 0x8040; }

    /// The 60-pin contacts of an LH5803 cycle (PC1600BusDrive::lh5803Pins).
    SystemBusPins busPins(uint16_t addr, bool forWrite, bool me1) const {
        return PC1600BusDrive::lh5803Pins(addr, forWrite, me1, m_pu, m_pv);
    }

    /// The SC7852's LHA90 pin (Ref/PC-1600/PC-1600-CPU-SC7852-Z80.md,
    /// pin 38) is forced high while the LH5803 accesses 7400H-744FH or
    /// 7500H-754FH, so those land on 7600H-764FH / 7700H-774FH -- the
    /// PC-1500's display-RAM aliases. Confirmed on a real PC-1600: an XPOKE
    /// there reads back at 76xxH and draws on the LCD like one to 76xxH.
    static constexpr uint16_t lha90(uint16_t addr) {
        return ((addr & 0xFE00) == 0x7400 && (addr & 0xFF) < 0x50) ? uint16_t(addr | 0x0200) : addr;
    }
    /// An ME0 write to the PC-1500 display RAM (7600H-764FH) also goes to
    /// the LCD, as the gate array does: see PC1600Display::mirrorPc1500Column().
    /// ME1 writes there only reach the RAM.
    void mirrorPc1500Display(uint16_t addr);
    /// Offers an access to the cards on PC1600Memory::systemBus().
    /// True (with *value set, for a read) when a card claims it.
    bool cardRead(uint16_t addr, bool me1, uint8_t* value) const;
    bool cardWrite(uint16_t addr, bool me1, uint8_t value);

    PC1600Memory& m_shared;
    PC1600BusArbiter* m_arbiter{nullptr};
    LH5803Rom m_rom;
    bool m_pv{false};
    bool m_pu{false};
};
