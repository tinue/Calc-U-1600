#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../Connector/CE1600FCard.hpp"
#include "../Connector/CE1600PCard.hpp"
#include "../Connector/ExpansionCard.hpp"
#include "../CPU/LH5803/LH5803.hpp"
#include "../CPU/LH5803/LH5803SharedMemory.hpp"
#include "../CPU/SC7852/SC7852.hpp"
#include "../PC1500/PC1500TraceFile.hpp"
#include "PC1600Bank.hpp"
#include "PC1600BusArbiter.hpp"
#include "PC1600Memory.hpp"

/// A genuine value-type snapshot of PC1600Display's pixel state, copied
/// once under PC1600Machine's lock -- unlike PC1500Display (itself already
/// a snapshot type constructed fresh from RAM bytes), PC1600Display is a
/// live, continuously-written HD61102 controller pair, so handing out a
/// reference to it would not be safe to read from a different thread while
/// the emulation loop is mid-step(). Row-major ([row][col]) to match
/// PCDisplaySnapshot's Bridge convention.
struct PC1600DisplaySnapshot {
    bool pixels[PC1600Display::kHeight][PC1600Display::kWidth]{};
    bool clockEnabled{false};
    bool statusSymbols[PC1600StatusLine::kCount]{}; // PC1600StatusLine::all()'s order
};

// ── PC-1600 dual-CPU machine facade ──────────────────────────────────────
//
// Owns both CPUs, the bus arbiter, and the bank/memory decoders -- does
// NOT reuse PC1500Machine: the PC-1600 is architecturally a dual-CPU,
// bank-switched machine with no PC-1500/1500A analog, and forcing a
// shared facade would drag that complexity into the simpler PC-1500A
// build.
//
// step() dispatches to whichever CPU PC1600BusArbiter says currently owns
// the bus; only one CPU ever executes per step() call, matching the real
// hardware's ELH#-gated single-owner bus. reset() always starts on SC7852
// (architecturally guaranteed, not configurable -- see
// PC-1600-Machine-Overview.md §6).
class PC1600Machine {
public:
    PC1600Machine();

    /// Simple reset -- the PC-1600 manual's lighter reset level: restart
    /// the CPUs but keep internal RAM (BASIC program, variables, IOCS work
    /// area), so the boot ROM takes its warm path and nothing is lost.
    /// Used on hardware to break out of a hung machine-language program.
    void reset();

    /// ALL RESET -- the deeper level: wipe internal RAM first, so the boot
    /// ROM runs full cold init, exactly as after a power loss. The cold
    /// init re-zeroes the calendar clock; re-seed it afterwards with
    /// seedClock().
    void allReset();

    /// Seed the sub-CPU's real-time clock. Values are plain decimal
    /// (year = full 4-digit); they are packed to the LU-57813P's BCD
    /// fields here, and the one-shot cold-start guard is armed so the very
    /// next boot's calendar init doesn't wipe the seed. The GUI/CLI calls
    /// this at startup (and after ALL RESET) from the host clock;
    /// thereafter the clock free-runs off emulated cycles (the 1 Hz
    /// accumulator in step()) and is never re-synced -- the same
    /// crystal-driven behavior the real chip has.
    void seedClock(int year, int month, int day, int hour, int minute, int second);
    /// Executes one instruction on whichever CPU currently owns the bus,
    /// completing a pending handoff first if one is due (see .cpp). Returns
    /// that CPU's own step() cycle count.
    int step();

    // ── Clock domains ────────────────────────────────────────────────────
    //
    // The two CPUs run off different crystals and their step() costs are
    // NOT interchangeable. `kTStateHz` is this machine's canonical unit of
    // emulated time: SC-7852 T-states at 3.58 MHz
    // (PC-1600-CPU-SC7852-Z80.md §2.1). `kLH5803Hz` is the LH-5803's basic
    // clock phi-OS on pin 4 -- a real PC-1600 number, but explicitly *not*
    // the Z-80 core clock; §2.1 warns about exactly this confusion, and
    // conflating the two runs the machine 2.75x too slow.
    //
    // Everything that measures emulated time derives from kTStateHz rather
    // than restating it: the timer periods below, runCycles()'s budget, and
    // (via `paceHz` on the Bridge wrapper) the GUI's batch pacing.
    static constexpr uint32_t kTStateHz = 3580000;
    static_assert(CE1600FCard::kTStateHz == kTStateHz, "CE1600FCard times seeks in SC7852 T-states");
    static constexpr uint32_t kLH5803Hz = 1300000;

