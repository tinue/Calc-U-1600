#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../CPU/LH5801/LH5801.hpp"
#include "../Connector/Ce150Card.hpp"
#include "../Connector/Ce158Port.hpp"
#include "../Connector/ExpansionConnector.hpp"
#include "../Connector/SystemBus.hpp"
#include "PC1500Clocks.hpp"
#include "PC1500Display.hpp"
#include "PC1500Memory.hpp"
#include "PC1500TraceFile.hpp"

// ── PC-1500A machine facade ──────────────────────────────────────────────
//
// Standalone PC-1500A core -- not a subset of the PC-1600 dual-CPU core.
// Owns a value-type CPU + memory pair, mirroring Calc-U-59's TI59Machine
// role: step()/reset()/pressKey()/getDisplay() plus the trace/breakpoint
// passthrough the debugger builds on.
//
// Keyboard matrix scanning (pressKey/releaseKey) and LCD readout
// (display()) are now real, not stubs — see PC1500Keyboard.hpp and
// PC1500Display.hpp for the confirmed hardware behavior they're built
// from (Phase 1's original gap here is closed). The ON key is
// deliberately excluded from the named-key vocabulary: real hardware
// wires it straight to the CPU's BFI power-on latch, not the matrix —
// see setOnKeyPressed() and PC1500Keyboard.hpp's doc comment.
//
// Thread safety: Phase 2's app steps the CPU continuously on a background
// queue while the UI thread reads display/key state at 60Hz — the same
// two-thread split Calc-U-59's TI59Machine uses, and (per its own
// AppArchitecture.md) with the same shape: one facade-level mutex here,
// separate from LH5801's own trace-ring mutex (which guards a different,
// narrower concern and is fine to keep independent). Every method that
// touches CPU/memory state (step, runCycles, pressKey, releaseKey, reset,
// display) takes m_mutex internally. cpu()/memory() return unlocked direct
// references — safe for single-threaded contexts (headless tests, the CLI
// tool) but callers sharing a PC1500Machine across threads (the Bridge
// wrapper) must go through the locked methods, not these, for anything
// that isn't a one-off debug peek.
class PC1500Machine {
public:
    /// LH5801 cycles per second -- the unit runCycles() counts (PC1500Clocks.hpp).
    static constexpr uint32_t kCpuHz = kPC1500CpuHz;

    explicit PC1500Machine(PC1500Variant variant = PC1500Variant::PC1500A);

    PC1500Variant variant() const { return m_memory.variant(); }

    bool loadROM(const uint8_t* data, std::size_t size);
    bool loadROMFile(const std::string& path);
    /// The reset button: CPU and chips reset, RAM kept.
    void reset();
    /// Reset with all RAM cleared to 0x00 first (Machine > Reset All). The
    /// PC-1500 has no separate ALL RESET line; this models pulling the
    /// batteries, like PC1600Machine::allReset().
    void allReset();

    /// Seed the uPD1990AC real-time clock from a host date/time so BASIC's
    /// TIME reads back something sensible instead of 00000. `month` is
    /// 1-12, the rest plain decimals; day-of-week is derived here. A real
    /// PC-1500's RTC is battery-backed and the boot ROM never re-inits it,
    /// so this one seed rides through boot -- but it is deliberately kept
    /// out of reset() so headless tests / the CLI stay deterministic; the
    /// Bridge wrapper calls this after reset() with the real host time.
    void seedClock(int year, int month, int day, int hour, int minute, int second, int millisecond = 0);

    /// Execute one instruction. Returns the cycle count consumed (0 if
    /// halted with no pending interrupt, or a breakpoint was just hit).
    int step();

    /// Run until either `maxCycles` total cycles have been consumed or a
    /// breakpoint stops execution first. Returns the number of cycles
    /// actually consumed.
    uint64_t runCycles(uint64_t maxCycles);

    /// Optional host callback invoked from inside runCycles() roughly every
    /// `intervalCycles` of emulated time (counted across calls, so many
    /// short runCycles() calls still add up). Lets a UI thread that drives
    /// a long synchronous preset/program load keep its event loop alive
    /// (repaint, show a progress popup) without the loaders knowing about
    /// it. Called with m_mutex NOT held, so the hook may safely read the
    /// display snapshot. The hook must not drive the machine itself. An
    /// empty function (the default) removes it.
    void setYieldHook(std::function<void()> hook, uint64_t intervalCycles);

    // Named-key vocabulary used by preset scripts and the BASIC typer. Unknown
    // names (including "on" -- see class doc comment) are silently ignored,
    // matching a real keyboard's behavior when a matrix position isn't wired
    // to anything.
    void pressKey(const std::string& name);
    void releaseKey(const std::string& name);
    void setOnKeyPressed(bool pressed);

