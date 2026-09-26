#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../Audio/PiezoSampler.hpp"
#include "../CPU/SC7852/SC7852.hpp"
#include "../Connector/ExpansionCard.hpp"
#include "../Connector/MemorySlotConnector.hpp"
#include "../Connector/PC1600SystemBus.hpp"
#include "PC1600Bank.hpp"
#include "PC1600BusArbiter.hpp"
#include "PC1600Display.hpp"
#include "PC1600SubCpu.hpp"
#include "PC1600Keyboard.hpp"
#include "TC8576F.hpp"

// ── PC-1600 (SC7852/Z-80 side) address decoder ───────────────────────────
//
// Resolves each of the SC7852's four 16KB pages against a PC1600Bank's
// current register state, per the "Memory Map by Bank" table in
// SharpPC1500Reference/PC-1600/PC-1600-Memory-Bank-Switching.md.
//
// Pages (Z-80 address space):
//   Page A  0000-3FFF  system ROM, CS001 — always resident, regardless of
//                      Port 31H bit 0 (only bank 0's content is loaded;
//                      a hypothetical bank 1 here is undocumented).
//                      SLOT2MAP mode 2 CAN still swap this ROM out live, via
//                      slot2MapTarget()'s page-A branch (checked before this
//                      class's own resolveConst()) — "always" above means
//                      "under the plain, non-remapped decode," not "no
//                      register can ever override it." (EmulatorViewModel's
//                      debug bank-grid page-A bank-1 CELL prints this plain
//                      meaning unconditionally, since it can't show "would
//                      redirect if selected" on a bank that isn't currently
//                      selected — see its own comment for the same gotcha.)
//   Page B  4000-7FFF  bank-selected (Port 31H bits 1-3): bank 0 = system
//                      ROM upper half (same physical ROM device as page A,
//                      loaded together — see loadBank0()); bank 3 = normal
//                      Bank 3 ROM, or the hidden Bank 3b BASIC ROM in its
//                      place when Port 3DH bit 2 is clear (see
//                      PC1600Bank::hiddenBasicRomSelected()) — modeled as a
//                      whole-16KB swap between the two images; the
//                      documented CS24 8KB-sub-banking detail (A16A
//                      splitting Bank 3's window into two independent 8KB
//                      halves) isn't separately modeled since both ROM
//                      images load as flat 16KB blocks with no internal
//                      structure this decoder needs to reason about.
//                      Banks 1/2 are genuinely open bus, not merely
//                      unresearched: PC-1600-Memory-Bank-Switching.md's
//                      "Slot 2 ROM"/"Slot 1 ROM" labels for this table cell
//                      are a loose paraphrase, corrected at schematic level
//                      (Part 4/12, and the CE-1600M/CE-1620M module
//                      schematics) — the whole 4000-7FFF window is decoded
//                      by CS24 alone, wired only to the internal Memory-PWB
//                      ROM; neither memory-slot connector's pin 4 (RAMSN)
//                      is ever asserted outside 8000-BFFF, so no card in
//                      Slot 1/2 can ever answer here. Bank
//                      6 has no documented content in this window at all
//                      (as opposed to Page C's bank 6, a different bank
//                      register value entirely — see below). Banks 4/5
//                      (CE-1600P plotter/floppy ROM) are real, documented
//                      peripheral content, deliberately left open bus until
//                      CE-1600P itself is emulated.
//   Page C  8000-BFFF  bank-selected (Port 31H bits 4-6): bank 6 = display/
//                      timer/serial/char-table ROM (CS123). Banks 0/1
//                      (Slot 1) and 2/3 (Slot 2, whose vertical bank the
//                      module further selects off Port 28H) route through
//                      the m_slot1Conn/m_slot2Conn MemorySlotConnectors to
//                      whatever ExpansionCard is attached — open bus only
//                      when the slot is empty. Bank 7 is dual-source
//                      confirmed (TRM §2.1 and
//                      the PC-1600's German user manual, independently) as
//                      genuinely unused hardware-wide, not merely
//                      undocumented — open bus permanently.
//                      (Page C has no separate banks 4/5 of its own;
//                      those bank numbers belong to Page B, above.)
//   Page D  C000-FFFF  bank-selected (Port 31H bit 7): bank 0 = fixed
//                      internal 16KB RAM (RAM3). Bank 1's target is
//                      undocumented in the TRM, Service Manual, and
//                      German user manual — left open bus until a
//                      primary source confirms what it maps to.
//
// System address F07DH (within page D bank 0's RAM window) is a read-side
// mirror of Port 3DH's last-written value (Port 3DH itself isn't readable
// via IN) — handled as a special case in read(), not stored separately.
//
// The gateway routines that make bank switching software-transparent all
// live in the always-resident page A ROM, so they need no special-casing
// here — their addresses are recorded as constants purely so a ROM
// sanity check can confirm they're actually present where documented.
//
// Also implements SC7852Bus so this one object can drive a standalone
// SC7852 core for boot testing -- readMem/writeMem delegate straight to
// read()/write() above; readIO/writeIO decode a subset of the 0x00-0xBF
// I/O space: Port 31H/28H/3DH (forwarded to PC1600Bank, same registers
// the memory decode above already reads), Port 32H/35H (interrupt
// cause/mask, stored but not wired to any real interrupt source), Port
// 39H (IM2 vector low byte), and Port 38H (CPU-switch trigger, forwarded
// to a PC1600BusArbiter via setBusArbiter() or silently ignored if none
// is attached, e.g. standalone single-CPU tests). Also wired: Port
// 1CH-1FH (LH5810-style DDA/DDB/OPA/OPB, keyboard strobes -- see
// PC1600Keyboard), Port 1BH (IF register, ON/BREAK latch), Port 37H (read:
// keyboard sense via PC1600Keyboard::scan(); write: bit4 gates the LCD's
// CK0 clock, see PC1600Display), Port 18H (OPC -- buzzer drive, see
// m_opc), and Port 50H-5BH (forwarded to PC1600Display). Every other port reads open bus (0xFF) and ignores
// writes -- this class has no opinion on the rest of the I/O map (UART,
// timer/RTC, etc. all remain out of scope).
class PC1600Memory : public SC7852Bus {
public:
    // Firmware gateway routines (always in page A, Bank 0) —
    // PC-1600-Memory-Bank-Switching.md Part 5.
    static constexpr uint16_t kMemoryChk = 0x018D;
    static constexpr uint16_t kBankSet   = 0x0190;
    static constexpr uint16_t kSlot1Map  = 0x0196;
    static constexpr uint16_t kSlot2Map  = 0x0199;
    static constexpr uint16_t kBankRead  = 0x0193;
    static constexpr uint16_t kBankJump  = 0x019C;
    static constexpr uint16_t kBankCall  = 0x019F;
    static constexpr uint16_t kSlotSt    = 0x00E8;