    /// Converts a cycle count returned by whichever CPU owned the bus into
    /// this machine's canonical T-state unit. SC-7852 counts pass through;
    /// LH-5803 cycles are scaled by the clock ratio (rounded to nearest).
    static constexpr uint64_t toTStates(uint64_t cycles, bool sc7852Owned) {
        return sc7852Owned ? cycles
                           : (cycles * kTStateHz + kLH5803Hz / 2) / kLH5803Hz;
    }

    /// Runs until `maxCycles` **T-states** (see kTStateHz) have been
    /// consumed across both CPUs combined, returning the number actually
    /// consumed. A plain step-and-sum loop: unlike PC1500Machine's, no
    /// breakpoint or stuck-HALT bailout is needed here, because the bus
    /// arbiter always has the other CPU to dispatch to on the next call, so
    /// this loop cannot spin without making progress. A halted SC7852 step
    /// is charged SC7852::kHaltTickCycles, the same figure step() charges
    /// its own timer accumulators -- one function now owns both, so they
    /// can no longer drift apart.
    uint64_t runCycles(uint64_t maxCycles);

    /// Optional host callback invoked from inside runCycles() roughly every
    /// `intervalTStates` T-states of emulated time (counted across calls, so many
    /// short runCycles() calls still add up). Lets a UI thread that drives
    /// a long synchronous preset/program load keep its event loop alive
    /// (repaint, show a progress popup) without the loaders knowing about
    /// it. Called with m_mutex NOT held, so the hook may safely read the
    /// display snapshot. The hook must not drive the machine itself. An
    /// empty function (the default) removes it.
    void setYieldHook(std::function<void()> hook, uint64_t intervalTStates);

    // ── ROM loading ────────────────────────────────────────────────────
    bool loadBank0(const uint8_t* lower, size_t lowerSize, const uint8_t* upper, size_t upperSize) {
        return m_z80Mem.loadBank0(lower, lowerSize, upper, upperSize);
    }
    bool loadBank3Rom(const uint8_t* data, size_t size) { return m_z80Mem.loadBank3Rom(data, size); }
    bool loadBank3bRom(const uint8_t* data, size_t size) { return m_z80Mem.loadBank3bRom(data, size); }
    bool loadBank6Rom(const uint8_t* data, size_t size) { return m_z80Mem.loadBank6Rom(data, size); }
    bool loadLH5803Rom(const uint8_t* data, size_t size) { return m_lh5803Mem.loadROM(data, size); }
    bool loadLH5803RomFile(const std::string& path) { return m_lh5803Mem.loadROMFile(path); }

    // ── CE-1600P plotter (60-pin system bus) ─────────────────────────────
    //
    // `PC1600-P1-B4-CE1600P.bin`/`-2.bin` are confirmed CE-1600P ROM (see
    // roms/README.md) -- `attachCE1600P` builds a card, loads both halves,
    // and attaches it to `m_z80Mem.ce1600pBus()`; PC1600Memory routes Page B
    // banks 4/5 and I/O ports 0x70-0x8F to that bus once attached.
    //
    // The CE-1600F floppy docks onto the CE-1600P and cannot run
    // standalone (its driver lives in the CE-1600P's own bank-5 ROM), so
    // the two attach/detach as a union: `attachCE1600P` always also builds
    // a `CE1600FCard` (empty drive -- no disk) and chains it onto the same bus; `detachCE1600P` tears down
    // both together. There is no separate floppy attach/detach entry
    // point -- only disk *image* selection (`ce1600fLoadImage` etc.) is
    // independent of attach/detach.
    bool attachCE1600P(const uint8_t* rom1, size_t rom1Size,
                        const uint8_t* rom2, size_t rom2Size);
    void detachCE1600P();
    bool ce1600pAttached() const { return m_ce1600pCard != nullptr; }
    /// Unlocked direct access -- headless/tests only, same convention as
    /// `keyboard()`/`memory()`. The GUI Bridge must use the three locked
    /// accessors below instead: the mechanism's stroke/event containers are
    /// mutated from inside `step()` (motor writes), so a GUI-thread reader
    /// touching them directly races the emulation loop.
    CE1600PCard* ce1600pCard() { return m_ce1600pCard.get(); }