    /// Queues a keystroke to be tapped (pressed, held, released, then
    /// idled) at real hardware's own key-scan cadence -- see
    /// advanceKeyQueue()'s doc comment. Unlike pressKey()/releaseKey(),
    /// the caller never has to also deliver a matching release; the queue
    /// owns the whole press/hold/release/idle cycle internally, once per
    /// step()/runCycles() call, so a UI event source that can't reliably
    /// guarantee delivering a matching "key up" (a fast/overlapping tap, a
    /// focus change mid-press) can never leave a key stuck down in the
    /// matrix, which would wedge the ROM's own debounce and stop any
    /// further keystroke from being recognized. An unmapped `name` is
    /// silently ignored, matching pressKey()'s own convention. If the
    /// queue is already at capacity (kKeyQueueCapacity), the new keystroke
    /// is silently dropped -- matching real hardware's own behavior when
    /// typed faster than its key-scan loop can keep up, not a bug to work
    /// around.
    void enqueueKey(const std::string& name);

    /// Locked snapshot of the display -- safe to call from a UI thread
    /// while another thread is mid-step(). Prefer this over cpu()/memory()
    /// for anything shared across threads.
    PC1500Display display() const;

    /// Whether the LCD panel is actually lit -- for the app to blank the
    /// dot-matrix and indicator bar rather than freeze-frame the last
    /// image. True DISP (LH5801::displayOn(), the RDP/SDP flip-flop) isn't
    /// by itself enough: a real multiplexed LCD needs the CPU continuously
    /// re-driving its backplate signals to stay visible at all, and a
    /// genuine power-down (poweredOff(), see LH5801's own doc comment)
    /// stops that regardless of DISP's last logical value -- confirmed by
    /// tracing the real ROM's OFF-key handler, which reaches OFF (0xFD
    /// 0x4C) without ever executing RDP first. So this is off whenever
    /// either signal says the backplate isn't being driven.
    bool isDisplayOn() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cpu.displayOn() && !m_cpu.poweredOff();
    }

    /// Buzzer audio (mono int16 PCM at audioSampleRate()), produced from
    /// the PC6 drive line as emulated time advances -- see PiezoSampler.
    /// drainAudio() moves up to `max` of the oldest samples into `out`.
    size_t drainAudio(int16_t* out, size_t max) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_memory.piezo().drain(out, max);
    }
    void discardAudio() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_memory.piezo().discard();
    }
    int audioSampleRate() const { return PiezoSampler::kDefaultSampleRate; }
    /// Buzzer line level changes so far (PiezoSampler::edgeCount()).
    uint64_t buzzerEdgeCount() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_memory.piezo().edgeCount();
    }

    // Locked, UI-thread-safe debug reads so the App layer can log
    // Core-internal state (PC, and RAM bytes like the cursor-key dispatch
    // gate at 0x7B0EH, or the BREAK_LINE pointer at 0x78AEH/0x78AFH) right
    // alongside its own key-event logs, for direct correlation.
    uint8_t debugPeek(uint16_t addr) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_memory.peek(addr);
    }
    uint16_t debugPC() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cpu.pc();
    }

    /// The attached expansion module's own current bank (-1 if no card is
    /// attached, or the card has no bank concept) -- for the "Dump Mem"
    /// panel's per-region label. See ExpansionCard::debugCurrentBank().
    int debugSlotCardBank() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_attachedExpansionCard ? m_attachedExpansionCard->debugCurrentBank() : -1;
    }

    /// The attached expansion module's total bank count (-1 if no card is
    /// attached, or the card has no bank concept). See
    /// ExpansionCard::debugBankCount().
    int debugSlotCardBankCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_attachedExpansionCard ? m_attachedExpansionCard->debugBankCount() : -1;
    }

    /// The attached expansion module's whole backing image (every bank
    /// concatenated), read directly off the card rather than through the
    /// CPU-visible, single-bank-at-a-time address space -- for a "dump
    /// whole card" debug feature that needs every bank in one pass, not
    /// just whichever one is currently latched in. See
    /// ExpansionCard::debugImage(). Empty if no card is attached.
    std::vector<uint8_t> debugSlotCardImage() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_attachedExpansionCard ? m_attachedExpansionCard->debugImage() : std::vector<uint8_t>{};
    }

    /// Whether the attached module actually decodes `addr` -- see
    /// PC1500Memory::debugSlotResponds().
    bool debugSlotResponds(uint16_t addr) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_memory.debugSlotResponds(addr);
    }

    LH5801&       cpu() { return m_cpu; }
    const LH5801& cpu() const { return m_cpu; }
    PC1500Memory&       memory() { return m_memory; }
    const PC1500Memory& memory() const { return m_memory; }

    // The 40-pin (single-slot) and 60-pin (daisy-chain) connectors, wired
    // into m_memory by the constructor. No card is attached by default --
    // tests and the app layer attach directly via these accessors.
    ExpansionConnector& expansionConnector() { return m_expansionConnector; }
    SystemBus&          systemBus() { return m_systemBus; }

    /// Takes ownership of `card` and attaches it to the 40-pin
    /// ExpansionConnector -- for callers (the preset loader, the CLI) with
    /// no longer-lived object of their own to hold the card. Replaces any
    /// previously-attached owned card (matches the 40-pin connector's own
    /// single-slot semantics -- see ExpansionConnector::attach()). Takes
    /// m_mutex: the emulation thread dispatches bus accesses to the card.
    void attachExpansionCard(std::unique_ptr<ExpansionCard> card) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_expansionConnector.attach(card.get());
        m_attachedExpansionCard = std::move(card); // old card freed only after it's unplugged
    }

    // ── CE-150 plotter / printer (60-pin system bus) ─────────────────────
    //
    // The CE-150 plugs into the 60-pin connector (SystemBus): its ROM
    // window (guest 0xA000-0xBFFF, ME0) and its LH5810 register block
    // (guest 0xB008-0xB00F, ME1) are both served by the card off that bus.
    // `attachCE150` builds a card, loads its 8 KB ROM, resets it, and
    // attaches it to `m_systemBus`. A chip/machine reset does not clear the
    // attachment (it does re-anchor the card -- see reset()).
    bool attachCE150(const uint8_t* rom, size_t romSize);
    void detachCE150();
    bool ce150Attached() const { return m_ce150Card != nullptr; }
    /// Unlocked direct access -- headless/tests only, same convention as
    /// cpu()/memory(). The GUI Bridge must use the locked accessors below.
    Ce150Card* ce150Card() { return m_ce150Card.get(); }

    /// GUI-safe (take m_mutex, like the CE-1600P plot accessors on
    /// PC1600Machine). Empty / 0 when no plotter is attached.
    std::vector<AlpsPlotterMechanism::FlatPoint> ce150PlotPoints() const;
    uint64_t ce150PlotRevision() const;
    std::vector<std::string> drainCE150Events();
    void clearCE150Paper();

    // ── CE-158 RS-232C / Centronics interface (60-pin system bus) ────────
    //
    // Attached to the same SystemBus chain as the CE-150, alone or together
    // with it (on hardware it plugs into the PC-1500 directly or into the
    // CE-150's rear connector). `attachCE158` builds a card, loads its
    // 16 KB ROM, resets it and attaches it; the serial link set with
    // setCE158SerialLink() is kept across detach/attach, so the host PTY
    // stays put while the user toggles the interface.
    bool attachCE158(const uint8_t* rom, size_t romSize);
    void detachCE158();
    bool ce158Attached() const { return m_ce158.attached(); }
    /// Unlocked direct access -- headless/tests only (see ce150Card()).
    Ce158Card* ce158Card() { return m_ce158.card(); }
    /// Non-owning; the caller keeps `link` alive until it sets another one
    /// (or nullptr) or destroys the machine. GUI-safe (takes m_mutex).
    void setCE158SerialLink(SerialLink* link);
    /// GUI-safe: the bytes printed on the Centronics port since the last
    /// call. Empty when no CE-158 is attached.
    std::vector<uint8_t> drainCE158ParallelOutput();

    // Trace/breakpoint passthrough (Phase 2a's debugger consumes this via
    // the Bridge layer; exercised directly by headless tests since Phase
    // 1). Unlocked, like cpu()/memory() above -- LH5801 already guards its
    // own trace ring/breakpoint state internally (see LH5801.hpp), so no
    // additional locking is needed here.
    void     setTraceFlags(uint32_t flags) { m_cpu.setTraceFlags(flags); }
    uint32_t traceFlags() const { return m_cpu.traceFlags(); }
    uint32_t drainTraceEvents(CpuFrame* out, uint32_t max, uint32_t* outLost) { return m_cpu.drainTraceEvents(out, max, outLost); }

    // ── Headless CPU-trace file (the preset loader's `trace:` step) ──────
    //
    // Unlike the setTraceFlags()/drainTraceEvents() pair above -- which
    // leave draining the ring to the caller -- this captures a full
    // instruction trace to a file entirely inside the Core: step()/
    // runCycles() drain the ring into the file themselves. Used by the
    // GUI's TRACE button and the preset `trace:` step. Don't mix the two
    // APIs on one machine.

    /// Begin capturing to `handle` (open for binary writing; this machine
    /// takes ownership and endCpuTrace() closes it). Sets `flags` as the
    /// active trace level. Returns false and does nothing if `handle` is
    /// null or a capture is already active.
    bool beginCpuTrace(std::FILE* handle, uint32_t flags);

    /// Drain what's left in the ring, write SESSION_END, close the file,
    /// clear the trace flags. No-op if no capture is active.
    void endCpuTrace();

    bool cpuTraceActive() const { return m_traceFile != nullptr; }
    /// Size of the active capture so far (0 when none); see
    /// PC1500TraceFile::bytesWritten().
    uint64_t cpuTraceBytes() const { return m_traceFile ? m_traceFile->bytesWritten() : 0; }
    void addBreakpoint(uint16_t addr) { m_cpu.addBreakpoint(addr); }
    void removeBreakpoint(uint16_t addr) { m_cpu.removeBreakpoint(addr); }
    void clearBreakpoints() { m_cpu.clearBreakpoints(); }
    bool consumeBreakpointHit() { return m_cpu.consumeBreakpointHit(); }

