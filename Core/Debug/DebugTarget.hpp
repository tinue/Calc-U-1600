#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../CPU/DebugStop.hpp"
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
    uint64_t cycles = 0;  ///< run(): machine cycles consumed
};

class CpuView;

/// The machine-wide part of a debug target; each CPU is a CpuView
/// (CpuViews.hpp), thread 1 the first. A thread id other than 1 names the
/// last CPU.
class DebugTarget {
public:
    virtual ~DebugTarget();

    std::vector<Thread> threads() const;
    /// The thread whose CPU currently owns the bus (executes on step()).
    virtual int busOwner() const { return 1; }
    CpuKind kindOf(int thread) const;

    // ── Registers ─────────────────────────────────────────────────────────
    /// The CPU's registers, in display order; the status register (T / F)
    /// is one entry, its bits named by flagNames().
    std::vector<Register> registers(int thread) const;
    /// Any name registers() lists, plus the 8-bit halves (Z-80 a/f/b/c/...,
    /// LH580x xl/xh/...) and single flags as <flag>f ("cf", "zf", "ief").
    /// False for an unknown name.
    bool readRegister(int thread, const std::string& name, uint32_t* value) const;
    bool writeRegister(int thread, const std::string& name, uint32_t value);
    uint16_t pc(int thread) const;
    uint16_t sp(int thread) const;
    bool halted(int thread) const;

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
    /// Decodes a history entry from its recorded bytes (the instruction as
    /// it ran, even if memory has changed since).
    disasm::Decoded decode(int thread, const HistoryEntry& entry, const disasm::SymbolFn& symbols = {}) const;
    /// Bank qualifiers of the code at `addr` right now: the PC-1600 page
    /// bank (0-7) for the Z-80, -1 elsewhere; PU/PV for the LH580x.
    virtual int bankAt(int, uint16_t) const { return -1; }
    bool pu(int thread) const;
    bool pv(int thread) const;
    /// Whether code at `addr` on `thread` currently satisfies `key`.
    bool bankMatches(int thread, const BankKey& key, uint16_t addr) const;

    // ── History ───────────────────────────────────────────────────────────
    uint32_t historySize(int thread) const;
    HistoryEntry history(int thread, uint32_t age) const;
    /// Instructions retired so far (grows by one per instruction; reset
    /// by a machine reset). Used to tell that a CPU has moved.
    uint32_t retired(int thread) const;

    // ── Breakpoints and watches ───────────────────────────────────────────
    /// Replaces the thread's PC breakpoints. Checking is enabled only while
    /// some CPU has at least one (see breakpointsActive()).
    void setBreakpoints(int thread, const std::vector<uint16_t>& addrs);
    const std::vector<uint16_t>& breakpoints(int thread) const;
    bool breakpointsActive() const;
    /// Replaces the thread's memory watches (in its own address space);
    /// empty turns its checking off.
    void setWatches(int thread, const std::vector<WatchSet::Watch>& watches);
    /// Armed (the default), breakpoints and watches act on the CPUs;
    /// disarmed, the lists are kept but the CPUs don't check them -- for
    /// whenever something other than the debugger drives the machine (a
    /// boot to the prompt, a program load), which must not park on one.
    void arm(bool on);

    // ── Execution ─────────────────────────────────────────────────────────
    /// Free run for up to `budget` machine cycles (the machine's own
    /// runCycles() unit); reports a breakpoint or watch the machine latched.
    virtual Stop runMachine(uint64_t budget) = 0;
    /// One machine step: the bus owner executes one instruction (or idles
    /// one halted tick).
    virtual Stop stepMachine() = 0;
    /// Both stop again on a breakpoint a CPU sits on unless this let it go
    /// first (the caller -- RunControl -- manages resumes).
    void resumeFromStop();

    /// Machine reset / all reset (RAM cleared first).
    virtual void reset(bool allReset) = 0;

protected:
    /// The machine target's constructor adds its CPUs in thread order.
    void addCpu(std::unique_ptr<CpuView> cpu);
    CpuView& view(int thread) const;
    /// Takes every CPU's breakpoints and watches off (the machine target's
    /// destructor, while the machine is still there).
    void detachAll();
    /// Hands the thread's watch set to its CPU (nullptr: none).
    virtual void attachWatches(int thread, WatchSet* watches) = 0;
    /// Consumes the machine's latched debugger stop (see DebugStop).
    virtual DebugStop consumeMachineStop() = 0;
    /// The Stop for a latched machine stop: a breakpoint as is, a watch
    /// with its hit (consumed from the CPU's watch set).
    Stop stopFor(const DebugStop& stop);

private:
    void enableBreakpointChecks(bool on);

    std::vector<std::unique_ptr<CpuView>> m_cpus;     // thread id - 1
    std::vector<std::vector<uint16_t>> m_breakpoints; // per thread id - 1
    std::map<int, WatchSet> m_watches;                // per thread id; nodes stay put for the CPUs' pointers
    bool m_armed = true;
};

/// Flag bit names of the status register, least significant first:
/// LH580x T = c ie z v h; Z-80 F = c n pv - h - z s (an empty name marks an
/// undocumented bit).
const std::vector<std::string>& flagNames(CpuKind kind);

} // namespace debug
