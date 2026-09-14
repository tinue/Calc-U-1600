#pragma once

// ── ELH# CPU-ownership handoff ───────────────────────────────────────────
//
// Tracks which of the two PC-1600 CPUs currently owns the shared bus
// (`ELH#` high = SC7852 running, the reset default; low =
// LH5803 running; never both), per
// SharpPC1500Reference/PC-1600/PC-1600-Machine-Overview.md §3.
//
// This class only tracks state and pending-switch requests -- it does not
// itself drive either CPU. PC1600Machine::step() is what actually
// dispatches to whichever CPU currently owns the bus and, after stepping
// it, asks this arbiter whether a switch was requested and (for the
// SC7852 direction only) whether the requesting CPU has also reached the
// HALT that makes the handoff complete -- see the two request methods'
// own comments for the asymmetry between the two directions.
class PC1600BusArbiter {
public:
    /// True if SC7852 currently owns the bus (ELH# high, the reset
    /// default); false if LH5803 does.
    bool sc7852Owns() const { return m_sc7852Owns; }

    /// Called by PC1600Memory::writeIO() when the Z-80 side writes Port
    /// 38H (`OUT (38H),A`) -- the first half of the documented two-
    /// instruction SC7852→LH5803 handoff sequence. Only sets a pending
    /// flag; PC1600Machine::step() completes the actual switch once the
    /// SC7852 subsequently executes the `HALT` that always follows this
    /// write in the documented sequence (see that file's `step()`) --
    /// modeling the OUT-then-HALT pair as two separate, observable steps
    /// keeps the handoff steppable in a debugger.
    void requestSwitchFromSC7852() { m_switchRequestedBySC7852 = true; }

    /// Called by LH5803SharedMemory::writeME1() when the LH5803 side
    /// stores to ME1 address 0xA038 (`STA #(0A038H)`) -- the LH5803-side
    /// alias of the same handoff trigger. Unlike the SC7852 direction,
    /// the documented sequence has no following HALT, so this switch is
    /// completed immediately by PC1600Machine::step() once the LH5803's
    /// step() call that performed the store returns.
    void requestSwitchFromLH5803() { m_switchRequestedByLH5803 = true; }

    /// Non-consuming: PC1600Machine::step() needs to check this on every
    /// SC7852 step() call until the CPU actually reaches HALT (a separate
    /// step() call, since OUT and HALT are two distinct instructions) --
    /// consuming it early, before the switch is actually performed, would
    /// lose the request. Cleared only when switchToLH5803() actually
    /// performs the switch.
    bool switchRequestedBySC7852() const { return m_switchRequestedBySC7852; }
    /// Non-consuming, same rationale, though in practice the LH5803
    /// direction has no following HALT so PC1600Machine::step() acts on
    /// this within the same step() call that set it.
    bool switchRequestedByLH5803() const { return m_switchRequestedByLH5803; }

    void switchToLH5803() { m_sc7852Owns = false; m_switchRequestedBySC7852 = false; }
    void switchToSC7852() { m_sc7852Owns = true; m_switchRequestedByLH5803 = false; }

    /// SC7852 always starts first after reset (architecturally guaranteed
    /// per PC-1600-Machine-Overview.md §6, not configurable).
    void reset() {
        m_sc7852Owns = true;
        m_switchRequestedBySC7852 = false;
        m_switchRequestedByLH5803 = false;
    }

private:
    bool m_sc7852Owns{true};
    bool m_switchRequestedBySC7852{false};
    bool m_switchRequestedByLH5803{false};
};
