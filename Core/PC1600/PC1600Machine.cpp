#include "PC1600Machine.hpp"

#include <cstring>

PC1600Machine::PC1600Machine()
    : m_z80Mem(m_bank), m_sc7852(m_z80Mem), m_lh5803Mem(m_z80Mem), m_lh5803(m_lh5803Mem) {
    m_z80Mem.setBusArbiter(&m_arbiter);
    m_z80Mem.setCPU(&m_sc7852);
    m_lh5803Mem.setBusArbiter(&m_arbiter);
    m_lh5803.setCpuIdTag(CPU_ID_LH5803);
}

void PC1600Machine::resetLocked() {
    m_bank.reset();
    m_z80Mem.reset();
    m_z80Mem.keyboard().releaseAll();
    m_sc7852.reset();
    m_lh5803.reset();
    m_arbiter.reset();
    m_timer64Accum = 0;
    m_timer64State = false;
    m_timer64EdgeCount = 0;
    m_rtcAccum = 0; // the clock value itself survives reset (see seedClock())
    m_z80Mem.setTimer64Bit(false);
    m_lh5803Mem.reset();                  // clear the internal-PIO register file (0xF00x)
    m_lh5803Mem.updatePUPV(false, false); // match the just-reset LH5803 CPU
    if (m_ce150Card) m_ce150Card->reset(); // re-anchor, keep it attached (like the CE-1600P)
}

void PC1600Machine::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    resetLocked();
    // Tell the boot ROM this was the lighter reset level: it keeps the
    // calendar clock, the internal-RAM work area and every setting.
    m_z80Mem.subCpu().setResetCauseSimple();
}

void PC1600Machine::allReset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Two things make this the ALL RESET: the wiped RAM, and the reset
    // cause the sub-CPU reports. The ROM then runs full cold init --
    // clock back to 1 Jan 00:00:00 (the caller re-seeds it), work area
    // zeroed, settings to default.
    m_z80Mem.clearInternalRam();
    resetLocked();
    m_z80Mem.subCpu().setResetCauseAllReset();
}

void PC1600Machine::seedClock(int year, int month, int day, int hour, int minute, int second) {
    std::lock_guard<std::mutex> lock(m_mutex);
    PC1600SubCpu::DateTime dt;
    dt.month  = static_cast<uint8_t>(month); // plain 1-12, as the protocol carries it
    dt.day    = PC1600SubCpu::packBcd(day);
    dt.hour   = PC1600SubCpu::packBcd(hour);
    dt.minute = PC1600SubCpu::packBcd(minute);
    dt.second = PC1600SubCpu::packBcd(second);
    m_z80Mem.subCpu().setDateTime(dt);
    m_z80Mem.subCpu().setYear(year);
    // Boot re-inits the calendar to its cold-start default once; keep the
    // seeded time through that write.
    m_z80Mem.subCpu().armHostSeedGuard();
}

bool PC1600Machine::attachCE1600P(const uint8_t* rom1, size_t rom1Size,
                                   const uint8_t* rom2, size_t rom2Size) {
    if (rom1Size != CE1600PCard::kRomHalfSize || rom2Size != CE1600PCard::kRomHalfSize)
        return false;
    auto card = std::make_unique<CE1600PCard>();
    if (!card->loadRom(rom1, rom1Size, rom2, rom2Size)) return false;
    auto floppy = std::make_unique<CE1600FCard>();  // drive starts empty
    detachCE1600P();
    detachCE150(); // one plotter on the bus at a time
    m_z80Mem.ce1600pBus().attach(card.get());
    m_z80Mem.ce1600pBus().attach(floppy.get());
    m_ce1600pCard = std::move(card);
    m_ce1600fCard = std::move(floppy);
    return true;
}

