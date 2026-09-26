#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "../Audio/PiezoSampler.hpp"
#include "../CPU/LH5801/LH5801.hpp"
#include "../Connector/ExpansionConnector.hpp"
#include "../Connector/SystemBus.hpp"
#include "PC1500Keyboard.hpp"
#include "PC1500Variant.hpp"
#include "Upd1990ac.hpp"

// ── PC-1500 / PC-1500A physical memory map ───────────────────────────────
//
// Implements LH5801Bus for both models. Address decoding follows the
// documented memory map; the two models share the same top-level layout
// and differ only in which S-blocks are
// backed by RAM and how S7 decodes (see PC1500Variant.hpp):
//
//   0x0000–0x3FFF  Y0   optional user memory (module slot) — open bus, no module
//   0x4000–0x47FF  S0   standard user RAM, 2KB (built-in on both models)
//   0x4800–0x57FF  S1-2 PC-1500A: built-in RAM (6KB total with S0).
//                       PC-1500: open bus (module slot).
//   0x5800–0x6FFF  S3-5 optional user memory (module slot) — open bus
//   0x7000–0x77FF  S6   display-driver RAM, 512B, mirrored 4× across the
//                       2KB window (AD9/AD10 unused by the V2/V3 sub-decode
//                       — see the doc's "V2/V3 mirroring" appendix)
//   0x7800–0x7FFF  S7   PC-1500A: independent 2KB system RAM (no aliasing —
//                       the "machine language area" at &7C01-&7FFF lives
//                       here). PC-1500: only 1KB physically present (a
//                       TC5514 pair decoding just A0-A9), so &7C00-&7FFF
//                       aliases &7800-&7BFF at a fixed &400 offset.
//   0x8000–0xBFFF  Y2   optional ROM area (CE-150/153/158) — open bus, no module
//   0xC000–0xFFFF  Y3   system ROM (16KB — PC-1500A: A04 only; PC-1500: A01/A03/A04)
//
// ME1 (the LH5801's second bank, accessed via `#(addr)` forms) hosts the
// LH5811 I/O-port controller, confirmed by live hardware testing on a
// real PC-1500. The chip is selected by
// any ME1 address with bits 12-13 both set (confirmed on real hardware
// that bits 4-11 and 14-15 don't matter — 0xF000-0xF00F is just the
// conventional address the ROM uses, not the only one that works); the
// low 4 bits select one of its registers (RS0-3 = AD0-3). Only the
// registers a stock PC-1500 actually needs are modeled — DDA/OPA (drives
// the keyboard's column strobe; keyboard *rows* are read directly off the
// CPU's IN0-IN7 pins via ITA, bypassing this chip entirely) and DDB/OPB
// (PB3's "must read high" ROM dispatch gotcha, PB7's ON-key readback), and
// the uPD1990AC real-time clock bit-banged via OPC/PC0-PC5 (see
// Upd1990ac.hpp) — WAIT/BEEP's timing depends on its TP output, latched
// into IF bit 1 (0xB). The buzzer is PC6 (OPC bit 6): the ROM's BEEP loop
// (A04 E655ff) toggles it directly, and every write is forwarded to
// m_piezo (see PiezoSampler.hpp) so the sound comes from that square wave
// itself. Serial transfer remains out of scope. Any ME1
// address outside the I/O-chip's decode window still mirrors ME0, the
// same conservative Phase 1 placeholder as before (nothing else is
// documented as living there) — flagged for revisit once Phase 4's
// ExpansionConnector work clarifies ME1's remaining role.
class PC1500Memory : public LH5801Bus {
public:
    explicit PC1500Memory(PC1500Variant variant = PC1500Variant::PC1500A);

    PC1500Variant variant() const { return m_variant; }

    /// Load a 16KB ROM image into 0xC000–0xFFFF. Returns false (image
    /// untouched) if `size` isn't exactly 16384 bytes.
    bool loadROM(const uint8_t* data, size_t size);
    bool loadROMFile(const std::string& path);

    void reset();     // chip-side reset (PIO, RTC, I/O); RAM, ROM and keyboard state untouched
    void clearRam();  // power-up / ALL RESET: user RAM 0x00, the &7600-&7BFF window 0xFF