    // Read-side mirror of Port 3DH (see class comment).
    static constexpr uint16_t kPort3DMirrorAddr = 0xF07D;

    static constexpr size_t kBankSize = 0x4000; // 16384B, one Z-80 "bank"

    explicit PC1600Memory(PC1600Bank& bank)
        : m_bank(bank),
          m_slot1Conn(MemorySlotConnector::Slot::Slot1, bank),
          m_slot2Conn(MemorySlotConnector::Slot::Slot2, bank) {}

    /// Wires Port 38H writes to the given arbiter. Defaults to nullptr --
    /// a Port 38H write is silently ignored when unset, which is what
    /// standalone single-CPU use (e.g. sc7852_tests.cpp) relies on.
    void setBusArbiter(PC1600BusArbiter* arbiter) { m_arbiter = arbiter; }

    /// Wires Port 39H writes through to the CPU's own IM2 vector-byte
    /// latch -- without this, a real ROM write to 39H only updated this
    /// class's own copy
    /// (`m_im2VectorLow`, kept for readback/tests) and the SC7852's IM2
    /// dispatch used its unrelated, never-written default, so any genuine
    /// interrupt would have vectored through garbage. Defaults to nullptr,
    /// same no-op-when-unset convention as setBusArbiter().
    void setCPU(SC7852* cpu) {
        m_cpu = cpu;
        // The TC8576F's INT output (a level, masked per source by its own
        // pr[5]) reaches the SC-7852 on INT0 = interrupt-cause bit 0. It is
        // not latched: bit 0 follows the chip until the handler services
        // the chip (e.g. reads RxD). Port 35H bit 0 gates it in
        // updateIntLine().
        m_uart.setInterruptHook([this](bool) { updateIntLine(); });
        // The sub-CPU's Z7 reaches INT6 = cause bit 6, also a level: it
        // drops when the handler reads SRIRQ (PC1600SubCpu).
        m_subCpu.setInterruptHook([this] { updateIntLine(); });
        updateIntLine();
    }

    /// Raw port 35H value -- debug/test access, same convention as
    /// PC1500Machine's own cpu()/memory() unlocked accessors.
    uint8_t intMask() const { return m_intMask; }

    PC1600Keyboard&       keyboard() { return m_keyboard; }
    const PC1600Keyboard& keyboard() const { return m_keyboard; }
    PC1600Display&        display() { return m_display; }
    const PC1600Display&  display() const { return m_display; }
    PC1600SubCpu&         subCpu() { return m_subCpu; }
    const PC1600SubCpu&   subCpu() const { return m_subCpu; }
    TC8576F&             uart() { return m_uart; }
    const TC8576F&       uart() const { return m_uart; }
    /// Buzzer drive line as PCM, in SC-7852 T-states -- advanced by
    /// PC1600Machine::step() on both CPUs' branches (via advanceBuzzer()).
    /// See m_opc and m_fReg.
    PiezoSampler&        piezo() { return m_piezo; }
    /// Credits `tstates` of elapsed time to the buzzer: runs the F-register
    /// modulator (toggling SDO at its exact edge times) and the sampler.
    void advanceBuzzer(uint32_t tstates);