void PC1600Machine::detachCE1600P() {
    if (m_ce1600fCard) {
        m_z80Mem.ce1600pBus().detach(m_ce1600fCard.get());
        m_ce1600fCard.reset();
    }
    if (!m_ce1600pCard) return;
    m_z80Mem.ce1600pBus().detach(m_ce1600pCard.get());
    m_ce1600pCard.reset();
}

// The three below take m_mutex so the GUI thread can read/clear the
// plotter mechanism while the emulation loop runs -- the mechanism's
// stroke and event containers are mutated from inside step() on every
// motor write, so an unlocked reader races a vector reallocation.

std::vector<AlpsPlotterMechanism::FlatPoint> PC1600Machine::ce1600pPlotPoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce1600pCard) return {};
    return m_ce1600pCard->mechanism().flatPoints();
}

uint64_t PC1600Machine::ce1600pPlotRevision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce1600pCard) return 0;
    return m_ce1600pCard->mechanism().revision();
}

std::vector<std::string> PC1600Machine::drainCE1600PEvents() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce1600pCard) return {};
    return m_ce1600pCard->mechanism().drainEvents();
}

void PC1600Machine::clearCE1600PPaper() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ce1600pCard) m_ce1600pCard->mechanism().clearPaper();
}

// ── CE-1600F floppy (union-attached with CE-1600P, above) ─────────────

std::vector<uint8_t> PC1600Machine::ce1600fDiskImage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce1600fCard) return {};
    return m_ce1600fCard->imageForSave();
}

uint64_t PC1600Machine::ce1600fRevision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ce1600fCard ? m_ce1600fCard->revision() : 0;
}

void PC1600Machine::ce1600fEject() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ce1600fCard) m_ce1600fCard->ejectDisk();
}

bool PC1600Machine::ce1600fHasDisk() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ce1600fCard && m_ce1600fCard->hasDisk();
}

bool PC1600Machine::ce1600fLoadImage(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce1600fCard) return false;
    return m_ce1600fCard->loadImage(data, size);
}

int PC1600Machine::ce1600fSide() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ce1600fCard ? m_ce1600fCard->side() : 0;
}

void PC1600Machine::ce1600fSetSide(int side) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ce1600fCard) m_ce1600fCard->setSide(side);
}

bool PC1600Machine::ce1600fMotorOn() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ce1600fCard && m_ce1600fCard->motorOn();
}

// ── CE-150 plotter (LH5803 side) ──────────────────────────────────────

bool PC1600Machine::attachCE150(const uint8_t* rom, size_t romSize) {
    if (romSize != Ce150Card::kRomSize) return false;
    auto card = std::make_unique<Ce150Card>();
    if (!card->loadRom(rom, romSize)) return false;
    detachCE150();
    detachCE1600P(); // one plotter on the bus at a time
    card->reset();
    m_lh5803Mem.attachCe150(card.get());
    m_ce150Card = std::move(card);
    return true;
}

void PC1600Machine::detachCE150() {
    if (!m_ce150Card) return;
    m_lh5803Mem.detachCe150();
    m_ce150Card.reset();
}

std::vector<AlpsPlotterMechanism::FlatPoint> PC1600Machine::ce150PlotPoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce150Card) return {};
    return m_ce150Card->mechanism().flatPoints();
}

uint64_t PC1600Machine::ce150PlotRevision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce150Card) return 0;
    return m_ce150Card->mechanism().revision();
}

std::vector<std::string> PC1600Machine::drainCE150Events() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce150Card) return {};
    return m_ce150Card->mechanism().drainEvents();
}

void PC1600Machine::clearCE150Paper() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ce150Card) m_ce150Card->mechanism().clearPaper();
}

