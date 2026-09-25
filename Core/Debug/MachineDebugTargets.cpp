#include "MachineDebugTargets.hpp"

#include <memory>

#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "CpuViews.hpp"

namespace debug {

// ── Shared ────────────────────────────────────────────────────────────────

template <typename Machine>
MachineDebugTarget<Machine>::~MachineDebugTarget() {
    detachAll();
}

template <typename Machine>
Stop MachineDebugTarget<Machine>::runMachine(uint64_t budget) {
    const uint64_t cycles = m_machine.runCycles(budget);
    Stop s = stopFor(m_machine.consumeDebugStop());
    s.cycles = cycles;
    return s;
}

template <typename Machine>
Stop MachineDebugTarget<Machine>::stepMachine() {
    m_machine.step();
    return stopFor(m_machine.consumeDebugStop());
}

template <typename Machine>
void MachineDebugTarget<Machine>::reset(bool allReset) {
    if (allReset) m_machine.allReset();
    else m_machine.reset();
}

template <typename Machine>
void MachineDebugTarget<Machine>::attachWatches(int thread, WatchSet* watches) {
    // Thread ids beyond the CPUs name the last one (see DebugTarget).
    m_machine.setWatches(thread == 1 ? 1 : int(threads().size()), watches);
}

template <typename Machine>
DebugStop MachineDebugTarget<Machine>::consumeMachineStop() {
    return m_machine.consumeDebugStop();
}

template class MachineDebugTarget<PC1500Machine>;
template class MachineDebugTarget<PC1600Machine>;

// ── PC-1500 ───────────────────────────────────────────────────────────────

PC1500DebugTarget::PC1500DebugTarget(PC1500Machine& machine) : MachineDebugTarget(machine) {
    // PU/PV select memory banks; the bus must see an edited value at once.
    addCpu(std::make_unique<LhCpuView>(machine.cpu(), CpuKind::LH5801, "LH5801",
                                       [&machine](bool pu, bool pv) { machine.memory().updatePUPV(pu, pv); }));
}

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

// ── PC-1600 ───────────────────────────────────────────────────────────────

PC1600DebugTarget::PC1600DebugTarget(PC1600Machine& machine) : MachineDebugTarget(machine) {
    addCpu(std::make_unique<Z80CpuView>(machine.sc7852()));
    // PV gates the CE-150 ROM window on the LH5803's bus.
    addCpu(std::make_unique<LhCpuView>(machine.lh5803(), CpuKind::LH5803, "LH5803",
                                       [&machine](bool pu, bool pv) { machine.lh5803Memory().updatePUPV(pu, pv); }));
}

int PC1600DebugTarget::busOwner() const { return m_machine.sc7852Owns() ? kZ80 : kLh5803; }

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
    uint16_t z80 = 0;
    if (space != kSpaceMain || !LH5803SharedMemory::toZ80Address(addr, &z80)) return false;
    return m_machine.pokeMemory(z80, &value, 1);
}

int PC1600DebugTarget::bankAt(int thread, uint16_t addr) const {
    if (thread != kZ80) return -1;
    const PC1600Bank& b = m_machine.bank();
    switch (addr >> 14) {
        case 0: return b.pageABank();
        case 1: return b.pageBBank();
        case 2: return b.pageCBank();
        default: return b.pageDBank();
    }
}

std::vector<BankField> PC1600DebugTarget::bankState(int thread) const {
    if (thread != kZ80) return DebugTarget::bankState(thread);
    static const char* const kPages[4] = {"Page A (0000-3FFF)", "Page B (4000-7FFF)", "Page C (8000-BFFF)",
                                          "Page D (C000-FFFF)"};
    std::vector<BankField> list;
    for (int p = 0; p < 4; p++) list.push_back({kPages[p], "bank " + std::to_string(bankAt(thread, uint16_t(p << 14)))});
    return list;
}

} // namespace debug