    /// Sets/clears the ON key's live state (not part of the scan matrix --
    /// see PC1600Keyboard's class comment). A press transition sets IF
    /// register (port 1BH) bit 1, per
    /// PC-1600-Keyboard.md §8 -- confirmed distinct from the PC-1500's own
    /// ON-key wiring in bit position only, same latch-on-press-edge idea.
    /// Returns true on a press (rising) edge -- the caller
    /// (PC1600Machine::setOnKeyPressed) uses that to resume whichever CPU
    /// owns the bus from its HALT (the ROM's power-down park). ON is not a
    /// port-32H interrupt cause.
    ///
    /// The key is also PB7's live level: Baum, PC-1600 Systemhandbuch
    /// (ISBN 3-924327-31-9) p.92 ("INP &1A AND &20 oder INP &1F AND &80")
    /// and Anhang A pp.93-94 (1 = pressed). This matches the
    /// PC-1500 TRM p.71's MSK read layout: CL1, SD1, PB7, IRQ in bits 7-4.
    /// The ROM polls it directly: wait-for-release at P0-B0 0775H, the
    /// gates at 0B07H/0B66H and P1-B3 4770H, and the key scan at
    /// P2-B6 9412H.
    bool setOnKeyPressed(bool pressed) {
        const bool risingEdge = pressed && !(m_pbIn & kPbInOnKey);
        if (risingEdge) m_if |= 0x02;
        if (pressed) m_pbIn |= kPbInOnKey;
        else         m_pbIn &= static_cast<uint8_t>(~kPbInOnKey);
        return risingEdge;
    }

    /// Reflects the sub-CPU's free-running 1/64s timer pulse onto PB5 (a
    /// live GPIO input pin, port 1FH/OPB bit 5) -- per Systemhandbuch
    /// §7.3/§7.4 a plain 64 Hz, 50%-duty square wave on the LU-57813P's
    /// Z6 pin, so this is just "what the level currently is," called
    /// every PC1600Machine::step() from an SC7852-T-state accumulator,
    /// not latched here. The boot ROM's idle loop polls PB5 via
    /// `IN A,(1FH)` directly.
    ///
    /// **Does NOT also set port 32H's cause-register bit 4.** A raw level
    /// and a latched "this interrupt is pending" flag are two different
    /// kinds of signal: INT4/PB5 genuinely is a live pin (per the TRM
    /// excerpt above), but the real timer ISR reads port 32H immediately
    /// after being woken by this falling edge and expects to find bit 4
    /// already clear there -- the *aggregated cause register* has to
    /// latch "an event happened" independently of whatever the raw pin is
    /// doing by the time software gets around to reading it. See
    /// `latchTimer64InterruptCause()`, called separately on the falling
    /// edge only, and `readIO()`'s own read-clears-cause convention.
    /// Stored in `m_pbIn` (the live PB *pin* levels), NOT in `m_opb` (the
    /// output latch): PB5 is an input pin, so per §2.4 of
    /// PC-1600-IO-Ports.md a read of 1FH must return its pin level, not
    /// whatever the CPU last wrote to the port. Keeping the two apart
    /// means a plain `OUT (1FH),A` cannot forge an input pin's level, even
    /// though the ROM only ever touches OPB through a read-modify-write
    /// (`PC1600-P1-B3-new.bin` 4887H/4896H) that happens to preserve bit 5
    /// anyway, so a live headless trace shows the same duty cycle and edge
    /// count with or without the split (measured over 21.6M T-states:
    /// 50.78% high, 771 edges vs. 772 expected for 64 Hz).
    ///
    /// PB5 reads low in the vast majority of `IN A,(1FH)` samples (1 high
    /// in 2387 reads in one trace) because the ROM's key scan runs inside
    /// the 1/64s timer ISR, which is dispatched on this signal's *falling*
    /// edge -- so every read it performs is necessarily taken while PB5 is
    /// low. This is expected sampling bias, not a stuck pin.
    void setTimer64Bit(bool active) {
        if (active) m_pbIn |= kPbInFreeRunning;
        else        m_pbIn &= static_cast<uint8_t>(~kPbInFreeRunning);
    }