    /// GUI-safe (take m_mutex, like displaySnapshot()). Empty when no
    /// plotter is attached.
    std::vector<AlpsPlotterMechanism::FlatPoint> ce1600pPlotPoints() const;
    /// O(1) change token for the plot geometry (0 when no plotter attached).
    /// A GUI poll loop reads this every frame and only calls the copying
    /// `ce1600pPlotPoints()` when it has moved -- see `EmulatorViewModel.tick()`.
    uint64_t ce1600pPlotRevision() const;
    std::vector<std::string> drainCE1600PEvents();
    void clearCE1600PPaper();

    // ── CE-1600F floppy (attached as a union with CE-1600P, above) ──────
    //
    // All GUI-safe (take m_mutex, mirroring the plotter accessors above --
    // CE1600FCard's image/revision state is written from inside
    // step() on every data-register access, same race as the plotter
    // mechanism). No-op/empty-returning when no floppy is attached.
    bool ce1600fAttached() const { return m_ce1600fCard != nullptr; }
    std::vector<uint8_t> ce1600fDiskImage() const;
    uint64_t ce1600fRevision() const;
    void ce1600fEject();         // leaves the drive empty
    bool ce1600fHasDisk() const;
    bool ce1600fLoadImage(const uint8_t* data, size_t size);  // live hot-swap, no power-cycle needed
    /// 0 = side A, 1 = side B -- the software analogue of ejecting and
    /// flipping the physical disk (CE1600FCard::setSide()'s own comment).
    /// A no-op when no floppy is attached.
    int ce1600fSide() const;
    void ce1600fSetSide(int side);
    /// The "green lamp" (drive-active indicator) -- see CE1600FCard::
    /// motorOn(). False when no floppy is attached.
    bool ce1600fMotorOn() const;

    // ── CE-150 plotter (LH5803 side, MODE 1) ────────────────────────────
    //
    // The PC-1500's CE-150 attached to the PC-1600's LH5803 compatibility
    // CPU: its ROM window (LH5803 0xA000-0xBFFF, PV=0) and LH5810 block
    // (LH5803 ME1 0xB008-0xB00F) are served by the same Ce150Card the
    // PC-1500 uses, wired through LH5803SharedMemory. Mutually exclusive
    // with the CE-1600P on the shared 60-pin bus concept -- attaching one
    // detaches the other. A chip/machine reset re-anchors the card but
    // leaves it attached (like the CE-1600P).
    bool attachCE150(const uint8_t* rom, size_t romSize); // 8192 bytes
    void detachCE150();
    bool ce150Attached() const { return m_ce150Card != nullptr; }
    Ce150Card* ce150Card() { return m_ce150Card.get(); } // unlocked -- tests only

    std::vector<AlpsPlotterMechanism::FlatPoint> ce150PlotPoints() const;
    uint64_t ce150PlotRevision() const;
    std::vector<std::string> drainCE150Events();
    void clearCE150Paper();

    // ── Access (debug / tests) ───────────────────────────────────────────
    SC7852&       sc7852() { return m_sc7852; }
    const SC7852& sc7852() const { return m_sc7852; }
    LH5803&       lh5803() { return m_lh5803; }
    const LH5803& lh5803() const { return m_lh5803; }
    PC1600Bank&   bank() { return m_bank; }
    PC1600Memory& memory() { return m_z80Mem; }
    /// The LH5803's memory view -- its 0000-3FFF window aliases the Z-80's
    /// 8000-BFFF (Slot 1/2), so a slot card is visible through both.
    LH5803SharedMemory& lh5803Memory() { return m_lh5803Mem; }
    PC1600BusArbiter& busArbiter() { return m_arbiter; }
    bool sc7852Owns() const { return m_arbiter.sc7852Owns(); }

