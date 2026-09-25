#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../CPU/LH5801/LH5801.hpp"
#include "../CPU/SC7852/SC7852.hpp"
#include "CpuRegisters.hpp"
#include "DebugTarget.hpp"

// ── One CPU as the debugger sees it ──────────────────────────────────────
//
// Everything DebugTarget needs per CPU: registers, history, halt state and
// PC breakpoints. The machine targets hold one view per thread and add
// only what is machine-wide (bus owner, memory mapping, banks, run/step).

namespace debug {

class CpuView {
public:
    virtual ~CpuView() = default;

    virtual CpuKind kind() const = 0;
    virtual const char* name() const = 0;

    virtual std::vector<Register> registers() const = 0;
    virtual bool readRegister(const std::string& name, uint32_t* value) const = 0;
    virtual bool writeRegister(const std::string& name, uint32_t value) = 0;
    virtual uint16_t pc() const = 0;
    virtual uint16_t sp() const = 0;
    virtual bool pu() const { return false; }
    virtual bool pv() const { return false; }
    /// Stopped until an interrupt (or, on the LH580x, powered off).
    virtual bool halted() const = 0;

    virtual uint32_t historySize() const = 0;
    virtual HistoryEntry history(uint32_t age) const = 0;
    virtual uint32_t retired() const = 0;

    virtual void setBreakpoints(const std::vector<uint16_t>& addrs) = 0;
    virtual void enableBreakpoints(bool on) = 0;
    virtual void resumePastBreakpoint() = 0;
};

namespace detail {

template <typename Cpu>
void loadBreakpoints(Cpu& cpu, const std::vector<uint16_t>& addrs) {
    cpu.clearBreakpoints();
    for (uint16_t a : addrs) cpu.addBreakpoint(a);
}

} // namespace detail

/// An LH5801 (PC-1500) or LH5803 (PC-1600). `pupvChanged` pushes an edited
/// PU/PV to the memory map: those flags select banks, and the bus must see
/// the new value at once.
class LhCpuView final : public CpuView {
public:
    LhCpuView(LH5801& cpu, CpuKind kind, const char* name, std::function<void(bool pu, bool pv)> pupvChanged)
        : m_cpu(cpu), m_kind(kind), m_name(name), m_pupvChanged(std::move(pupvChanged)) {}

    CpuKind kind() const override { return m_kind; }
    const char* name() const override { return m_name; }
    std::vector<Register> registers() const override { return lhRegisters(m_cpu); }
    bool readRegister(const std::string& name, uint32_t* value) const override {
        return lhReadRegister(m_cpu, name, value);
    }
    bool writeRegister(const std::string& name, uint32_t value) override {
        const bool ok = lhWriteRegister(m_cpu, name, value);
        if (ok && m_pupvChanged) m_pupvChanged(m_cpu.pu(), m_cpu.pv());
        return ok;
    }
    uint16_t pc() const override { return m_cpu.pc(); }
    uint16_t sp() const override { return m_cpu.sp(); }
    bool pu() const override { return m_cpu.pu(); }
    bool pv() const override { return m_cpu.pv(); }
    bool halted() const override { return m_cpu.halted() || m_cpu.poweredOff(); }
    uint32_t historySize() const override { return m_cpu.history().size(); }
    HistoryEntry history(uint32_t age) const override { return lhHistoryEntry(m_cpu.history().recent(age)); }
    uint32_t retired() const override { return m_cpu.history().total(); }
    void setBreakpoints(const std::vector<uint16_t>& addrs) override { detail::loadBreakpoints(m_cpu, addrs); }
    void enableBreakpoints(bool on) override { m_cpu.setBreakpointsEnabled(on); }
    void resumePastBreakpoint() override { m_cpu.resumePastBreakpoint(); }

private:
    LH5801& m_cpu;
    CpuKind m_kind;
    const char* m_name;
    std::function<void(bool, bool)> m_pupvChanged;
};

/// The PC-1600's SC7852 (Z-80).
class Z80CpuView final : public CpuView {
public:
    explicit Z80CpuView(SC7852& cpu) : m_cpu(cpu) {}

    CpuKind kind() const override { return CpuKind::Z80; }
    const char* name() const override { return "Z80 (SC7852)"; }
    std::vector<Register> registers() const override { return z80Registers(m_cpu); }
    bool readRegister(const std::string& name, uint32_t* value) const override {
        return z80ReadRegister(m_cpu, name, value);
    }
    bool writeRegister(const std::string& name, uint32_t value) override { return z80WriteRegister(m_cpu, name, value); }
    uint16_t pc() const override { return m_cpu.pc(); }
    uint16_t sp() const override { return m_cpu.sp(); }
    bool halted() const override { return m_cpu.halted(); }
    uint32_t historySize() const override { return m_cpu.history().size(); }
    HistoryEntry history(uint32_t age) const override { return z80HistoryEntry(m_cpu.history().recent(age)); }
    uint32_t retired() const override { return m_cpu.history().total(); }
    void setBreakpoints(const std::vector<uint16_t>& addrs) override { detail::loadBreakpoints(m_cpu, addrs); }
    void enableBreakpoints(bool on) override { m_cpu.setBreakpointsEnabled(on); }
    void resumePastBreakpoint() override { m_cpu.resumePastBreakpoint(); }

private:
    SC7852& m_cpu;
};

} // namespace debug