private:
    PC1500Memory m_memory;
    LH5801       m_cpu;
    ExpansionConnector m_expansionConnector;
    SystemBus          m_systemBus;
    std::unique_ptr<ExpansionCard> m_attachedExpansionCard; // see attachExpansionCard()
    std::unique_ptr<Ce150Card> m_ce150Card;                 // see attachCE150()
    Ce158Port m_ce158;                                      // see attachCE158()
    mutable std::mutex m_mutex;

    // See setYieldHook(). m_yieldCountdown only runs down while a hook is set.
    std::function<void()> m_yieldHook;
    uint64_t m_yieldInterval = 0;
    uint64_t m_yieldCountdown = 0;

    // ── Headless CPU-trace file -- see beginCpuTrace() ───────────────────
    std::unique_ptr<PC1500TraceFile> m_traceFile;
    uint32_t m_traceDrainCounter{0};
    // Scratch buffer for one drainTraceEvents() call -- sized to match
    // LH5801's trace ring so a single drain empties it.
    static constexpr uint32_t kTraceDrainBufFrames = 512;
    // Drain the ring every kTraceDrainInterval instructions -- strictly
    // less than kTraceDrainBufFrames so nothing is lost in the common
    // case (PC1500TraceFile::writeGap() covers the pathological one).
    static constexpr uint32_t kTraceDrainInterval = 256;
    CpuFrame m_traceDrainBuf[kTraceDrainBufFrames];
    /// Bodies of the public detach calls; caller holds m_mutex. The
    /// attach/detach calls take it because step() dispatches bus accesses
    /// to these cards on the emulation thread.
    void detachCE150Locked();
    void detachCE158Locked();

    /// Advances everything outside the CPU that runs on real time (RTC,
    /// buzzer, key queue, CE-150/CE-158) by `cycles`. The one place both
    /// step() and runCycles() feed, so they stay in step. Caller holds
    /// m_mutex.
    void advancePeripherals(uint32_t cycles);

    /// Drain the CPU trace ring into m_traceFile. Caller must hold
    /// m_mutex; safe to call only while m_traceFile is set.
    void pumpTraceFile();
    /// step()/runCycles() hook: drain the trace ring into the file every
    /// kTraceDrainInterval instructions while a headless capture is active.
    /// Caller must hold m_mutex.
    void maybeDrainTrace() {
        if (m_traceFile && ++m_traceDrainCounter >= kTraceDrainInterval) {
            pumpTraceFile();
            m_traceDrainCounter = 0;
        }
    }

    // ── Paced keystroke queue -- see enqueueKey()'s doc comment ──────────
    static constexpr size_t kKeyQueueCapacity = 16;
    std::deque<std::string> m_keyQueue;
    enum class KeyQueuePhase { Idle, Holding, Gap };
    KeyQueuePhase m_keyQueuePhase{KeyQueuePhase::Idle};
    std::string   m_keyQueueCurrentKey;
    uint64_t      m_keyQueuePhaseCyclesRemaining{0};

    /// Advances the keystroke queue's press/hold/release/idle state
    /// machine by `cycles` (the same cycle count step()/runCycles() just
    /// consumed) -- called from both, the same way advanceRtc()/
    /// updatePUPV() already are, so live interactive typing (runCycles(),
    /// the GUI's own background loop) and single-instruction stepping
    /// (step(), headless tests) both pace queued keys identically. Uses
    /// PC1500BasicTyper.cpp's own tapKey() cadence (kTapFrames=4 hold +
    /// kIdleFrames=4 idle @ 60fps, ~67ms each) -- the confirmed real-
    /// hardware key-scan timing already proven reliable there for
    /// preset/BASIC-program typing; duplicated locally (not shared via a
    /// header) matching this project's existing convention for small,
    /// stable, cross-file timing constants. Directly manipulates m_memory.keyboard() rather
    /// than going through pressKey()/releaseKey() -- both already lock
    /// m_mutex, and this is only ever called from within a method that's
    /// already holding it (std::mutex isn't recursive).
    void advanceKeyQueue(uint32_t cycles);
};