int PC1600Machine::step() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_arbiter.sc7852Owns()) {
        int c = m_sc7852.step();
        // A halted SC7852::step() returns 0 (this core's convention for
        // "made no forward progress"), but real HALT still burns 4 T-states
        // per internal NOP cycle -- without crediting that, a CPU parked in
        // HALT would never accumulate enough T-states to see the timer's
        // falling edge and would wait forever for the very interrupt meant
        // to wake it: the boot reaches a genuine HALT waiting on this
        // interrupt, so the timer must keep advancing once it gets there.
        const int cost = (c > 0 ? c : SC7852::kHaltTickCycles);
        m_timer64Accum += cost * kTimer64AccumScale;
        while (m_timer64Accum >= kTimer64HalfPeriodScaled) {
            m_timer64Accum -= kTimer64HalfPeriodScaled;
            m_timer64State = !m_timer64State;
            m_z80Mem.setTimer64Bit(m_timer64State); // PB5 raw level only, see its own comment
            // TRM pin table (INT4, pin 83): "an interrupt is sent to the
            // CPU on a falling edge." The actual INT line only pulses once
            // per period, on the high->low transition, gated by whether
            // this cause is unmasked at port 35H
            // (PC-1600-CPU-SC7852-Z80.md §5.2's cause/mask pair) -- latches
            // the port 32H cause bit separately from PB5's raw level (see
            // latchTimer64InterruptCause()'s own comment for why: found by
            // disassembling the real ISR, which reads port 32H immediately
            // after being woken and requires the bit to still read as set
            // then, not already back to whatever the raw pulse is doing).
            if (!m_timer64State && m_z80Mem.timer64InterruptEnabled()) {
                m_z80Mem.latchTimer64InterruptCause();
                m_sc7852.requestInterrupt();
            }
            // The sub-CPU's own aggregated interrupt line (INT6, cause bit
            // 6), driven here by its 0.5s timer -- divided down from this
            // same 64 Hz signal, see kTimer64EdgesPerHalfSecond.
            if (++m_timer64EdgeCount == kTimer64EdgesPerHalfSecond) m_timer64EdgeCount = 0;
            if (m_timer64EdgeCount == kHalfSecondEdgePhase) {
                // The sub-CPU's 0.5 s signal, visible in bit 1 of request
                // 5DH, is a free-running level -- toggle it every period.
                // The file/RAM-disk IOCS readiness handshake polls 5DH and
                // waits for this to change.
                m_z80Mem.subCpu().toggleHalfSecondSignal();
                if (m_z80Mem.subCpuInterruptEnabled()) {
                    m_z80Mem.latchSubCpuInterruptCause();
                    m_sc7852.requestInterrupt();
                }
            }
        }
        // LU-57813P calendar clock: one tick per emulated second. See
        // kRtcPeriodTStates.
        m_rtcAccum += cost;
        m_z80Mem.advanceBuzzer(static_cast<uint32_t>(cost));
        while (m_rtcAccum >= kRtcPeriodTStates) {
            m_rtcAccum -= kRtcPeriodTStates;
            m_z80Mem.subCpu().tickOneSecond();
        }
        // TC8576F UART + the sub-CPU parallel-port BUSY window: both are
        // paced in SC-7852 T-states, the domain the firmware's status
        // polls observe them in.
        m_z80Mem.uart().tick(cost);
        m_z80Mem.subCpu().tickByTStates(cost);
        m_z80Mem.display().tick(cost);
        if (m_ce1600fCard) m_ce1600fCard->advance(static_cast<uint32_t>(cost));
        // The documented handoff is OUT (38H),A then HALT -- the write
        // sets the pending flag (PC1600Memory::writeIO), but the actual
        // switch only happens once the SC7852 has also reached HALT, so
        // the parked CPU is left in a stopped, resumable state (see
        // resumeFromHalt()) rather than switching mid-instruction-stream.
        if (m_arbiter.switchRequestedBySC7852() && m_sc7852.halted()) {
            m_arbiter.switchToLH5803();
        }
        maybeDrainTrace();
        return c;
    }
    int c = m_lh5803.step();
    // Push the LH5803's post-instruction PU/PV so the next LH5803-side bus
    // access sees it (PV gates the CE-150 ROM window). Mirrors
    // PC1500Machine::step()'s updatePUPV for the LH5801.
    m_lh5803Mem.updatePUPV(m_lh5803.pu(), m_lh5803.pv());
    // The LH-5803 also drives the UART / sub-CPU handshake (the OFF-path
    // clock save, rom1500 E538). Credit its cycles in T-states so the
    // BUSY window and any serial timing advance while it owns the bus
    // too.
    {
        const int tstates = static_cast<int>(
            toTStates(static_cast<uint64_t>(c > 0 ? c : 1), /*sc7852Owned=*/false));
        m_z80Mem.uart().tick(tstates);
        m_z80Mem.subCpu().tickByTStates(tstates);
        m_z80Mem.display().tick(tstates);
        // LU-57813P calendar clock: unlike the two SC7852-only timer
        // accumulators above (real hardware sources they free-run
        // against, but this core only models while the SC7852 steps),
        // the calendar clock sits on the always-powered VGG rail and must
        // keep ticking here too. The OFF-key/auto-power-off shutdown
        // hands the bus to the LH5803 and can leave it there for the
        // machine's entire "powered off" span -- without this, TIME/DATE$
        // would visibly lag by however long the machine stayed off, even
        // though the emulation itself keeps running at real speed the
        // whole time. Same accumulator, same period, just fed from this
        // branch's own T-states instead.
        m_rtcAccum += tstates;
        // Buzzer time keeps running while the LH5803 owns the bus too.
        m_z80Mem.advanceBuzzer(static_cast<uint32_t>(tstates));
        while (m_rtcAccum >= kRtcPeriodTStates) {
            m_rtcAccum -= kRtcPeriodTStates;
            m_z80Mem.subCpu().tickOneSecond();
        }
    }
    // LH5803->SC7852 has no documented following HALT -- the STA
    // #(0A038H) store itself is the whole handoff, so the switch (and the
    // SC7852's resume-from-park) happens immediately after this step().
    if (m_arbiter.switchRequestedByLH5803()) {
        m_arbiter.switchToSC7852();
        m_sc7852.resumeFromHalt();
    }
    maybeDrainTrace();
    return c;
}