    // LH5801Bus
    uint8_t readME0(uint16_t addr) override;
    void    writeME0(uint16_t addr, uint8_t value) override;
    uint8_t readME1(uint16_t addr) override;
    void    writeME1(uint16_t addr, uint8_t value) override;
    /// ITA — reads the keyboard matrix's row state for whatever column(s)
    /// OPA currently has strobed (see this file's top comment).
    uint8_t readInputPort() override;

    // Debug/test access: same address decode as readME0/writeME0 (open-bus
    // ranges read 0xFF, ROM ignores writes), just callable from a const
    // context (peek) or without implying bus semantics (poke). poke() also
    // bypasses a card's runtime write-gating (e.g. the CE-163F's flash-bank
    // JEDEC unlock protocol) -- it's the host/debug/preset-loader path, and
    // an unconditional write is the right semantics there. Returns whether
    // the byte was stored (false: ROM, open bus, or a card that claimed the
    // write but dropped it).
    uint8_t peek(uint16_t addr) const;
    bool    poke(uint16_t addr, uint8_t value);

    /// Debugger view of ME1 without bus side effects. The LH5810's
    /// registers come back as their latched values (OPB as written, IF
    /// without taking a pending RTC edge); a card's I/O window can't be
    /// read without disturbing it, so `*readable` is false there and 0xFF
    /// is returned. Everything else reads as readME1() would.
    uint8_t debugPeekME1(uint16_t addr, bool* readable) const;

    /// Whether the attached expansion card actually decodes `addr` (vs.
    /// this being genuine open bus) -- for the "Dump Mem" panel's
    /// per-region emptiness check: a card's own pin decode may not extend
    /// to every module-slot range it's electrically wired to (e.g. a
    /// CE-163F only answers in Y0, not S3-5). Same connector->read() call
    /// peek()'s open-bus fallback already makes, so no new side effects.
    bool debugSlotResponds(uint16_t addr) const;

    PC1500Keyboard&       keyboard() { return m_keyboard; }
    const PC1500Keyboard& keyboard() const { return m_keyboard; }

    /// Set/clear the ON key's live state, read back via OPB bit 7. Polarity
    /// (active-high vs. active-low) is not confirmed against real hardware
    /// -- see readME1()'s OPB case.
    ///
    /// A press transition also sets IF bit 1 (the same bit the uPD1990AC's
    /// TP output latches -- see Upd1990ac.hpp). This is necessary because
    /// BREAK (pressing ON while a program is running) is polled by the
    /// ROM's own statement-boundary check (LC42A) and WAIT's own poll loop
    /// (0xE89C) via exactly this bit; the MI interrupt handler (LE171,
    /// vector 0xFFF8) never sets bit 1 itself -- it only tests/clears bit
    /// 0 (serial receive-done, unrelated) before returning. Real hardware
    /// most plausibly has the ON key's own interrupt-request line OR-wired
    /// onto the same physical latch TP's rising edge sets, with the CPU's
    /// own polling code never distinguishing the two sources -- reproduced
    /// here as a direct write on the press transition, not routed through
    /// Upd1990ac (which models the RTC chip specifically, not this
    /// separate physical latch).
    ///
    /// Returns true on a press (rising) edge -- the caller (PC1500Machine)
    /// also needs this to unconditionally wake a halted CPU, the same way
    /// PC1600Memory::setOnKeyPressed() reports it to PC1600Machine.
    bool setOnKeyPressed(bool pressed) {
        bool risingEdge = pressed && !m_onKeyPressed;
        if (risingEdge) m_if |= 0x02;
        m_onKeyPressed = pressed;
        return risingEdge;
    }

    /// Advances the uPD1990AC RTC's TP clock by `cycles` CPU cycles' worth
    /// of real time. Called once per instruction from
    /// PC1500Machine::step() — see Upd1990ac::advance()'s own comment for
    /// why this Core drives it from cycles rather than real wall-clock
    /// time.
    void advanceRtc(uint32_t cycles) { m_rtc.advance(cycles); }

    /// The buzzer line (PC6) as PCM -- advanced alongside the RTC, i.e.
    /// once per instruction (and per halted/off tick) by PC1500Machine.
    void advancePiezo(uint32_t cycles) { m_piezo.advance(cycles); }
    PiezoSampler& piezo() { return m_piezo; }