    /// Latches interrupt-cause register (port 32H) bit 4 -- called once,
    /// on the timer's falling edge only (see `PC1600Machine::step()`),
    /// distinct from `setTimer64Bit()`'s raw-level PB5 update above. A
    /// read of port 32H (`readIO()`) clears it, matching the observed
    /// real-ROM access pattern: the timer ISR's own dispatcher
    /// (`PC1600-P1-B3-new.bin` 0x40FF `IN A,(32H)`) captures the cause byte once
    /// into a register and only ever reads that port again on a
    /// subsequent, later interrupt -- never re-reads it mid-dispatch, and
    /// never writes it -- so "read clears" is the only access pattern
    /// this project's own trace evidence can distinguish from "write
    /// clears" or "cleared some other way," and is the simplest
    /// convention consistent with it.
    void latchTimer64InterruptCause() { m_intCause |= 0x10; updateIntLine(); }


    /// Latches cause bit 3, "interrupt from the LH-5801/5803 side": the
    /// LH5803's STA #(0A038H) handback. The ROM's only handoff (P1-B3
    /// 5C0E-5C22) unmasks just this cause (35H = 08H) before EI;HALT, and
    /// the dispatcher's bit-3 branch (4154H) acknowledges it -- so this
    /// INT is what ends the parked SC7852's HALT.
    void latchLh5803InterruptCause() { m_intCause |= 0x08; updateIntLine(); }

    /// Port 32H as read: the latched causes plus two live levels, bit 0
    /// the TC8576F's INT output (INT0, pin 81) and bit 6 the sub-CPU's Z7
    /// (INT6, pin 84).
    uint8_t intCause() const {
        return static_cast<uint8_t>(m_intCause | (m_uart.interruptOutput() ? 0x01 : 0x00) |
                                    (m_subCpu.interruptRequest() ? 0x40 : 0x00));
    }

    /// The SC-7852's INT line is the OR of the latched causes (port 32H)
    /// that are enabled at port 35H -- a level, so masking a cause or the
    /// 32H read that clears it withdraws a request not yet taken.
    void updateIntLine() { if (m_cpu) m_cpu->setIntLine((intCause() & m_intMask) != 0); }

    /// Loads the always-resident system ROM: `lower` backs page A
    /// (0000-3FFF, PC1600-P0-B0-new.bin) and `upper` backs page B bank 0
    /// (4000-7FFF, PC1600-P1-B0-new.bin) — the same physical ROM device, loaded as
    /// two 16KB halves since that's how the source images are split.
    /// Returns false (untouched) if either size isn't exactly kBankSize.
    bool loadBank0(const uint8_t* lower, size_t lowerSize,
                    const uint8_t* upper, size_t upperSize);

    /// Page B bank 3 (PC1600-P1-B3-new.bin) / hidden bank 3b (PC1600-P1-B3B-new.bin).
    bool loadBank3Rom(const uint8_t* data, size_t size);
    bool loadBank3bRom(const uint8_t* data, size_t size);
    /// Page C bank 6 (PC1600-P2-B6-new.bin, display/timer/serial/char tables).
    bool loadBank6Rom(const uint8_t* data, size_t size);

    /// The 60-pin system bus (Page B banks 4/5 ROM window + I/O ports
    /// 0x80-0x8F) -- CE-1600P plugs in here, not the two memory slots. See
    /// PC1600SystemBus.hpp for why this bus has its own pin model rather
    /// than reusing ExpansionCard::PinState.
    PC1600SystemBus& ce1600pBus() { return m_ce1600pBus; }

    /// Plugs a card into Slot 1 / Slot 2 -- the connector-level path, taking
    /// ownership of the card (mirrors PC1500Machine::attachExpansionCard).
    /// Any card (a SoftwareDefinedCard built from a .card.yaml definition)
    /// plugs in pin-for-pin; the MemorySlotConnector drives the PC-1600
    /// bay's pins.
    void attachSlot1Card(std::unique_ptr<ExpansionCard> card) {
        m_slot1Card = std::move(card);
        m_slot1Conn.attach(m_slot1Card.get());
    }
    void attachSlot2Card(std::unique_ptr<ExpansionCard> card) {
        m_slot2Card = std::move(card);
        m_slot2Conn.attach(m_slot2Card.get());
    }

    void detachSlot1() { m_slot1Conn.detach(); m_slot1Card.reset(); }
    void detachSlot2() { m_slot2Conn.detach(); m_slot2Card.reset(); }
    bool slot1Attached() const { return m_slot1Conn.attachedCard() != nullptr; }
    bool slot2Attached() const { return m_slot2Conn.attachedCard() != nullptr; }
    /// The module in Slot `slot` (1 or 2) by name (ExpansionCard::moduleName()),
    /// "" for an empty slot.
    std::string slotModuleName(int slot) const {
        const ExpansionCard* c = (slot == 1 ? m_slot1Conn : m_slot2Conn).attachedCard();
        return c ? c->moduleName() : std::string();
    }