uint64_t PC1600Machine::runCycles(uint64_t maxCycles) {
    uint64_t consumed = 0;
    while (consumed < maxCycles) {
        // Sampled before step(), which may complete a pending handoff and
        // hand the bus to the other CPU: the cost step() returns belongs to
        // whichever CPU actually executed, i.e. the owner on entry.
        const bool z80Owns = sc7852Owns();
        const int c = step(); // takes m_mutex per step, as PC1500Machine does
        // The halted-step fallback must match whichever CPU actually owned
        // the bus -- step()'s own internal accounting (the LH5803 branch's
        // local `tstates`, which feeds m_rtcAccum among others) already
        // charges a halted LH5803 step 1 raw LH5803 cycle
        // (LH5801::kHaltTickCycles), not SC7852::kHaltTickCycles. Using the
        // SC7852 figure here regardless of owner would overcount this
        // budget by ~5.5x during an OFF/auto-power-off span -- runCycles
        // would then stop calling step() long before step()'s own
        // m_rtcAccum had actually accumulated the requested amount of real
        // elapsed time, so the calendar clock would lag during the very
        // window m_rtcAccum's LH5803 branch (see step()) exists to cover.
        const uint64_t cycles = static_cast<uint64_t>(
            c > 0 ? c : (z80Owns ? SC7852::kHaltTickCycles : LH5801::kHaltTickCycles));
        const uint64_t tstates = toTStates(cycles, z80Owns);
        consumed += tstates;
        // See setYieldHook(). step() takes m_mutex per call, so it isn't
        // held here.
        if (m_yieldHook) {
            if (tstates >= m_yieldCountdown) {
                m_yieldCountdown = m_yieldInterval;
                m_yieldHook();
            } else {
                m_yieldCountdown -= tstates;
            }
        }
    }
    return consumed;
}