    /// Seed the uPD1990AC's calendar from the host clock (see
    /// PC1500Machine::seedClock). month is 1-12, dow 0-6 (Sunday=0), the
    /// rest plain decimals. Deliberately kept out of reset() so the Core
    /// stays deterministic for headless tests -- the app seeds via this.
    void seedClock(int year, int month, int day, int hour, int minute, int second, int dow,
                   int millisecond = 0) {
        m_rtc.seedFromHost(year, month, day, hour, minute, second, dow, millisecond);
    }

    static constexpr uint16_t kDisplayRamBase = 0x7000;
    static constexpr size_t   kDisplayRamSize = 0x0200; // 512B backing store

    /// Direct copy of display RAM's backing store (the kDisplayRamSize
    /// bytes starting at kDisplayRamBase), bypassing peek()'s per-byte
    /// address-decode -- for PC1500Display's constructor, which otherwise
    /// has no way to get these bytes without re-deriving this class's own
    /// base/size/mirroring constants itself.
    std::array<uint8_t, kDisplayRamSize> displayRamSnapshot() const { return m_displayRam; }

    // ── Expansion connectors ──────────────────────────────────────────────
    //
    // The 40-pin (single-slot) and 60-pin (daisy-chain) connectors, built
    // for this memory's own variant. With no card attached they are pure
    // open bus. See Core/Connector/ExpansionConnector.hpp and SystemBus.hpp
    // for what these model; PC1500Machine forwards its accessors here.
    ExpansionConnector& expansionConnector() { return m_expansionConnector; }
    SystemBus&          systemBus() { return m_systemBus; }

    /// Live LH5801 PU/PV flip-flop state, pushed once per instruction by
    /// PC1500Machine::step()/runCycles() -- PC1500Memory has no CPU
    /// reference of its own (LH5801Bus's dependency direction runs the
    /// other way). Consulted by the expansion connectors' chip-select
    /// logic on the next bus access; PU/PV-setting instructions (SPU/RPU/
    /// SPV/RPV) never touch the bus themselves, so updating once per
    /// instruction (rather than mid-instruction) is sufficient.
    void updatePUPV(bool pu, bool pv) { m_pu = pu; m_pv = pv; }

private:
    static constexpr uint16_t kUserRamBase = 0x4000;
    static constexpr size_t   kUserRamSizeA     = 0x1800; // 6144B, S0+S1+S2 (PC-1500A)
    static constexpr size_t   kUserRamSizePlain = 0x0800; // 2048B, S0 only (plain PC-1500)
    static constexpr uint16_t kDisplayRamWindowSize = 0x0800; // 2KB window, mirrored
    static constexpr uint16_t kSystemRamBase = 0x7800;
    static constexpr uint16_t kSystemRamWindowSize = 0x0800; // 2KB CPU-visible window, both models
    static constexpr size_t   kSystemRamSize = 0x0800; // 2048B backing store (PC-1500A only uses it
                                                         // all; the plain PC-1500's TC5514 pair only
                                                         // decodes the first 1024B — see resolve())
    // &7800-&7BFF: the part of system RAM inside the measured power-up 0xFF
    // window (see clearRam()). Not derivable from m_systemRamAddrMask — the
    // window is these 1024B on both models, PC-1500A's wider mask included.
    static constexpr size_t   kSystemRamPowerUpFfSize = 0x0400;
    static constexpr uint16_t kRomBase = 0xC000;
    static constexpr size_t   kRomSize = 0x4000; // 16384B

    PC1500Variant m_variant;
    // Precomputed at construction (variant is fixed for the object's
    // lifetime -- see resolve()) so the hot read/write path doesn't
    // re-branch on m_variant every access.
    const size_t   m_userRamSize;
    const uint16_t m_systemRamAddrMask; // 0x7FF (PC-1500A, no aliasing) or 0x3FF (plain PC-1500)
    std::array<uint8_t, kUserRamSizeA>   m_userRam{};
    std::array<uint8_t, kDisplayRamSize> m_displayRam{};
    std::array<uint8_t, kSystemRamSize>  m_systemRam{};
    std::array<uint8_t, kRomSize>        m_rom{};