    /// The bank the card in Slot 1 / Slot 2 currently exposes through its
    /// banked window (ExpansionCard::debugCurrentBank()); -1 when the slot
    /// is empty or the card is plain unbanked RAM. GUI debug dump only.
    int slot1CardBank() const { return m_slot1Conn.attachedCardBank(); }
    int slot2CardBank() const { return m_slot2Conn.attachedCardBank(); }

    /// The card's total bank count (-1 = empty slot or no bank concept),
    /// for the GUI debug resource-inventory's "N-way paging" label.
    int slot1CardBankCount() const { return m_slot1Conn.attachedCardBankCount(); }
    int slot2CardBankCount() const { return m_slot2Conn.attachedCardBankCount(); }

    /// The whole backing store of the card in Slot 1 / Slot 2
    /// (ExpansionCard::debugImage()), empty when the slot is empty. GUI
    /// debug "Dump Mem" contents view only.
    std::vector<uint8_t> slot1CardImage() const {
        auto* c = m_slot1Conn.attachedCard();
        return c ? c->debugImage() : std::vector<uint8_t>{};
    }
    std::vector<uint8_t> slot2CardImage() const {
        auto* c = m_slot2Conn.attachedCard();
        return c ? c->debugImage() : std::vector<uint8_t>{};
    }

    /// The Slot 1 / Slot 2 card's contentRevision(); 0 for an empty slot.
    uint64_t slot1CardRevision() const {
        auto* c = m_slot1Conn.attachedCard();
        return c ? c->contentRevision() : 0;
    }
    uint64_t slot2CardRevision() const {
        auto* c = m_slot2Conn.attachedCard();
        return c ? c->contentRevision() : 0;
    }

    /// Copy the fixed internal 16 KB RAM (page D bank 0) into `out` (which
    /// must hold kBankSize bytes) -- the live state, read directly with no
    /// bank-register games. GUI debug dump only.
    void debugCopyInternalRam(uint8_t* out) const {
        std::copy(m_internalRam.begin(), m_internalRam.end(), out);
    }

    /// Write counterparts of slot{1,2}CardImage() / debugCopyInternalRam():
    /// overwrite backing storage directly, bypassing the emulated bus and
    /// its bank-register / pin gating. For the fast BASIC loader, which
    /// scatters a tokenised program across module banks + internal RAM that
    /// the post-NEW0 bank state may not currently map for CPU writes. Each
    /// returns false, writing nothing, on an empty slot / out-of-range /
    /// non-writable-backing.
    bool slot1CardImageWrite(size_t off, const uint8_t* data, size_t n) {
        auto* c = m_slot1Conn.attachedCard();
        return c && c->debugImageWrite(off, data, n);
    }
    bool slot2CardImageWrite(size_t off, const uint8_t* data, size_t n) {
        auto* c = m_slot2Conn.attachedCard();
        return c && c->debugImageWrite(off, data, n);
    }
    bool debugWriteInternalRam(size_t off, const uint8_t* data, size_t n) {
        if (n == 0) return true;
        if (!data || off > m_internalRam.size() || n > m_internalRam.size() - off) return false;
        std::copy(data, data + n, m_internalRam.begin() + off);
        return true;
    }

    /// Simple reset (the PC-1600 manual's lighter of two reset levels):
    /// re-latch the reset-level port pins, but leave internal RAM -- the
    /// BASIC program, variables and the IOCS work area -- intact, so the
    /// boot ROM takes its warm-start path. Loaded ROM images and the
    /// sub-CPU (RTC/password, on standby power) are untouched either way.
    void reset();

    /// ALL RESET / power loss: wipe the 16 KB internal RAM. With the work
    /// area gone the boot ROM fails its RAM check and runs full cold
    /// init. Call before reset().
    void clearInternalRam() { m_internalRam.fill(0); }

    uint8_t read(uint16_t addr) const;
    void    write(uint16_t addr, uint8_t value) { writeImpl(addr, value, /*direct=*/false); }
    /// True if `addr` currently resolves to a genuinely writable region
    /// (internal RAM, or a card in Slot 1/2 that would claim a write here)
    /// -- for a caller (PC1600Machine::pokeMemory, GUI program loading)
    /// that wants to know whether a write will actually stick before/instead
    /// of silently no-op-ing like write() does.
    bool isWritable(uint16_t addr) const {
        const SlotRemap r = resolveSlotRemap(addr);
        if (r.hit1) {
            uint8_t ignored;
            if (m_slot1Conn.readRemapped(r.off1, r.pvoutHigh1, ignored)) return true;
        }
        if (r.hit2) {
            uint8_t ignored;
            if (m_slot2Conn.readRemapped(r.off2, r.pvoutHigh2, ignored)) return true;
        }
        if (isRom(addr)) return false;
        if (resolveConst(addr) != nullptr) return true;
        if (!isSlotWindow(addr)) return false;
        uint8_t ignored;
        return m_slot1Conn.read(addr, ignored) || m_slot2Conn.read(addr, ignored);
    }

