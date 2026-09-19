#include "PC1500Machine.hpp"

PC1500Machine::PC1500Machine(PC1500Variant variant)
    : m_memory(variant), m_cpu(m_memory), m_expansionConnector(variant), m_systemBus(variant) {
    m_memory.setExpansionConnector(&m_expansionConnector);
    m_memory.setSystemBus(&m_systemBus);
}

bool PC1500Machine::loadROMFile(const std::string& path) {
    return m_memory.loadROMFile(path);
}

void PC1500Machine::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.reset();
    m_cpu.reset();
    // A chip reset re-anchors an attached CE-150 (LH5810 latches cleared,
    // steppers/pen re-homed) but does NOT unplug it or wipe its paper --
    // real ink stays on real paper. Mirrors the CE-1600P, whose card is
    // likewise left attached across PC1600Machine::reset().
    if (m_ce150Card) m_ce150Card->reset();
}

bool PC1500Machine::attachCE150(const uint8_t* rom, size_t romSize) {
    if (romSize != Ce150Card::kRomSize) return false;
    auto card = std::make_unique<Ce150Card>();
    if (!card->loadRom(rom, romSize)) return false;
    detachCE150();
    card->reset();
    m_systemBus.attach(card.get());
    m_ce150Card = std::move(card);
    return true;
}

void PC1500Machine::detachCE150() {
    if (!m_ce150Card) return;
    m_systemBus.detach(m_ce150Card.get());
    m_ce150Card.reset();
}

// The four below take m_mutex so the GUI thread can read/clear the plotter
// mechanism while the emulation loop runs -- the mechanism's stroke and
// event containers are mutated from inside step()/runCycles() on every
// motor write. Mirrors PC1600Machine's CE-1600P plot accessors.

std::vector<AlpsPlotterMechanism::FlatPoint> PC1500Machine::ce150PlotPoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce150Card) return {};
    return m_ce150Card->mechanism().flatPoints();
}

uint64_t PC1500Machine::ce150PlotRevision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce150Card) return 0;
    return m_ce150Card->mechanism().revision();
}

std::vector<std::string> PC1500Machine::drainCE150Events() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ce150Card) return {};
    return m_ce150Card->mechanism().drainEvents();
}

void PC1500Machine::clearCE150Paper() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ce150Card) m_ce150Card->mechanism().clearPaper();
}

void PC1500Machine::seedClock(int year, int month, int day, int hour, int minute, int second) {
    // Sakamoto's day-of-week: 0 = Sunday. Valid for any Gregorian date;
    // keeps this Core self-contained (no host <ctime> needed) so headless
    // tests can seed a fixed date and check the dow nibble too.
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = (month < 3) ? year - 1 : year;
    int dow = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.seedClock(year, month, day, hour, minute, second, dow);
}

int PC1500Machine::step() {
    std::lock_guard<std::mutex> lock(m_mutex);
    int c = m_cpu.step();
    // The uPD1990AC RTC's TP output is defined in real time, not CPU
    // cycles (it has its own independent crystal) -- this Core has no
    // other notion of elapsed time to drive it from, so it advances here,
    // once per instruction, the same way LH5801's own internal timer
    // advances via tickTimer(). Advance by kHaltTickCycles (not 0) while
    // halted, matching runCycles()'s own halted-time bookkeeping below --
    // real time keeps passing even while the CPU is halted.
    uint32_t rtcCycles = static_cast<uint32_t>(c > 0 ? c : LH5801::kHaltTickCycles);
    m_memory.advanceRtc(rtcCycles);
    m_memory.advancePiezo(rtcCycles); // buzzer time, same clock as the RTC
    // PU/PV (SPU/RPU/SPV/RPV) never touch the bus themselves, so pushing
    // their post-instruction state here is sufficient for the next bus
    // access to see it -- see PC1500Memory::updatePUPV()'s own doc comment.
    m_memory.updatePUPV(m_cpu.pu(), m_cpu.pv());
    advanceKeyQueue(rtcCycles);
    // Per-step hook for an attached CE-150 (no-op today -- the plotter is
    // fully reactive; see Ce150Card::tick()).
    if (m_ce150Card) m_ce150Card->tick(rtcCycles);
    maybeDrainTrace();
    return c;
}

void PC1500Machine::setYieldHook(std::function<void()> hook, uint64_t intervalCycles) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_yieldHook = std::move(hook);
    m_yieldInterval = intervalCycles;
    m_yieldCountdown = intervalCycles;
}

