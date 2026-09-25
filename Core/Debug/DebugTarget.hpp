#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../CPU/WatchSet.hpp"
#include "DebugExpression.hpp"
#include "Disasm/Disassembly.hpp"

// ── Debugger view of one machine ─────────────────────────────────────────
//
// What the DAP server needs from a machine, independent of which one it is:
// its CPUs as threads, their registers and history, side-effect-free
// memory access per CPU address space, PC breakpoints and memory watches,
// and single-instruction execution. PC1500DebugTarget and
// PC1600DebugTarget adapt the two machines.
//
// Everything here runs on the thread that drives the machine (the GUI
// thread in the app), never concurrently with it -- no locking.

namespace debug {

enum class CpuKind : uint8_t { LH5801, LH5803, Z80 };

/// Address spaces a memory reference can name: the Z-80's single memory
/// space and the LH580x's ME0 share 0; ME1 is 1 (same values as
/// WatchHit::space).
enum Space : uint8_t { kSpaceMain = 0, kSpaceME1 = 1 };

struct Thread {
    int id = 0;            ///< stable within one machine: 1, and 2 on the PC-1600
    CpuKind kind = CpuKind::LH5801;
    std::string name;      ///< "LH5801", "Z80 (SC7852)", "LH5803"
};

struct Register {
    std::string name;      ///< lower case, as the expression evaluator knows it
    uint32_t value = 0;
    uint8_t bits = 8;      ///< 1 (a flag or PU/PV), 8 or 16
};

/// One entry of a CPU's instruction history (see HistoryRing.hpp).
struct HistoryEntry {
    uint16_t pc = 0;            ///< the instruction's address
    uint8_t bytes[5] = {};
    uint8_t len = 0;            ///< 0 for an interrupt entry
    bool interrupt = false;
    std::vector<Register> registers; ///< after the instruction
};

/// Bank qualifier of a listing or breakpoint; -1 = don't care. `bank`: the
/// PC-1600 Z-80's page bank (0-7) at the address (vertical banks are not
/// modelled). `me`: LH580x ME0/ME1 -- code is always fetched from ME0.
/// `pu` / `pv`: the LH580x PU/PV flags.
struct BankKey {
    int bank = -1, me = -1, pu = -1, pv = -1;
    bool any() const { return bank >= 0 || me >= 0 || pu >= 0 || pv >= 0; }
    std::string describe() const; ///< "bank 3", "pv=1", "" when unqualified
};

/// Why a run or step came back.
struct Stop {
    enum Kind : uint8_t { None, Breakpoint, Watch, Budget };
    Kind kind = None;
    int thread = 0;       ///< the CPU that hit it (Breakpoint / Watch)
    WatchHit hit;         ///< Watch only
};

class DebugTarget {
public:
    virtual ~DebugTarget() = default;

    virtual std::vector<Thread> threads() const = 0;
    /// The thread whose CPU currently owns the bus (executes on step()).
    virtual int busOwner() const = 0;
    CpuKind kindOf(int thread) const;

    // ── Registers ─────────────────────────────────────────────────────────
    /// The CPU's registers, in display order; the status register (T / F)
    /// is one entry, its bits named by flagNames().
    virtual std::vector<Register> registers(int thread) const = 0;
    /// Any name registers() lists, plus the 8-bit halves (Z-80 a/f/b/c/...,
    /// LH580x xl/xh/...) and single flags as <flag>f ("cf", "zf", "ief").
    /// False for an unknown name.
    virtual bool readRegister(int thread, const std::string& name, uint32_t* value) const = 0;
    virtual bool writeRegister(int thread, const std::string& name, uint32_t value) = 0;
    virtual uint16_t pc(int thread) const = 0;
    virtual uint16_t sp(int thread) const = 0;
    bool halted(int thread) const { return isHalted(thread); }

    // ── Memory ────────────────────────────────────────────────────────────
    /// Reads without side effects. False: the byte can't be read without
    /// disturbing a device (an I/O register) -- DAP "unreadable".
    virtual bool peek(int thread, Space space, uint16_t addr, uint8_t* value) const = 0;
    /// Host-path write; false if nothing stored it (ROM, open bus, I/O).
    virtual bool poke(int thread, Space space, uint16_t addr, uint8_t value) = 0;