    // Debug/test access, identical semantics to read()/write() — kept as a
    // separate name for symmetry with PC1500Memory's peek()/poke() and to
    // make call sites' intent explicit. poke() takes the host/debug path
    // (PinState::direct) into a lock-gating card, and returns whether the
    // byte was stored (false: ROM, open bus, or a card that claimed the
    // write but dropped it).
    uint8_t peek(uint16_t addr) const { return read(addr); }
    bool    poke(uint16_t addr, uint8_t value) { return writeImpl(addr, value, /*direct=*/true); }

    // SC7852Bus
    uint8_t readMem(uint16_t addr) override { return read(addr); }
    void    writeMem(uint16_t addr, uint8_t value) override { write(addr, value); }
    uint8_t readIO(uint8_t port) override;
    void    writeIO(uint8_t port, uint8_t value) override;
#ifdef PC1600_POWER_PROBE
    // readIO() splits into a hookable wrapper + this impl only under the
    // throw-away power/UART probe build (tools/build_pc1600_power_probe.sh);
    // the shipping build has the one plain readIO().
    uint8_t readIOImpl(uint8_t port);
#endif

private:
    PC1600Bank& m_bank;
    // The two 40-pin memory-slot connectors (page C, 8000-BFFF). Owned here
    // rather than in PC1600Machine so a standalone PC1600Memory (tests,
    // pc1600_cli) and the LH5803 side (LH5803SharedMemory forwards +0x8000
    // into this same object) both go through them with no extra wiring.
    MemorySlotConnector m_slot1Conn;
    MemorySlotConnector m_slot2Conn;
    std::unique_ptr<ExpansionCard> m_slot1Card; // null = slot empty
    std::unique_ptr<ExpansionCard> m_slot2Card;
    PC1600SystemBus m_ce1600pBus; // Page B banks 4/5 + I/O 0x80-0x8F; see ce1600pBus()
    PC1600BusArbiter* m_arbiter{nullptr};
    SC7852* m_cpu{nullptr};
    uint8_t m_intCause{0};      // Port 32H latched causes, whatever the mask -- bit 3 LH5803
                                // handback, bit 4 1/64 s timer; the rest have no
                                // source yet. Read-clears.
                                // Bits 0 (comm) and 6 (sub-CPU) are live levels, see intCause().
                                // INT = intCause() & mask (updateIntLine())
    uint8_t m_intMask{0};       // Port 35H
    uint8_t m_im2VectorLow{0xFF}; // Port 39H