    // ── Serial port (RS-232C peer behind the TC8576F) ───────────────────
    //
    // Attach / detach the host-side peer for the PC-1600's serial port.
    // Non-owning: the caller owns the SerialLink and must keep it alive
    // until it detaches (`setSerialLink(nullptr)`) or the machine is
    // destroyed. Locked, so the GUI can attach/detach while the emulation
    // loop is running. `nullptr` restores the standalone no-peer
    // behaviour. A chip/machine reset does not clear the attachment.
    void setSerialLink(SerialLink* link);
    SerialLink* serialLink() const { return m_z80Mem.uart().serialLink(); }

    // ── Keyboard / display ────────────────────────────────────────────
    //
    // keyboard()/display() are unlocked direct access, for tests/headless
    // tools only (same convention PC1500Machine's cpu()/memory() already
    // use) -- pressKey/releaseKey/setOnKeyPressed/displaySnapshot below are
    // the locked, cross-thread-safe surface the GUI Bridge layer uses.
    PC1600Keyboard&       keyboard() { return m_z80Mem.keyboard(); }
    const PC1600Keyboard& keyboard() const { return m_z80Mem.keyboard(); }
    PC1600Display&        display() { return m_z80Mem.display(); }
    const PC1600Display&  display() const { return m_z80Mem.display(); }

    /// Named-key vocabulary per PC1600Keyboard::keyFromName. Unknown names
    /// are silently ignored, matching PC1500Machine's own pressKey/
    /// releaseKey convention.
    void pressKey(const std::string& name);
    void releaseKey(const std::string& name);
    void setOnKeyPressed(bool pressed);
    /// A point-in-time copy of the display's pixel state -- safe to read on
    /// a different thread while the emulation loop is mid-step() (see
    /// PC1600DisplaySnapshot's own doc comment).
    PC1600DisplaySnapshot displaySnapshot() const;