    /// Expression context for `thread`: its registers and flags first, then
    /// `symbols` (may be empty); memory through peek().
    ExpressionContext expressionContext(int thread,
                                        std::function<bool(const std::string&, int64_t*)> symbols = {}) const;

    // ── Code ──────────────────────────────────────────────────────────────
    disasm::Decoded decode(int thread, uint16_t addr, const disasm::SymbolFn& symbols = {}) const;
    /// Bank qualifiers of the code at `addr` right now: the PC-1600 page
    /// bank (0-7) for the Z-80, -1 elsewhere; PU/PV for the LH580x.
    virtual int bankAt(int thread, uint16_t addr) const = 0;
    virtual bool pu(int thread) const = 0;
    virtual bool pv(int thread) const = 0;
    /// Whether code at `addr` on `thread` currently satisfies `key`.
    bool bankMatches(int thread, const BankKey& key, uint16_t addr) const;

    // ── History ───────────────────────────────────────────────────────────
    virtual uint32_t historySize(int thread) const = 0;
    virtual HistoryEntry history(int thread, uint32_t age) const = 0;
    /// Instructions retired so far (grows by one per instruction; reset
    /// by a machine reset). Used to tell that a CPU has moved.
    virtual uint32_t retired(int thread) const = 0;

    // ── Breakpoints and watches ───────────────────────────────────────────
    /// Replaces the thread's PC breakpoints. Checking is enabled only while
    /// some CPU has at least one (see breakpointsActive()).
    void setBreakpoints(int thread, const std::vector<uint16_t>& addrs);
    const std::vector<uint16_t>& breakpoints(int thread) const;
    bool breakpointsActive() const;
    /// Replaces the thread's memory watches (in its own address space);
    /// empty turns its checking off.
    void setWatches(int thread, const std::vector<WatchSet::Watch>& watches);

    // ── Execution ─────────────────────────────────────────────────────────
    /// Free run for up to `budget` machine cycles (the machine's own
    /// runCycles() unit). Resumes past a breakpoint the CPUs sit on.
    Stop run(uint64_t budget);
    /// One machine step: the bus owner executes one instruction (or idles
    /// one halted tick). Resumes past a breakpoint the CPUs sit on.
    Stop step();
    /// Steps until `done` returns true after a step, a breakpoint or watch
    /// stops it, or `maxSteps` machine steps have run (Stop::Budget).
    Stop runUntil(uint64_t maxSteps, const std::function<bool()>& done);
    /// Steps until `thread` has retired one instruction (the other CPU of
    /// a PC-1600 runs meanwhile if it owns the bus).
    Stop stepInstruction(int thread, uint64_t maxSteps);

    /// Machine reset / all reset (RAM cleared first).
    virtual void reset(bool allReset) = 0;

protected:
    virtual bool isHalted(int thread) const = 0;
    virtual void applyBreakpoints(int thread, const std::vector<uint16_t>& addrs) = 0;
    virtual void enableBreakpointChecks(bool on) = 0;
    virtual void resumePastBreakpoint(int thread) = 0;
    /// Hands the thread's watch set to its CPU (nullptr: none).
    virtual void attachWatches(int thread, WatchSet* watches) = 0;
    /// Runs the machine; reports a PC breakpoint the machine latched.
    virtual Stop runMachine(uint64_t budget) = 0;
    virtual Stop stepMachine() = 0;
    /// A Stop for a pending watch hit of `thread`'s CPU, or none.
    Stop watchStop(int thread);
    /// The first thread with a pending watch hit, or 0.
    int watchHitThread() const;

private:
    void prepareResume();
    std::vector<std::vector<uint16_t>> m_breakpoints; // per thread id - 1
    std::map<int, WatchSet> m_watches;                // per thread id; nodes stay put for the CPUs' pointers
};

/// Flag bit names of the status register, least significant first:
/// LH580x T = c ie z v h; Z-80 F = c n pv - h - z s (an empty name marks an
/// undocumented bit).
const std::vector<std::string>& flagNames(CpuKind kind);

} // namespace debug