uint64_t PC1500Machine::runCycles(uint64_t maxCycles) {
    std::unique_lock<std::mutex> lock(m_mutex);
    uint64_t consumed = 0;
    uint64_t yieldAccounted = 0;
    while (consumed < maxCycles) {
        // See setYieldHook(). Checked at the top of the loop so the halted
        // branch's `continue` below is covered too; the hook runs unlocked.
        if (m_yieldHook) {
            const uint64_t ran = consumed - yieldAccounted;
            yieldAccounted = consumed;
            if (ran >= m_yieldCountdown) {
                m_yieldCountdown = m_yieldInterval;
                const std::function<void()> hook = m_yieldHook;
                lock.unlock();
                hook();
                lock.lock();
            } else {
                m_yieldCountdown -= ran;
            }
        }
        int c = m_cpu.step();
        m_memory.updatePUPV(m_cpu.pu(), m_cpu.pv());
        if (c == 0) {
            if (m_cpu.consumeBreakpointHit()) break;
            if (m_cpu.halted() || m_cpu.poweredOff()) {
                // step() still ticks the timer once per call while halted
                // (see LH5801::step()'s HLT branch) -- keep polling so a
                // loaded timer can eventually wake it, budgeting each poll
                // at kHaltTickCycles the same way step() paces its own
                // internal tick, rather than treating "halted" as "no
                // forward progress possible" (real hardware's HLT-then-
                // timer-interrupt idle loop, confirmed via the boot smoke
                // test, depends on exactly this still advancing). Genuinely
                // powered off (poweredOff()) budgets the same way -- no
                // internal-timer tick happens there (step() returns 0
                // before even reaching it, matching the real cut CPU
                // clock), but the uPD1990AC RTC chip and the host key
                // queue both sit outside the CPU and must keep advancing
                // regardless (design doc: "the RTC keeps advancing while
                // powered off").
                m_memory.advanceRtc(static_cast<uint32_t>(LH5801::kHaltTickCycles));
                m_memory.advancePiezo(static_cast<uint32_t>(LH5801::kHaltTickCycles)); // buzzer time, same clock as the RTC
                advanceKeyQueue(static_cast<uint32_t>(LH5801::kHaltTickCycles));
                consumed += static_cast<uint64_t>(LH5801::kHaltTickCycles);
                continue;
            }
        }
        m_memory.advanceRtc(static_cast<uint32_t>(c));
        m_memory.advancePiezo(static_cast<uint32_t>(c)); // buzzer time, same clock as the RTC
        advanceKeyQueue(static_cast<uint32_t>(c));
        if (m_ce150Card) m_ce150Card->tick(static_cast<uint32_t>(c)); // no-op today; see step()
        consumed += static_cast<uint64_t>(c);
        maybeDrainTrace();
    }
    return consumed;
}

bool PC1500Machine::beginCpuTrace(std::FILE* handle, uint32_t flags) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!handle || m_traceFile) return false;
    m_traceFile = std::make_unique<PC1500TraceFile>(handle);
    m_traceDrainCounter = 0;
    m_cpu.setTraceFlags(flags);
    // Discard whatever is already in the ring (and its overflow
    // accounting) so the file starts clean even if tracing was already
    // enabled before this call -- e.g. tools/pc1500_cli.cpp turns the
    // trace flags on for its own ring demo before applying the preset.
    uint32_t staleLost = 0;
    m_cpu.drainTraceEvents(m_traceDrainBuf, kTraceDrainBufFrames, &staleLost);
    return true;
}

void PC1500Machine::endCpuTrace() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_traceFile) return;
    pumpTraceFile();
    m_traceFile->finish();
    m_traceFile.reset();
    m_cpu.setTraceFlags(TRACE_NONE);
    m_traceDrainCounter = 0;
}

void PC1500Machine::pumpTraceFile() {
    // Caller holds m_mutex. drainTraceEvents() takes LH5801's own,
    // independent ring mutex -- no lock-ordering concern.
    uint32_t lost = 0;
    uint32_t n = m_cpu.drainTraceEvents(m_traceDrainBuf, kTraceDrainBufFrames, &lost);
    if (lost) m_traceFile->writeGap(lost);
    for (uint32_t i = 0; i < n; i++) m_traceFile->writeFrame(m_traceDrainBuf[i]);
}

