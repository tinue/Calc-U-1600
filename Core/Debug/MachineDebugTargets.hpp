#pragma once

#include <memory>

#include "DebugTarget.hpp"

class PC1500Machine;
class PC1600Machine;

// DebugTarget adapters for the two machines. Thread 1 is the machine's main
// CPU (LH5801 / SC7852); the PC-1600's LH5803 is thread 2. They drive the
// machine through its normal step()/runCycles() and hold no machine state
// of their own besides the breakpoint lists and watches; a new machine
// needs a new target.
namespace debug {

/// What both machines share: run/step with the machine's debug-stop
/// latch, reset, per-CPU watches, and taking everything off the CPUs
/// when the target goes.
template <typename Machine>
class MachineDebugTarget : public DebugTarget {
public:
    ~MachineDebugTarget() override;

    Stop runMachine(uint64_t budget) override;
    Stop stepMachine() override;
    void reset(bool allReset) override;

protected:
    explicit MachineDebugTarget(Machine& machine) : m_machine(machine) {}
    void attachWatches(int thread, WatchSet* watches) override;
    DebugStop consumeMachineStop() override;

    Machine& m_machine;
};

extern template class MachineDebugTarget<PC1500Machine>;
extern template class MachineDebugTarget<PC1600Machine>;

class PC1500DebugTarget final : public MachineDebugTarget<PC1500Machine> {
public:
    explicit PC1500DebugTarget(PC1500Machine& machine);

    bool peek(int thread, Space space, uint16_t addr, uint8_t* value) const override;
    bool poke(int thread, Space space, uint16_t addr, uint8_t value) override;
};

class PC1600DebugTarget final : public MachineDebugTarget<PC1600Machine> {
public:
    static constexpr int kZ80 = 1;
    static constexpr int kLh5803 = 2;

    explicit PC1600DebugTarget(PC1600Machine& machine);

    int busOwner() const override;
    bool peek(int thread, Space space, uint16_t addr, uint8_t* value) const override;
    bool poke(int thread, Space space, uint16_t addr, uint8_t value) override;
    int bankAt(int thread, uint16_t addr) const override;
    std::vector<BankField> bankState(int thread) const override;
};

/// The target for a machine (for callers that visit either one).
std::unique_ptr<DebugTarget> makeDebugTarget(PC1500Machine& machine);
std::unique_ptr<DebugTarget> makeDebugTarget(PC1600Machine& machine);

} // namespace debug
