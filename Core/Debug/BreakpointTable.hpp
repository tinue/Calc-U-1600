#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DebugTarget.hpp"
#include "SourceMap.hpp"

// ── Breakpoint bookkeeping ───────────────────────────────────────────────
//
// Everything the DAP client set -- source-line, instruction, function
// (symbol) and data breakpoints, each with an optional condition, hit
// condition and (not data) log message -- and what they mean for the
// CPUs: which PC addresses to arm and which memory to watch, and whether
// a raw hit really stops. The CPUs only know addresses; conditions, hit
// counts, bank qualifiers and logpoints are decided here when one fires.

namespace debug {

struct BreakpointSpec {
    std::string condition;     ///< expression; empty = always
    std::string hitCondition;  ///< "5", "== 5", ">= 5", "% 3"; empty = always
    std::string logMessage;    ///< non-empty: a logpoint (never stops); {expr} is interpolated
};

/// A parsed hit condition: "5" / "== 5", ">= 5", "% 3", ... An unreadable
/// one is `valid == false` and always met, so it doesn't hide the
/// breakpoint.
struct HitCondition {
    enum Op : uint8_t { Always, Eq, Ne, Ge, Le, Gt, Lt, Mod };
    Op op = Always;
    long long n = 0;
    bool valid = true;

    static HitCondition parse(const std::string& text);
    /// `hits` counts this hit.
    bool met(int hits) const;
};

/// A log message split once into text and compiled {expr} parts.
class LogTemplate {
public:
    static LogTemplate parse(const std::string& message);
    /// Unevaluable parts become "{error}".
    std::string render(const ExpressionContext& ctx) const;

private:
    struct Part {
        std::string text;             ///< literal text, or the expression's source
        bool expression = false;
        CompiledExpression compiled;
    };
    std::vector<Part> m_parts;
};

/// What the client gets back for each breakpoint it set.
struct BreakpointStatus {
    int id = 0;
    bool verified = false;
    int line = 0;              ///< source breakpoints: the line it resolved to
    std::string message;       ///< why it's unverified, or which bank it's bound to
    bool hasAddress = false;
    int thread = 0;
    uint16_t addr = 0;
};

enum class DataAccess : uint8_t { Read, Write, ReadWrite };

struct DataBreakpointSpec : BreakpointSpec {
    int thread = 1;
    Space space = kSpaceMain;
    uint16_t addr = 0;
    uint16_t length = 1;
    DataAccess access = DataAccess::Write;
};

/// Result of a raw hit: stop or not, which breakpoints, logpoint output.
struct HitDecision {
    bool stop = false;
    bool entry = false;            ///< the one-shot entry breakpoint (reason "entry")
    std::vector<int> ids;
    std::vector<std::string> log;  ///< expanded log messages, in order
};

class BreakpointTable {
public:
    using SymbolLookup = std::function<bool(const std::string& name, int64_t* value)>;

    struct SourceRequest : BreakpointSpec { int line = 0; };
    struct InstructionRequest : BreakpointSpec { int thread = 1; uint16_t addr = 0; };
    struct FunctionRequest : BreakpointSpec { std::string name; };

    /// Replaces the breakpoints of one source file.
    std::vector<BreakpointStatus> setSource(const std::string& file, const std::vector<SourceRequest>& requests,
                                            const SourceMap& map);
    std::vector<BreakpointStatus> setInstructions(const std::vector<InstructionRequest>& requests);
    /// Function breakpoints resolve their name through the source map's
    /// symbols; each is armed on `thread` (the CPU its listing belongs to
    /// isn't known for a plain symbol, so the caller picks).
    std::vector<BreakpointStatus> setFunctions(const std::vector<FunctionRequest>& requests, const SourceMap& map,
                                               int thread);
    std::vector<BreakpointStatus> setData(const std::vector<DataBreakpointSpec>& requests);
    void clear();
    /// A one-shot stop at `addr` (Build & Load's stopOnEntry): it stops
    /// once, with reason "entry", then disappears.
    void setEntry(int thread, uint16_t addr);

    /// Re-resolves source and function breakpoints after the source map
    /// changed (a new load, a stale listing). Returns the ones whose status
    /// changed, for "breakpoint changed" events.
    std::vector<BreakpointStatus> reresolve(const SourceMap& map, int functionThread);

    /// Arms the PC breakpoints and memory watches on the target.
    void apply(DebugTarget& target) const;

    /// A CPU parked on `pc`: decides whether that is a stop.
    HitDecision onBreakpoint(int thread, uint16_t pc, DebugTarget& target, const SymbolLookup& symbols);
    /// A memory watch fired.
    HitDecision onWatch(int thread, const WatchHit& hit, DebugTarget& target, const SymbolLookup& symbols);

private:
    /// A spec's condition, hit condition and log message, parsed once when
    /// the breakpoint is set; shared by every address it arms.
    struct Compiled {
        bool hasCondition = false;
        CompiledExpression condition;
        HitCondition hit;
        bool hasLog = false;
        LogTemplate log;
    };
    static std::shared_ptr<const Compiled> compile(const BreakpointSpec& spec);

    struct Armed : BreakpointSpec {
        int id = 0;
        int thread = 1;
        uint16_t addr = 0;
        BankKey key;
        int hits = 0;
        std::shared_ptr<const Compiled> compiled;
    };
    struct Source {
        std::string file;
        std::vector<SourceRequest> requests;
        std::vector<int> ids; // one per request, stable across re-resolves
    };
    struct Function {
        FunctionRequest request;
        int id = 0;
    };
    struct Data : DataBreakpointSpec {
        int id = 0;
        int hits = 0;
        std::shared_ptr<const Compiled> compiled;
    };

    std::vector<BreakpointStatus> resolveSource(Source& s, const SourceMap& map);
    std::vector<BreakpointStatus> resolveFunctions(const SourceMap& map, int thread);
    void forgetStatus(int id);
    void passes(const BreakpointSpec& spec, const Compiled& compiled, int& hits, int thread, DebugTarget& target,
                const SymbolLookup& symbols, HitDecision* decision, int id);

    std::vector<Source> m_sources;
    std::vector<Armed> m_sourceArmed;       // from m_sources
    std::vector<Armed> m_instructionArmed;
    std::vector<Function> m_functions;
    std::vector<Armed> m_functionArmed;
    std::vector<Data> m_data;
    std::vector<BreakpointStatus> m_lastStatus; // source + function, for reresolve() diffs
    int m_nextId = 1;
    bool m_entryArmed = false;
    int m_entryThread = 1;
    uint16_t m_entryAddr = 0;
};

/// Hit-condition check: `hits` is the count including this hit.
bool hitConditionMet(const std::string& hitCondition, int hits, bool* valid = nullptr);
/// Expands {expr} in a log message; unevaluable parts become "{error}".
std::string interpolateLog(const std::string& message, const ExpressionContext& ctx);

} // namespace debug
