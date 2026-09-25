#pragma once

#include "DebugTarget.hpp"

class PC1500Machine;
class PC1600Machine;

// DebugTarget adapters for the two machines. Thread 1 is the machine's main
// CPU (LH5801 / SC7852); the PC-1600's LH5803 is thread 2. They drive the
// machine through its normal step()/runCycles() and hold no machine state
// of their own besides the breakpoint lists and watches; a new machine
// needs a new target.
namespace debug {

class PC1500DebugTarget final : public DebugTarget {
public:
    explicit PC1500DebugTarget(PC1500Machine& machine) : m_machine(machine) {}
    ~PC1500DebugTarget() override;

    std::vector<Thread> threads() const override;
    int busOwner() const override { return 1; }
    CpuKind kindOf(int) const override { return CpuKind::LH5801; }
    std::vector<Register> registers(int thread) const override;
    bool readRegister(int thread, const std::string& name, uint32_t* value) const override;
    bool writeRegister(int thread, const std::string& name, uint32_t value) override;
    uint16_t pc(int thread) const override;
    uint16_t sp(int thread) const override;
    bool peek(int thread, Space space, uint16_t addr, uint8_t* value) const override;
    bool poke(int thread, Space space, uint16_t addr, uint8_t value) override;
    int bankAt(int, uint16_t) const override { return -1; }
    bool pu(int thread) const override;
    bool pv(int thread) const override;
    uint32_t historySize(int thread) const override;
    HistoryEntry history(int thread, uint32_t age) const override;
    uint32_t retired(int thread) const override;
    void reset(bool allReset) override;

protected:
    bool isHalted(int thread) const override;
    void applyBreakpoints(int thread, const std::vector<uint16_t>& addrs) override;
    void enableBreakpointChecks(bool on) override;
    void resumePastBreakpoint(int thread) override;
    void attachWatches(int thread, WatchSet* watches) override;
    Stop runMachine(uint64_t budget) override;
    Stop stepMachine() override;

private:
    Stop latchedStop();
    PC1500Machine& m_machine;
};

class PC1600DebugTarget final : public DebugTarget {
public:
    static constexpr int kZ80 = 1;
    static constexpr int kLh5803 = 2;

    explicit PC1600DebugTarget(PC1600Machine& machine) : m_machine(machine) {}
    ~PC1600DebugTarget() override;

    std::vector<Thread> threads() const override;
    int busOwner() const override;
    CpuKind kindOf(int thread) const override { return thread == kZ80 ? CpuKind::Z80 : CpuKind::LH5803; }
    std::vector<Register> registers(int thread) const override;
    bool readRegister(int thread, const std::string& name, uint32_t* value) const override;
    bool writeRegister(int thread, const std::string& name, uint32_t value) override;
    uint16_t pc(int thread) const override;
    uint16_t sp(int thread) const override;
    bool peek(int thread, Space space, uint16_t addr, uint8_t* value) const override;
    bool poke(int thread, Space space, uint16_t addr, uint8_t value) override;
    int bankAt(int thread, uint16_t addr) const override;
    bool pu(int thread) const override;
    bool pv(int thread) const override;
    uint32_t historySize(int thread) const override;
    HistoryEntry history(int thread, uint32_t age) const override;
    uint32_t retired(int thread) const override;
    void reset(bool allReset) override;

protected:
    bool isHalted(int thread) const override;
    void applyBreakpoints(int thread, const std::vector<uint16_t>& addrs) override;
    void enableBreakpointChecks(bool on) override;
    void resumePastBreakpoint(int thread) override;
    void attachWatches(int thread, WatchSet* watches) override;
    Stop runMachine(uint64_t budget) override;
    Stop stepMachine() override;

private:
    Stop latchedStop();
    WatchSet* m_z80Watches = nullptr;
    WatchSet* m_lh5803Watches = nullptr;
    PC1600Machine& m_machine;
};

} // namespace debug