    /// Buzzer audio (mono int16 PCM at audioSampleRate()) produced from the
    /// OPC 18H drive line as emulated time advances. See PiezoSampler and
    /// PC1600Memory::m_opc. drainAudio() moves up to `max` of the oldest
    /// samples into `out`. Both take m_mutex.
    size_t drainAudio(int16_t* out, size_t max) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_z80Mem.piezo().drain(out, max);
    }
    void discardAudio() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_z80Mem.piezo().discard();
    }
    int audioSampleRate() const { return PiezoSampler::kDefaultSampleRate; }

    // ── Memory-slot connectors ───────────────────────────────────────
    // A card plugs in pin-for-pin; MemorySlotConnector drives the PC-1600
    // bay's pins (see Core/Connector/MemorySlotConnector.hpp).
    void attachSlot1Card(std::unique_ptr<ExpansionCard> card) { m_z80Mem.attachSlot1Card(std::move(card)); }
    void attachSlot2Card(std::unique_ptr<ExpansionCard> card) { m_z80Mem.attachSlot2Card(std::move(card)); }
    void detachSlot1() { m_z80Mem.detachSlot1(); }
    void detachSlot2() { m_z80Mem.detachSlot2(); }
    bool slot1Attached() const { return m_z80Mem.slot1Attached(); }
    bool slot2Attached() const { return m_z80Mem.slot2Attached(); }

    /// Writes `data` starting at `address` on the Z-80 (SC7852) side, on
    /// whichever page/bank is currently selected -- a direct, non-BASIC
    /// memory-poke path (see PC1600BasicTyper/PC1600BasicLoader for
    /// loading a BASIC program instead). Returns false, writing nothing,
    /// if any byte in the range would land on ROM or an open-bus/
    /// unattached region -- an all-or-nothing check up front, not a
    /// partial write on failure.
    bool pokeMemory(uint16_t address, const uint8_t* data, size_t size);

    // ── Debug reads (GUI-safe: take m_mutex, like pokeMemory()) ───────────
    //
    // The unlocked memory()/bank() accessors above are "tests/headless tools
    // only"; these are the cross-thread-safe surface the GUI Bridge uses for
    // the debug panel's "Pointers" / "Dump Mem" buttons.

    /// One byte, read through the currently-selected banks (Port 31H/28H/3DH
    /// state as-is) -- same semantics as PC1500Machine::debugPeek().
    uint8_t debugPeek(uint16_t addr);

    /// True while SC7852 owns the bus -- false means the OFF-key/auto-
    /// power-off handoff has parked the machine on the LH5803 side (see
    /// `sc7852Owns()`, the unlocked headless-tools accessor above, whose
    /// own state this reflects). GUI-safe locked wrapper for a caller
    /// (the CE-1600P attach toggle) that needs to poll for the machine
    /// having actually finished powering down.
    bool debugSc7852Owns();

    /// The fixed internal 16 KB RAM (page D bank 0, Z-80 C000-FFFF) copied
    /// into `out` (which must hold kInternalRamSize bytes) -- the live
    /// state, read directly, no bank-register games.
    static constexpr size_t kInternalRamSize = 0x4000;
    void debugCopyInternalRam(uint8_t* out);

    /// Direct backing-store writes, bypassing the emulated bus / bank
    /// gating -- the write side of debugCopyInternalRam() / debugSlotImage().
    /// Used by the fast BASIC loader to scatter a tokenised program across
    /// module banks + internal RAM regardless of the current page-C bank
    /// state. GUI-safe (take m_mutex, like pokeMemory()). Return false,
    /// writing nothing, on an out-of-range offset or an empty/read-only
    /// slot. `off` is into the concatenated card image (debugSlotImage()'s
    /// address space) for debugWriteSlotImage(), or 0..kInternalRamSize for
    /// debugWriteInternalRam().
    bool debugWriteInternalRam(size_t off, const uint8_t* data, size_t n);
    bool debugWriteSlotImage(int slot, size_t off, const uint8_t* data, size_t n);

    /// The entire backing store of the card in Slot `slot` (1 or 2) --
    /// every bank / vertical bank concatenated ascending
    /// (ExpansionCard::debugImage()); empty when the slot is empty or the
    /// card exposes no readable storage. For the debug "Dump Mem" contents
    /// view, which chunks it into 16 KB bank rows.
    std::vector<uint8_t> debugSlotImage(int slot);

    /// What a CPU page currently decodes to, for the debug panel's live
    /// address-map view. Mirrors PC1600Memory::resolveConst()'s branch set
    /// plus the two slot windows; `PeripheralRom` is the CE-1600P ROM at
    /// page-B banks 4/5, which has no backing store yet (shown, not read).
    enum class PageTarget : uint8_t {
        OpenBus = 0,
        SystemRomLo,   // Bank 0 lower 16 KB (page A)
        SystemRomHi,   // Bank 0 upper 16 KB (page B bank 0)
        Bank3Rom,      // page B bank 3, Port 3DH b2 set
        Bank3bRom,     // page B bank 3, Port 3DH b2 clear (hidden BASIC ROM)
        Bank6Rom,      // ROM IV, page C bank 6
        PeripheralRom, // CE-1600P, page B banks 4/5 -- not modelled
        InternalRam,   // fixed internal 16 KB, page D bank 0
        Slot1,
        Slot2,
    };

    /// Bank / slot-mapping register state for the debug "Dump Mem" panel --
    /// its header (which slot each page-C bank routes to, the SLOT1MAP /
    /// SLOT2MAP gate-array remap, the Slot 2 vertical bank), its per-column
    /// labels (`slotNCardBank`), and its live address-map view (`target` /
    /// `slotmapRedirect`, one entry per CPU page A/B/C/D). A locked snapshot
    /// of plain values, like debugPeek() -- no references into live state.
    struct DebugBankState {
        uint8_t port31{0};   // IOW MAP -- the page A/B/C/D bank-select register
        uint8_t port28{0};   // last OUT (28H) -- Slot 2 vertical-bank select
        uint8_t port3c{0};   // SLOT1MAP / SLOT2MAP gate-array control
        uint8_t pageABank{0}, pageBBank{0}, pageCBank{0}, pageDBank{0};
        uint8_t slot2MapMode{0};      // PC1600Bank::slot2MapMode(): 0 default / 1 / 2
        bool    slot1MapRemapped{false}; // Port 3CH b2 -- PC1600Bank::slot1MapActive()
        bool    hiddenBasicRom{false};   // Port 3DH b2 clear -> hidden Bank 3b BASIC ROM
        int     slot1CardBank{-1};    // the module's OWN latched bank, -1 = none / unbanked
        int     slot2CardBank{-1};
        int     slot1CardBankCount{-1}; // the module's total bank count, -1 = none / unbanked
        int     slot2CardBankCount{-1};
        PageTarget target[4]{};       // A,B,C,D -- what each page decodes to right now
        bool    slotmapRedirect[4]{}; // A,B,C,D -- true when a SLOTMAP rewrite is live for that page
    };
    DebugBankState debugBankState();

    // ── Trace (both CPUs' trace rings drain into one
    // TRACE.bin, tagged by cpuId; see TraceWriter.swift) ──────────────────
    //
    // setTraceEnabled()/traceEnabled() just flip the CPU trace flags and
    // expect an external consumer (the GUI's Swift drain at 60 Hz) to pump
    // the rings. beginCpuTrace()/endCpuTrace() below instead capture a full
    // instruction trace to a file entirely inside the Core -- step() drains
    // both rings into it -- for the synchronous, tick-less
    // PC1600PresetLoader::applyPC1600Preset() `trace:` step. Same design as
    // PC1500Machine's own headless-trace pair; don't mix the two APIs on
    // one machine.
    void setTraceEnabled(bool enabled);
    bool traceEnabled() const { return m_traceEnabled; }

    /// Begin capturing to `handle` (open for binary writing; this machine
    /// takes ownership and endCpuTrace() closes it). Sets `flags` as the
    /// active trace level on BOTH CPUs; their frames land in one file,
    /// tagged by cpuId (SC7852 / LH5803). Returns false and does nothing
    /// if `handle` is null or a capture is already active.
    bool beginCpuTrace(std::FILE* handle, uint32_t flags);

    /// Drain what's left in both rings, write SESSION_END, close the file,
    /// clear the trace flags. No-op if no capture is active.
    void endCpuTrace();

    bool cpuTraceActive() const { return m_traceFile != nullptr; }