void PC1500Machine::pressKey(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.keyboard().setKeyState(PC1500Keyboard::keyFromName(name), true);
}
void PC1500Machine::releaseKey(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.keyboard().setKeyState(PC1500Keyboard::keyFromName(name), false);
}
void PC1500Machine::setOnKeyPressed(bool pressed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool risingEdge = m_memory.setOnKeyPressed(pressed);
    // m_memory.setOnKeyPressed() above only latches the pollable IF bit 1
    // (for BREAK, which needs the CPU already running to poll it) -- a
    // halted or powered-off CPU never polls it, so also raise a real wake
    // here. Two distinct states can leave the CPU unresponsive to
    // anything else, and each needs its own wake shape:
    //
    // - Genuinely powered off (LH5801::poweredOff(), set by the OFF
    //   instruction/0xFD 0x4C -- the ROM's real OFF-key and AUTO POWER OFF
    //   handler both execute it): real hardware has physically cut the
    //   CPU clock, so the ON key's BFI pin restores power with a cold
    //   reset, not a HLT resume -- powerOn() (reset() under the hood).
    // - Merely HLTed with the timer stopped (TM==0, so
    //   LH5801::tickTimer()'s usual wake-on-timer-interrupt can never fire
    //   again): a ROM code path can reach this shape without going through
    //   OFF at all -- wakeFromHalt() is the unconditional (non-IE-gated)
    //   counterpart to requestMaskableInterrupt(), matching the ON key's
    //   real wiring straight to the BFI pin/power latch, not the ordinary
    //   maskable-IRQ line, but (unlike powerOn()) just resumes fetch/
    //   execute right where HLT left off -- no reset.
    //
    // Same shape as PC1600Machine::setOnKeyPressed()'s wakeFromHalt() call
    // for its LH5801-family LH5803 (which has no OFF instruction of its
    // own to distinguish). Rising edge only, matching the latch and a
    // real PB7 edge.
    if (risingEdge) {
        if (m_cpu.poweredOff()) m_cpu.powerOn();
        else m_cpu.wakeFromHalt();
    }
}

void PC1500Machine::enqueueKey(const std::string& name) {
    if (PC1500Keyboard::keyFromName(name) == PC1500Keyboard::Key::Unknown) return; // matches pressKey()'s own silent no-op
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_keyQueue.size() >= kKeyQueueCapacity) return; // dropped, see doc comment
    m_keyQueue.push_back(name);
}

void PC1500Machine::advanceKeyQueue(uint32_t cycles) {
    // Same cadence as PC1500BasicTyper.cpp's tapKey() -- see this
    // method's own header doc comment for why it's duplicated here
    // rather than shared.
    constexpr double kCpuHz = 1300000.0;
    constexpr int kFramesPerSecond = 60;
    constexpr uint64_t kCyclesPerFrame = static_cast<uint64_t>(kCpuHz / kFramesPerSecond);
    constexpr uint64_t kTapCycles = kCyclesPerFrame * 4;  // kTapFrames
    constexpr uint64_t kGapCycles = kCyclesPerFrame * 4;  // kIdleFrames

    if (m_keyQueuePhase == KeyQueuePhase::Idle) {
        if (m_keyQueue.empty()) return;
        m_keyQueueCurrentKey = m_keyQueue.front();
        m_keyQueue.pop_front();
        m_memory.keyboard().setKeyState(PC1500Keyboard::keyFromName(m_keyQueueCurrentKey), true);
        m_keyQueuePhase = KeyQueuePhase::Holding;
        m_keyQueuePhaseCyclesRemaining = kTapCycles;
        return;
    }
    if (cycles < m_keyQueuePhaseCyclesRemaining) {
        m_keyQueuePhaseCyclesRemaining -= cycles;
        return;
    }
    if (m_keyQueuePhase == KeyQueuePhase::Holding) {
        m_memory.keyboard().setKeyState(PC1500Keyboard::keyFromName(m_keyQueueCurrentKey), false);
        m_keyQueuePhase = KeyQueuePhase::Gap;
        m_keyQueuePhaseCyclesRemaining = kGapCycles;
    } else { // Gap elapsed -- back to Idle, picked up by the next call
        m_keyQueuePhase = KeyQueuePhase::Idle;
        m_keyQueuePhaseCyclesRemaining = 0;
    }
}

PC1500Display PC1500Machine::display() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return PC1500Display(m_memory);
}