    // Keyboard/display I/O. LH5810-style port block:
    // DDA(1CH)/DDB(1DH) direction registers (bit i: 1 = output, 0 = input).
    // The KS0-7 strobes on OPA are treated as simply "active when OPA bit
    // reads 0" -- DDA not consulted, the same simplification PC1500Memory's
    // own comment flags (PC-1500 KEYSCAN_NOWAIT DDA nuance not mattering
    // here). The PB6 strobe (CTRL/KBII/BS) *does* consult DDB.6: it is
    // asserted only while PB6 is an output driven low -- see readIO(0x37).
    uint8_t m_dda{0}, m_opa{0}, m_ddb{0}, m_opb{0};
    // OPC (18H), the PC-port output buffer. The BEEP loop (P1-B3 5EC5)
    // toggles bit 7 with `IN A,(18H)` / OR 80H or AND 7FH / `OUT (18H),A`,
    // and bit 6 gates the buzzer: the firmware clears it for BEEP OFF
    // (5EFE, F86B bit 0) and sets it again for BEEP ON (5F31, OR C0H). This
    // matches Systemhandbuch Appendix 6 ("b7 buzzer line, b6 buzzer on").
    // Per TRM 7.5 the same line also carries cassette-record output (SD0),
    // so a future cassette model shares this latch. It has to be readable
    // because the ROM does read-modify-write on it.
    uint8_t m_opc{0};
    // F register (17H) and the modulated serial output SDO -- PC-1500 TRM
    // 3-3-2 (9) and 3-2 D for the LH5810/5811 this block is compatible
    // with. F6 = 1 switches SDO from normal serial data to the modulation
    // clocks: SDO = SXO*FX + /SXO*FY. F0-2 pick FX and F3-5 pick FY, each
    // phi/64, /128, /256, /512 or /1024. Only the idle case is modelled:
    // no serial transmit (L, 16H), so SXO sits at mark = 1 and SDO = FX.
    //
    // phi, measured: dampflok.bas (Baum Systemhandbuch p.52) whistles with F = 41H
    // (FX = /128). A real unit plays it at 2539 Hz (2533.24 Hz recorded, less
    // the recorder's -0.22% seen in every BEEP recording), = 1.3 MHz / 512.
    // So this block's modulator runs from phi = 1.3 MHz / 4 = 325 kHz.
    // Baum Systemhandbuch Anhang A p.93 confirms &17: "OUT &17,65" on /
    // "OUT &17,0" off, a continuous tone, the cassette-recording sync signal.
    // Its "2639 Hz" is a typo: no power-of-two divider of 1.3 or 3.58 MHz
    // gives that, while 2539 Hz is 1.3 MHz / 512. (The
    // PC-1500's own LH5811 runs at 1.3 MHz: the CE-150 tape code writes
    // F = 63H for its 2539 / 1270 Hz tones, /512 and /1024.)
    //
    // SDO reaches the buzzer through the same gate as OPC b7/b6. The line
    // idles high and either input going low sounds it
    // (PC-1600-CPU-SC7852-Z80.md pin 75: PC6 = NAND(..., SD0)). So the
    // audible level is (b6 && b7) && SDO. The recording agrees: the
    // whistle runs on unchanged through the noise routine's OPC writes, and
    // BEEP OFF (b6 low) silences both.
    static constexpr int64_t kModulatorHz = kPC1600PhiOsHz / 4;   // phi of the F-register dividers
    uint8_t m_fReg{0};
    bool    m_sdo{true};
    int64_t m_sdoAccum{0};  // T-states * kModulatorHz into the current SDO half period
    void updateBuzzerLine() { m_piezo.setLevel((m_opc & 0xC0) == 0xC0 && m_sdo); }
    // Sampled in SC-7852 T-states.
    PiezoSampler m_piezo{double(kPC1600TStateHz), PiezoSampler::Transducer::PC1600};
    // Live PB *pin* levels for the bits driven from outside the CPU, kept
    // apart from the m_opb output latch above and merged in on a read of
    // 1FH (see readIO()). PB5 = the sub-CPU's 64Hz timer square wave
    // (setTimer64Bit()); PB7 = the ON key (setOnKeyPressed(), which also
    // sets the 1BH interrupt-flag latch on the press edge).
    ///
    /// **PB3 starts high.** Pin 78 (PCSTB) is "reset → input mode, current
    /// state latched in the PB3 flip-flop (externally pulled up on the
    /// PC-1600) ... not used on the production PC-1600"
    /// (PC-1600-CPU-SC7852-Z80.md §6). Nothing ever drives it low on a
    /// production machine, so the flip-flop latches the pull-up at reset
    /// and PB3 reads 1 forever after.
    ///
    /// This is not cosmetic: the boot ROM copies exactly this bit into the
    /// alternate-charset enable flag. `PC1600-P0-B0-new.bin` 0512H does
    /// `LD HL,F1BCH / LD A,(HL) / AND 7FH / LD B,A / IN A,(1FH) /
    /// AND 08H / JR Z,+2 / SET 7,B / LD (HL),B` -- F1BCH bit 7 is PB3,
    /// latched once at startup. The KBII key handler (`PC1600-P1-B0-new.bin` 6CC1H)
    /// then gates its whole toggle on that bit: `LD A,(F1BCH) / RLA /
    /// JR NC,...` skips the `LD A,L / XOR 80H / LD L,A` at 6CCBH that
    /// flips the KBII flag. With PB3 low, KBII would be inert -- the key
    /// scans, decodes and dispatches perfectly and then does nothing. Key
    /// code 02H's handler (6C7CH) is gated on the same bit.
    static constexpr uint8_t kPbInResetLevels = 0x08; // PB3
    /// PB input pins driven by a source that free-runs across a reset, so
    /// their level is carried over rather than re-latched: today just PB5's
    /// 64Hz square wave (setTimer64Bit()). Named so reset() and the setter
    /// agree -- wiring a second such pin means editing one constant, not
    /// two bare literals in different functions.
    static constexpr uint8_t kPbInFreeRunning = 0x20; // PB5
    /// PB7: the ON key's live level. Also carried across reset(): the key
    /// is a physical input, not a reset-latched line.
    static constexpr uint8_t kPbInOnKey = 0x80;
    uint8_t m_pbIn{kPbInResetLevels};
    // MSK (1AH) -- interrupt mask bits 0-3 (IRQ, PB7, RD, TD enables; PC-1500
    // TRM p.71). Stored only: nothing in this core raises those causes.
    uint8_t m_msk{0};
    uint8_t m_if{0}; // port 1BH -- bit1 = ON/BREAK latch (PC1600Keyboard's own doc §8)
    PC1600Keyboard m_keyboard;
    PC1600Display m_display;
    PC1600SubCpu  m_subCpu;
    TC8576F       m_uart{m_subCpu}; // declared after m_subCpu -- it holds a ref