    // LH5811 I/O-port controller state (see this file's top comment) —
    // only the registers a stock PC-1500 actually exercises.
    PC1500Keyboard m_keyboard;
    uint8_t m_dda{0};   // DDA: PA direction (1=output). The "any key pressed?" check
                        // (KEYSCAN's initial poll) sets this to 0xFF and always drives
                        // OPA=0, strobing every column at once. But the real per-key
                        // identification routine (KEYSCAN_NOWAIT) scans one column at a
                        // time by varying *this* register instead -- it cycles DDA
                        // through 0x01, 0x02, 0x04, ... 0x80 (exactly one output-enabled
                        // pin per iteration) while leaving OPA fixed at 0x00 the whole
                        // time. A DDA bit of 0 makes that PA pin an input, which floats
                        // (not driven low), so it must NOT be treated as "strobed"
                        // regardless of OPA's bit value there -- see readInputPort().
    uint8_t m_opa{0};   // OPA: last-written PA value -- only meaningful for a PA bit
                        // that DDA has configured as an output (see m_dda above).
    uint8_t m_ddb{0};   // DDB: PB direction (unused by any modeled PB bit so far)
    uint8_t m_opb{0};   // OPB: last-written PB value (bits 3/5/6/7 are overridden on
                        // readback -- see readME1()'s OPB case)
    bool    m_onKeyPressed{false};
    uint8_t m_opc{0};   // OPC: last-written value (PC0-5 forwarded live to m_rtc on
                        // every write, see writeME1()'s OPC case; stored too so a
                        // readback sees what was last written, matching every other
                        // output-buffer register here)
    uint8_t m_if{0};    // IF: interrupt-flag register. Bit 1 (TP) is OR'd in from
                        // m_rtc on read, not written directly by the ROM -- but
                        // otherwise behaves as a plain read/write latch (the ROM
                        // clears bit 1 itself via `ani #(0xF00B),0xFD` when it's
                        // done with it, e.g. LC4C6/LC4ED), so writes still go
                        // straight through. Deliberately does NOT clear on read --
                        // see readME1()'s IF case for why that specific behavior
                        // (which looks like the "obvious" flag-register convention)
                        // is actually what breaks BREAK.
    Upd1990ac m_rtc;
    PiezoSampler m_piezo{static_cast<double>(kPC1500CpuHz), PiezoSampler::Transducer::PC1500}; // LH5801 clock; buzzer driven from OPC bit 6 (PC6)

    // F/G/MSK (registers 0x7,0x9,0xA) and the two unused register-select
    // codes (0x0-0x3): stored as plain read/write bytes, defaulting to
    // 0x00. This does NOT model the real hardware behind these registers
    // (serial transfer, and MSK's real interrupt-masking effect, are out
    // of scope — see this file's top comment) -- but a real ROM both
    // writes AND reads some of them (e.g. it zeroes MSK during boot), so
    // storing what's written and echoing it back is a strictly more
    // faithful default than a hardcoded constant that ignores writes
    // entirely.
    std::array<uint8_t, 16> m_ioScratchRegs{};

    // Expansion connectors -- see expansionConnector()/systemBus() above.
    ExpansionConnector m_expansionConnector;
    SystemBus          m_systemBus;
    bool m_pu{false};
    bool m_pv{false};

    // True if any card on either connector is pulling INHIBIT low --
    // suppresses the system ROM (see resolve()'s ROM branch). With nothing
    // attached, always false (today's unconditional ROM behavior).
    bool inhibitAsserted() const;

    static bool isIoChipAddress(uint16_t addr) { return (addr & 0x3000) == 0x3000; }

    // Single source of truth for the address decode, shared by
    // readME0/writeME0/peek/poke. Returns nullptr for an open-bus address,
    // or for a ROM address when `forWrite` (ROM ignores writes). Checked
    // ROM-first: instruction fetches (the hottest access this core makes)
    // are overwhelmingly from ROM.
    uint8_t*       resolve(uint16_t addr, bool forWrite);
    const uint8_t* resolve(uint16_t addr, bool forWrite) const;

    // Shared open-bus fallback for readME0/peek once resolve() has already
    // returned nullptr: give the expansion connector, then the system bus,
    // first refusal before defaulting to 0xFF.
    uint8_t readOpenBus(uint16_t addr) const;
};