void PC1600Machine::setYieldHook(std::function<void()> hook, uint64_t intervalTStates) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_yieldHook = std::move(hook);
    m_yieldInterval = intervalTStates;
    m_yieldCountdown = intervalTStates;
}

void PC1600Machine::pressKey(const std::string& name) {
    PC1600Keyboard::Key key = PC1600Keyboard::keyFromName(name);
    if (key == PC1600Keyboard::Key::Unknown) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_z80Mem.keyboard().setKeyState(key, true);
}
void PC1600Machine::releaseKey(const std::string& name) {
    PC1600Keyboard::Key key = PC1600Keyboard::keyFromName(name);
    if (key == PC1600Keyboard::Key::Unknown) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_z80Mem.keyboard().setKeyState(key, false);
}
void PC1600Machine::setOnKeyPressed(bool pressed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool risingEdge = m_z80Mem.setOnKeyPressed(pressed);
    // The ROM's auto-power-off / OFF-key power-down parks the SC7852 in a
    // HALT with IFF1=1 but every *periodic* interrupt cause masked at port
    // 35H (confirmed by a GUI freeze trace, 2026-09: last frame is
    // `PC=5C22 OP=76` HALT, IFF1=1, reached right after an EI/RETI, and the
    // 64 Hz key-scan and 0.5 s sub-CPU ticks this class would otherwise
    // raise never fire again). Only the ON/BREAK line can wake it from
    // there. `m_z80Mem.setOnKeyPressed()` just latches the pollable IF-b1
    // bit (port 1BH) -- a HALTed CPU never polls it -- so also raise a real
    // interrupt: SC7852::step()'s HALT branch resumes on any interrupt
    // regardless of IFF1, and the ROM's ISR then services the latch. Rising
    // edge only, matching the latch and a real PB7 edge.
    // Wake whichever CPU is actually parked: a GUI-freeze repro (headless,
    // real ROM boot) found the arbiter had already switched bus ownership
    // to the LH5803 by the time power-down settles (SC7852 issues its
    // documented `OUT (38H),A` handoff before its own final HALT, per
    // step()'s own comment on that sequence), so waking only the SC7852
    // left the machine stuck -- the LH5803 was the CPU actually halted
    // (at 0xE555 in that repro). The LH5803 is LH5801-family and its
    // ordinary requestMaskableInterrupt() is IE-gated (won't wake a HALT
    // with IE clear, which this repro also hit) -- wakeFromHalt() is the
    // unconditional counterpart for exactly this non-maskable ON signal.
    if (risingEdge) {
        m_sc7852.requestInterrupt();
        m_lh5803.wakeFromHalt();
    }
}

bool PC1600Machine::pokeMemory(uint16_t address, const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // All-or-nothing: check every byte's address is writable before
    // committing any write.
    for (size_t i = 0; i < size; i++) {
        uint16_t addr = static_cast<uint16_t>(address + i); // wraps at 0xFFFF, matching real Z-80 address arithmetic
        if (!m_z80Mem.isWritable(addr)) return false;
    }
    for (size_t i = 0; i < size; i++) {
        uint16_t addr = static_cast<uint16_t>(address + i);
        m_z80Mem.write(addr, data[i]);
    }
    return true;
}

uint8_t PC1600Machine::debugPeek(uint16_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_z80Mem.peek(addr);
}

bool PC1600Machine::debugSc7852Owns() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_arbiter.sc7852Owns();
}

void PC1600Machine::setSerialLink(SerialLink* link) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_z80Mem.uart().setSerialLink(link);
}

void PC1600Machine::debugCopyInternalRam(uint8_t* out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_z80Mem.debugCopyInternalRam(out);
}

bool PC1600Machine::debugWriteInternalRam(size_t off, const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_z80Mem.debugWriteInternalRam(off, data, n);
}

bool PC1600Machine::debugWriteSlotImage(int slot, size_t off, const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return slot == 1 ? m_z80Mem.slot1CardImageWrite(off, data, n)
                     : m_z80Mem.slot2CardImageWrite(off, data, n);
}