    std::array<uint8_t, kBankSize> m_bank0Lower{};   // page A, fixed
    std::array<uint8_t, kBankSize> m_bank0Upper{};   // page B bank 0
    std::array<uint8_t, kBankSize> m_bank3Rom{};     // page B bank 3
    std::array<uint8_t, kBankSize> m_bank3bRom{};    // page B bank 3 (hidden)
    std::array<uint8_t, kBankSize> m_bank6Rom{};     // page C bank 6
    std::array<uint8_t, kBankSize> m_internalRam{};  // page D bank 0

    bool m_bank0Loaded{false};
    bool m_bank3Loaded{false};
    bool m_bank3bLoaded{false};
    bool m_bank6Loaded{false};

    /// Returns a pointer into a locally-backed region (ROM images, internal
    /// RAM) for `addr` given the bank registers' current state, or nullptr
    /// for open-bus/unbacked addresses AND for the memory slots, which are
    /// card-backed and resolved through the connectors instead. Used by
    /// read() and isWritable().
    const uint8_t* resolveConst(uint16_t addr) const;
    /// The writable subset of the above: today only page D's internal RAM.
    /// Slot writes do NOT come through here -- writeImpl() hands those to
    /// the connectors -- so a future locally-backed writable region goes
    /// here, and a future card-backed one goes in the connector path.
    uint8_t* resolveMutable(uint16_t addr);
    bool isRom(uint16_t addr) const;

    /// Page C (8000-BFFF) -- the only window either memory slot can ever be
    /// mapped into *by the ordinary Port 31H page-C decode*, so the only one
    /// worth consulting a connector for by default. Keeps the per-access
    /// read()/writeImpl() path off the connectors entirely for open-bus
    /// addresses everywhere else. (SLOT2MAP can additionally route bank-1
    /// accesses in page A/B/C to Slot 2 -- see slot2MapTarget().)
    static bool isSlotWindow(uint16_t addr) { return addr >= 0x8000 && addr < 0xC000; }

    /// SLOT2MAP gate-array remap (Port 3CH b5:b4, PC1600Bank::slot2MapMode):
    /// the firmware can make the Slot 2 RAM chip-select assert for a bank-1
    /// access *outside* the normal page-C window -- the "(S2:) at Bank 1"
    /// path (PC-1600-Memory-Bank-Switching.md Part 1; SLOT2MAP ROM routine
    /// PC1600-P0-B0-new.bin 0A6DH). Modelled as an effective-address rewrite feeding
    /// the ordinary Slot 2 decode. Returns true when `addr` is currently so
    /// remapped, filling `*pvoutHigh` (false/true = low/high 16 KB half of
    /// the module's selected vertical bank) and `*offset` (0..0x3FFF within
    /// that half). The Port 28H vertical-bank latch is the card's own state
    /// and is not touched here.
    bool slot2MapTarget(uint16_t addr, bool* pvoutHigh, uint16_t* offset) const;

    /// SLOT1MAP gate-array remap (Port 3CH b2, PC1600Bank::slot1MapActive):
    /// when active, Slot 1's high 16KB half (beta) ALSO answers at page-B
    /// bank 1 (4000-7FFF), a mirror onto its normal page-C bank-1 home, not
    /// a move (TRM 0196H entry + diagram). Same effective-address-rewrite
    /// shape as slot2MapTarget() above, targeting m_slot1Conn instead. This
    /// window (page-B bank 1) is the one place SLOT1MAP and SLOT2MAP mode 2
    /// can collide -- see PC1600Bank::resolveSlotCollision().
    bool slot1MapTarget(uint16_t addr, bool* pvoutHigh, uint16_t* offset) const;

    /// Combines slot1MapTarget()/slot2MapTarget()/PC1600Bank::resolveSlotCollision()
    /// into the one call read(), writeImpl() and isWritable() all need before
    /// deciding whether a slot connector claims the access.
    struct SlotRemap {
        bool hit1 = false, hit2 = false;
        uint16_t off1 = 0, off2 = 0;
        bool pvoutHigh1 = false, pvoutHigh2 = false;
    };
    SlotRemap resolveSlotRemap(uint16_t addr) const {
        SlotRemap r;
        r.hit1 = slot1MapTarget(addr, &r.pvoutHigh1, &r.off1);
        r.hit2 = slot2MapTarget(addr, &r.pvoutHigh2, &r.off2);
        m_bank.resolveSlotCollision(&r.hit1, &r.hit2);
        return r;
    }

    // Shared body of write()/poke(): internal RAM first, then the two slot
    // connectors (`direct` distinguishes a host poke from a guest store).
    // Returns whether the byte was stored.
    bool writeImpl(uint16_t addr, uint8_t value, bool direct);
};
