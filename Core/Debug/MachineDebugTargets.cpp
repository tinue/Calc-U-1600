#include "MachineDebugTargets.hpp"

#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "CpuRegisters.hpp"

namespace debug {

namespace {

void setBreakpointFlag(LH5801& cpu, bool on) {
    const uint32_t f = cpu.traceFlags();
    cpu.setTraceFlags(on ? (f | TRACE_BREAKPOINTS) : (f & ~uint32_t(TRACE_BREAKPOINTS)));
}

void setBreakpointFlag(SC7852& cpu, bool on) {
    const uint32_t f = cpu.traceFlags();
    cpu.setTraceFlags(on ? (f | TRACE_BREAKPOINTS) : (f & ~uint32_t(TRACE_BREAKPOINTS)));
}

template <typename Cpu>
void loadBreakpoints(Cpu& cpu, const std::vector<uint16_t>& addrs) {
    cpu.clearBreakpoints();
    for (uint16_t a : addrs) cpu.addBreakpoint(a);
}

} // namespace

// ── PC-1500 ───────────────────────────────────────────────────────────────

PC1500DebugTarget::~PC1500DebugTarget() {
    m_machine.setWatches(nullptr);
    m_machine.cpu().clearBreakpoints();
    setBreakpointFlag(m_machine.cpu(), false);
}

std::vector<Thread> PC1500DebugTarget::threads() const {
    return {{1, CpuKind::LH5801, "LH5801"}};
}

std::vector<Register> PC1500DebugTarget::registers(int) const { return lhRegisters(m_machine.cpu()); }

bool PC1500DebugTarget::readRegister(int, const std::string& name, uint32_t* value) const {
    return lhReadRegister(m_machine.cpu(), name, value);
}

bool PC1500DebugTarget::writeRegister(int, const std::string& name, uint32_t value) {
    const bool ok = lhWriteRegister(m_machine.cpu(), name, value);
    // PU/PV select memory banks; the bus must see an edited value at once.
    if (ok) m_machine.memory().updatePUPV(m_machine.cpu().pu(), m_machine.cpu().pv());
    return ok;
}

uint16_t PC1500DebugTarget::pc(int) const { return m_machine.cpu().pc(); }
uint16_t PC1500DebugTarget::sp(int) const { return m_machine.cpu().sp(); }

bool PC1500DebugTarget::peek(int, Space space, uint16_t addr, uint8_t* value) const {
    if (space == kSpaceMain) { *value = m_machine.memory().peek(addr); return true; }
    bool readable = false;
    *value = m_machine.memory().debugPeekME1(addr, &readable);
    return readable;
}

bool PC1500DebugTarget::poke(int, Space space, uint16_t addr, uint8_t value) {
    if (space != kSpaceMain) return false; // ME1 is I/O on the PC-1500
    return m_machine.memory().poke(addr, value);
}

bool PC1500DebugTarget::pu(int) const { return m_machine.cpu().pu(); }
bool PC1500DebugTarget::pv(int) const { return m_machine.cpu().pv(); }
uint32_t PC1500DebugTarget::historySize(int) const { return m_machine.cpu().history().size(); }
HistoryEntry PC1500DebugTarget::history(int, uint32_t age) const {
    return lhHistoryEntry(m_machine.cpu().history().recent(age));
}
uint32_t PC1500DebugTarget::retired(int) const { return m_machine.cpu().history().total(); }

void PC1500DebugTarget::reset(bool allReset) {
    if (allReset) m_machine.allReset();
    else m_machine.reset();
}

bool PC1500DebugTarget::isHalted(int) const { return m_machine.cpu().halted() || m_machine.cpu().poweredOff(); }

void PC1500DebugTarget::applyBreakpoints(int, const std::vector<uint16_t>& addrs) { loadBreakpoints(m_machine.cpu(), addrs); }
void PC1500DebugTarget::enableBreakpointChecks(bool on) { setBreakpointFlag(m_machine.cpu(), on); }
void PC1500DebugTarget::resumePastBreakpoint(int) { m_machine.cpu().resumePastBreakpoint(); }
void PC1500DebugTarget::attachWatches(int, WatchSet* watches) { m_machine.setWatches(watches); }

Stop PC1500DebugTarget::latchedStop() {
    if (m_machine.consumeBreakpointHit()) {
        Stop s;
        s.kind = Stop::Breakpoint;
        s.thread = 1;
        return s;
    }
    return watchStop(1);
}

Stop PC1500DebugTarget::runMachine(uint64_t budget) {
    m_machine.runCycles(budget);
    return latchedStop();
}

Stop PC1500DebugTarget::stepMachine() {
    m_machine.step();
    return latchedStop();
}

// ── PC-1600 ───────────────────────────────────────────────────────────────

PC1600DebugTarget::~PC1600DebugTarget() {
    m_machine.setWatches(nullptr, nullptr);
    m_machine.sc7852().clearBreakpoints();
    m_machine.lh5803().clearBreakpoints();
    setBreakpointFlag(m_machine.sc7852(), false);
    setBreakpointFlag(m_machine.lh5803(), false);
}

std::vector<Thread> PC1600DebugTarget::threads() const {
    return {{kZ80, CpuKind::Z80, "Z80 (SC7852)"}, {kLh5803, CpuKind::LH5803, "LH5803"}};
}

int PC1600DebugTarget::busOwner() const { return m_machine.sc7852Owns() ? kZ80 : kLh5803; }

std::vector<Register> PC1600DebugTarget::registers(int thread) const {
    return thread == kZ80 ? z80Registers(m_machine.sc7852()) : lhRegisters(m_machine.lh5803());
}

bool PC1600DebugTarget::readRegister(int thread, const std::string& name, uint32_t* value) const {
    return thread == kZ80 ? z80ReadRegister(m_machine.sc7852(), name, value)
                          : lhReadRegister(m_machine.lh5803(), name, value);
}

bool PC1600DebugTarget::writeRegister(int thread, const std::string& name, uint32_t value) {
    if (thread == kZ80) return z80WriteRegister(m_machine.sc7852(), name, value);
    const bool ok = lhWriteRegister(m_machine.lh5803(), name, value);
    if (ok) m_machine.lh5803Memory().updatePUPV(m_machine.lh5803().pu(), m_machine.lh5803().pv());
    return ok;
}

uint16_t PC1600DebugTarget::pc(int thread) const {
    return thread == kZ80 ? m_machine.sc7852().pc() : m_machine.lh5803().pc();
}

uint16_t PC1600DebugTarget::sp(int thread) const {
    return thread == kZ80 ? m_machine.sc7852().sp() : m_machine.lh5803().sp();
}

bool PC1600DebugTarget::peek(int thread, Space space, uint16_t addr, uint8_t* value) const {
    if (thread == kZ80) { *value = m_machine.memory().peek(addr); return true; }
    bool readable = false;
    *value = m_machine.lh5803Memory().debugPeek(addr, space == kSpaceME1, &readable);
    return readable;
}

bool PC1600DebugTarget::poke(int thread, Space space, uint16_t addr, uint8_t value) {
    if (thread == kZ80) return m_machine.pokeMemory(addr, &value, 1);
    // The LH5803's ME0 0000-7FFF is the Z-80's 8000-FFFF; the rest is ROM
    // or I/O.
    if (space != kSpaceMain || addr >= 0x8000) return false;
    return m_machine.pokeMemory(uint16_t(addr + 0x8000), &value, 1);
}

int PC1600DebugTarget::bankAt(int thread, uint16_t addr) const {
    if (thread != kZ80) return -1;
    const PC1600Machine::DebugBankState b = m_machine.debugBankState();
    const uint8_t banks[4] = {b.pageABank, b.pageBBank, b.pageCBank, b.pageDBank};
    return banks[addr >> 14];
}

bool PC1600DebugTarget::pu(int thread) const { return thread != kZ80 && m_machine.lh5803().pu(); }
bool PC1600DebugTarget::pv(int thread) const { return thread != kZ80 && m_machine.lh5803().pv(); }

uint32_t PC1600DebugTarget::historySize(int thread) const {
    return thread == kZ80 ? m_machine.sc7852().history().size() : m_machine.lh5803().history().size();
}

HistoryEntry PC1600DebugTarget::history(int thread, uint32_t age) const {
    return thread == kZ80 ? z80HistoryEntry(m_machine.sc7852().history().recent(age))
                          : lhHistoryEntry(m_machine.lh5803().history().recent(age));
}

uint32_t PC1600DebugTarget::retired(int thread) const {
    return thread == kZ80 ? m_machine.sc7852().history().total() : m_machine.lh5803().history().total();
}

void PC1600DebugTarget::reset(bool allReset) {
    if (allReset) m_machine.allReset();
    else m_machine.reset();
}

bool PC1600DebugTarget::isHalted(int thread) const {
    return thread == kZ80 ? m_machine.sc7852().halted() : (m_machine.lh5803().halted() || m_machine.lh5803().poweredOff());
}

void PC1600DebugTarget::applyBreakpoints(int thread, const std::vector<uint16_t>& addrs) {
    if (thread == kZ80) loadBreakpoints(m_machine.sc7852(), addrs);
    else loadBreakpoints(m_machine.lh5803(), addrs);
}

void PC1600DebugTarget::enableBreakpointChecks(bool on) {
    setBreakpointFlag(m_machine.sc7852(), on);
    setBreakpointFlag(m_machine.lh5803(), on);
}

void PC1600DebugTarget::resumePastBreakpoint(int thread) {
    if (thread == kZ80) m_machine.sc7852().resumePastBreakpoint();
    else m_machine.lh5803().resumePastBreakpoint();
}

void PC1600DebugTarget::attachWatches(int thread, WatchSet* watches) {
    (thread == kZ80 ? m_z80Watches : m_lh5803Watches) = watches;
    m_machine.setWatches(m_z80Watches, m_lh5803Watches);
}

Stop PC1600DebugTarget::latchedStop() {
    Stop s;
    switch (m_machine.consumeDebugStop()) {
        case PC1600Machine::DebugStop::Z80Breakpoint:    s.kind = Stop::Breakpoint; s.thread = kZ80; return s;
        case PC1600Machine::DebugStop::Lh5803Breakpoint: s.kind = Stop::Breakpoint; s.thread = kLh5803; return s;
        case PC1600Machine::DebugStop::Z80Watch:         return watchStop(kZ80);
        case PC1600Machine::DebugStop::Lh5803Watch:      return watchStop(kLh5803);
        case PC1600Machine::DebugStop::None: break;
    }
    // step() doesn't latch watch hits itself.
    if (const int thread = watchHitThread()) return watchStop(thread);
    return s;
}

Stop PC1600DebugTarget::runMachine(uint64_t budget) {
    m_machine.runCycles(budget);
    return latchedStop();
}

Stop PC1600DebugTarget::stepMachine() {
    m_machine.step();
    return latchedStop();
}

} // namespace debug