std::vector<uint8_t> PC1600Machine::debugSlotImage(int slot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return slot == 1 ? m_z80Mem.slot1CardImage() : m_z80Mem.slot2CardImage();
}

PC1600Machine::DebugBankState PC1600Machine::debugBankState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    DebugBankState s;
    s.port31 = m_bank.readPort31();
    s.port28 = m_bank.slot2VerticalBank();
    s.port3c = m_bank.readPort3C();
    s.pageABank = m_bank.pageABank();
    s.pageBBank = m_bank.pageBBank();
    s.pageCBank = m_bank.pageCBank();
    s.pageDBank = m_bank.pageDBank();
    s.slot2MapMode = m_bank.slot2MapMode();
    s.slot1MapRemapped = m_bank.slot1MapActive(); // SLOT1MAP bit -- now modelled, see PC1600Memory::slot1MapTarget()
    s.hiddenBasicRom = m_bank.hiddenBasicRomSelected();
    s.slot1CardBank = m_z80Mem.slot1CardBank();
    s.slot2CardBank = m_z80Mem.slot2CardBank();
    s.slot1CardBankCount = m_z80Mem.slot1CardBankCount();
    s.slot2CardBankCount = m_z80Mem.slot2CardBankCount();

    // Live address-map resolution -- mirrors PC1600Memory::resolveConst()'s
    // branch set + the two slot windows + slot2MapTarget()'s/slot1MapTarget()'s
    // gates. A SLOTMAP redirect only counts as live when it is both armed
    // (Port 3CH mode/bit) and triggered (the page's bank field == 1) AND the
    // relevant card is present -- an empty slot lets the access fall through
    // to its ordinary meaning (read()'s own comment).
    using PT = PageTarget;
    const bool s1 = m_z80Mem.slot1Attached();
    const bool s2 = m_z80Mem.slot2Attached();
    const uint8_t mode = s.slot2MapMode;

    // Page A (0000-3FFF): always Bank 0 lower, unless SLOT2MAP mode 2 steals
    // bank 1 for Slot 2's high 16 KB.
    if (mode == 2 && s.pageABank == 1 && s2) {
        s.target[0] = PT::Slot2; s.slotmapRedirect[0] = true;
    } else {
        s.target[0] = PT::SystemRomLo;
    }

    // Page B (4000-7FFF): SLOT1MAP's beta mirror and SLOT2MAP mode 2's low
    // half both live here -- the only real collision, arbitrated by the
    // same PC1600Bank::resolveSlotCollision() the live read/write decode
    // uses (PC1600Memory::resolveSlotRemap()).
    bool slot1B = s.slot1MapRemapped && s.pageBBank == 1 && s1;
    bool slot2B = mode == 2 && s.pageBBank == 1 && s2;
    m_bank.resolveSlotCollision(&slot1B, &slot2B);
    if (slot1B || slot2B) {
        s.target[1] = slot1B ? PT::Slot1 : PT::Slot2;
        s.slotmapRedirect[1] = true;
    } else {
        switch (s.pageBBank) {
            case 0:  s.target[1] = PT::SystemRomHi; break;
            case 3:  s.target[1] = s.hiddenBasicRom ? PT::Bank3bRom : PT::Bank3Rom; break;
            case 4:
            case 5:  s.target[1] = PT::PeripheralRom; break;
            default: s.target[1] = PT::OpenBus; break; // banks 1/2/6/7
        }
    }

    // Page C (8000-BFFF).
    if (mode == 1 && s.pageCBank == 1 && s2) {
        s.target[2] = PT::Slot2; s.slotmapRedirect[2] = true;
    } else {
        switch (s.pageCBank) {
            case 0:
            case 1:  s.target[2] = PT::Slot1; break;
            case 2:
            case 3:  s.target[2] = PT::Slot2; break;
            case 6:  s.target[2] = PT::Bank6Rom; break;
            default: s.target[2] = PT::OpenBus; break; // banks 4/5/7
        }
    }

    // Page D (C000-FFFF): bank 0 internal RAM, bank 1 open bus (unmodelled).
    s.target[3] = (s.pageDBank == 0) ? PT::InternalRam : PT::OpenBus;

    return s;
}