private:
    mutable std::mutex m_mutex; // guards step()/reset()/pressKey/releaseKey/setOnKeyPressed/displaySnapshot

    // See setYieldHook(). m_yieldCountdown only runs down while a hook is set.
    std::function<void()> m_yieldHook;
    uint64_t m_yieldInterval = 0;
    uint64_t m_yieldCountdown = 0;

    /// Shared body of reset()/allReset(); caller holds m_mutex.
    void resetLocked();

    PC1600Bank m_bank;
    PC1600Memory m_z80Mem;
    SC7852 m_sc7852;
    LH5803SharedMemory m_lh5803Mem;
    LH5803 m_lh5803;
    PC1600BusArbiter m_arbiter;

    std::unique_ptr<CE1600PCard> m_ce1600pCard; // see attachCE1600P()
    std::unique_ptr<CE1600FCard> m_ce1600fCard; // union-attached with m_ce1600pCard
    std::unique_ptr<Ce150Card> m_ce150Card;     // see attachCE150() -- LH5803-side plotter (MODE 1)

    bool m_traceEnabled{false};

    // ── Headless CPU-trace file -- see beginCpuTrace() ───────────────────
    std::unique_ptr<PC1500TraceFile> m_traceFile;
    uint32_t m_traceDrainCounter{0};
    // Sized to match LH5801's 512-entry ring so one drain empties it; the
    // SC7852 ring is far larger but is drained often enough (every
    // kTraceDrainInterval < 512 instructions) that 512 covers the common
    // case, and PC1500TraceFile::writeGap() covers the pathological one.
    static constexpr uint32_t kTraceDrainBufFrames = 512;
    static constexpr uint32_t kTraceDrainInterval = 256;
    CpuFrame m_lhTraceDrainBuf[kTraceDrainBufFrames];
    Z80CpuFrame m_z80TraceDrainBuf[kTraceDrainBufFrames];
    /// Drain both CPU trace rings into m_traceFile. Caller holds m_mutex;
    /// safe only while m_traceFile is set. Cross-ring order is per-drain,
    /// not globally chronological -- the cpuId tag disambiguates, and
    /// tools/read_trace.py already handles the interleave.
    void pumpTraceFile();
    /// step() hook: drain the rings into the file every kTraceDrainInterval
    /// instructions while a headless capture is active. Caller holds
    /// m_mutex. (step() is where both step() and runCycles() converge, so
    /// this one call site covers both.)
    void maybeDrainTrace() {
        if (m_traceFile && ++m_traceDrainCounter >= kTraceDrainInterval) {
            pumpTraceFile();
            m_traceDrainCounter = 0;
        }
    }

    // Sub-CPU 1/64s timer pulse (port 32H bit 4) -- a free-running, 50%-duty
    // 64 Hz square wave confirmed by Systemhandbuch §7.3/§7.4 (see
    // PC1600Memory::setTimer64Bit()'s own comment), modeled here as a
    // T-state accumulator against the SC-7852's own 3.58 MHz crystal
    // (PC-1600-CPU-SC7852-Z80.md §2.1) since that's the domain the ROM's
    // polling loop actually observes it in -- accumulates only SC7852
    // T-states (not LH5803 cycles, a different clock domain entirely), so
    // the pulse effectively pauses while the SC7852 is parked, a real but
    // small deviation from true hardware's always-running crystal.
    // = kTStateHz / 64 / 2 rounded to nearest (27968.75 -> 27969).
    static constexpr int kTimer64HalfPeriodTStates = (kTStateHz + 64) / 128;
    int m_timer64Accum{0};
    bool m_timer64State{false};

    // Sub-CPU interrupt (port 32H bit 6, INT6 pin 84). Per
    // PC-1600-CPU-SC7852-Z80.md §5.2 this one line aggregates everything
    // the LU-57813P raises: the 0.5s timer, low-battery/analog-in/CI
    // checks, auto-power-off, the RS-232C timeout, and the wakeup/alarm1/
    // alarm2 timers. Only the 0.5s timer is modeled here -- it is the one
    // that free-runs with no external stimulus, and §5.2's own firmware
    // list (TRM §3.4.1(2)(e)) makes it the carrier for the periodic
    // housekeeping tasks. The event-driven members of that list stay
    // unraised until there is something to raise them: no battery or
    // analog model, no CI line, and no serial peripheral to time out.
    //
    // Same accumulator shape as the 1/64s timer above, but no square-wave
    // state: that timer keeps a level because it *publishes* one (PB5, via
    // setTimer64Bit()). Nothing observes this one's level -- the sub-CPU
    // line is edge-only -- so it is simply one interrupt per full period.
    static constexpr int kTimer05PeriodTStates = kTStateHz / 2; // 0.5s
    int m_timer05Accum{0};

    // LU-57813P real-time clock: one calendar second per second of
    // emulated time. Same accumulator shape as the two timers above, but
    // -- unlike them -- fed from *both* bus-ownership branches of step()
    // (SC7852 T-states normally, LH5803 T-states while the OFF-key/
    // auto-power-off shutdown has handed it the bus), matching the real
    // chip's always-powered VGG rail: the calendar clock never freezes
    // just because a given CPU isn't the one stepping. So the emulated
    // clock tracks emulated wall-time, not just SC7852 CPU time, and a
    // seeded time drifts only if the host cannot hold the emulation at
    // real speed (exactly as a real PC-1600 drifts against its own
    // crystal). The clock itself lives in PC1600SubCpu; this only paces
    // PC1600SubCpu::tickOneSecond().
    static constexpr int kRtcPeriodTStates = kTStateHz; // 1 s
    int m_rtcAccum{0};
};