void PC1600Machine::setTraceEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_traceEnabled = enabled;
    // TRACE_PC alone never populates a frame's register fields at all.
    // PC1500Machine's own trace path (driven by EmulatorViewModel's
    // `updateTraceFlags()`) passes [.PC, .regsLight, .regsFull]; this
    // brings PC-1600 to parity.
    uint32_t flags = enabled ? (TRACE_PC | TRACE_REGS_LIGHT | TRACE_REGS_FULL) : TRACE_NONE;
    m_sc7852.setTraceFlags(flags);
    m_lh5803.setTraceFlags(flags);
}

bool PC1600Machine::beginCpuTrace(std::FILE* handle, uint32_t flags) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!handle || m_traceFile) return false;
    m_traceFile = std::make_unique<PC1500TraceFile>(handle);
    m_traceDrainCounter = 0;
    m_sc7852.setTraceFlags(flags);
    m_lh5803.setTraceFlags(flags);
    // Discard whatever is already in either ring (and its overflow
    // accounting) so the file starts clean even if tracing was already on.
    uint32_t staleLost = 0;
    m_sc7852.drainTraceEvents(m_z80TraceDrainBuf, kTraceDrainBufFrames, &staleLost);
    m_lh5803.drainTraceEvents(m_lhTraceDrainBuf, kTraceDrainBufFrames, &staleLost);
    return true;
}

void PC1600Machine::endCpuTrace() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_traceFile) return;
    pumpTraceFile();
    m_traceFile->finish();
    m_traceFile.reset();
    m_sc7852.setTraceFlags(TRACE_NONE);
    m_lh5803.setTraceFlags(TRACE_NONE);
    m_traceDrainCounter = 0;
}

void PC1600Machine::pumpTraceFile() {
    // Caller holds m_mutex. Each CPU's drainTraceEvents() takes that core's
    // own independent ring mutex -- no lock-ordering concern.
    uint32_t lost = 0;
    uint32_t n = m_sc7852.drainTraceEvents(m_z80TraceDrainBuf, kTraceDrainBufFrames, &lost);
    if (lost) m_traceFile->writeGap(lost);
    for (uint32_t i = 0; i < n; i++) {
        // SC7852::recordTraceFrame() leaves cpuId unset (the file's 0x04
        // record type already marks a Z80 frame); stamp it so the merged
        // stream is self-describing, matching the LH5803 frames the core
        // tags in its ctor.
        m_z80TraceDrainBuf[i].cpuId = CPU_ID_SC7852;
        m_traceFile->writeFrame(m_z80TraceDrainBuf[i]);
    }

    lost = 0;
    n = m_lh5803.drainTraceEvents(m_lhTraceDrainBuf, kTraceDrainBufFrames, &lost);
    if (lost) m_traceFile->writeGap(lost);
    for (uint32_t i = 0; i < n; i++) m_traceFile->writeFrame(m_lhTraceDrainBuf[i]);
}

PC1600DisplaySnapshot PC1600Machine::displaySnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    PC1600DisplaySnapshot snap;
    const PC1600Display& disp = m_z80Mem.display();
    snap.clockEnabled = disp.clockEnabled();
    for (int y = 0; y < PC1600Display::kHeight; y++) {
        for (int x = 0; x < PC1600Display::kWidth; x++) {
            snap.pixels[y][x] = disp.pixel(x, y);
        }
    }
    const auto& symbols = disp.statusLine().all();
    for (size_t i = 0; i < symbols.size(); i++) snap.statusSymbols[i] = symbols[i];
    return snap;
}
