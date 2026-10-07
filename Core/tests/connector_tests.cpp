// Headless C++ tests for the connector layer
// (Core/Connector/ExpansionConnector.hpp, SystemBus.hpp, ExpansionCard.hpp,
// SystemBusCard.hpp).
// Same no-framework, assert-and-tally style as lh5801_tests.cpp -- see that
// file's header comment. Exercises PC1500Memory's dispatch into both
// connectors via PC1500Machine, using a small lambda-backed test-only
// card (StubCard on the 40-pin plug, BusStub on the 60-pin one) instead of
// a software-defined card.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "../Connector/CE1600PCard.hpp"
#include "../Connector/CardChain.hpp"
#include "../Connector/Ce150Card.hpp"
#include "../Connector/ExpansionCard.hpp"
#include "../Connector/SystemBusCard.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600BusDrive.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Test-only card: every behavior is supplied as a lambda so each test can
// describe exactly the enable-condition shape it wants (mirroring
// docs/Memory-Card-Definition-Spec.md §4's AND/OR addressing model) without
// a new subclass per test.
template <class Card, class Pins>
class StubCardT : public Card, public InhibitSource {
public:
    using ReadFn = std::function<bool(const Pins&, uint8_t&)>;
    using WriteFn = std::function<bool(const Pins&, uint8_t)>;

    explicit StubCardT(ReadFn read, WriteFn write = nullptr, bool inhibit = false)
        : m_read(std::move(read)), m_write(std::move(write)), m_inhibit(inhibit) {}

    bool respondsToRead(const Pins& pins, uint8_t& outValue) const override {
        return m_read && m_read(pins, outValue);
    }
    WriteResult respondsToWrite(const Pins& pins, uint8_t value) override {
        return m_write && m_write(pins, value) ? WriteResult::taken() : WriteResult::ignored();
    }
    bool assertsInhibit() const override { return m_inhibit; }

private:
    ReadFn m_read;
    WriteFn m_write;
    bool m_inhibit;
};

using StubCard = StubCardT<ExpansionCard, PinState>;   // 40-pin
using BusStub = StubCardT<SystemBusCard, SystemBusPins>; // 60-pin

// A read-only stub that returns `value` whenever `predicate` matches --
// covers the common case (most tests below don't care about writes).
template <class Stub = StubCard, class Predicate>
Stub makeReadStub(uint8_t value, Predicate predicate, bool inhibit = false) {
    return Stub(
        [value, predicate](const auto& pins, uint8_t& out) -> bool {
            if (!predicate(pins)) return false;
            out = value;
            return true;
        },
        nullptr, inhibit);
}

void test_regression_no_card_attached() {
    // Identical to lh5801_tests.cpp's test_memory_open_bus_and_ram_regions,
    // but via PC1500Machine, which always owns both connectors -- confirms
    // that wiring is a true no-op when nothing is attached.
    PC1500Machine machine(PC1500Variant::PC1500A);
    CHECK(machine.memory().readME0(0x0000) == 0xFF); // Y0, no card
    CHECK(machine.memory().readME0(0x8000) == 0xFF); // Y2, no card
    CHECK(machine.memory().readME0(0x5800) == 0xFF); // S3, no card (PC-1500A open range)
    CHECK(machine.memory().readME0(0xC000) == 0xFF); // ROM, powers up 0xFF, no INHIBIT
}

void test_expansion_connector_y0_subrange_dispatch() {
    // Mirrors CE-155's own decode shape (Ref/PC-1500/Memory-Architecture/PC-1500-Address-Decoding.md §3.2):
    // Y0 AND AD11-AD13=111, i.e. the top 2KB of the 16KB Y0 window.
    PC1500Machine machine(PC1500Variant::PC1500A);
    StubCard card = makeReadStub(0x42, [](const PinState& p) {
        return p.pin[4] && (p.address & 0x3800) == 0x3800;
    });
    machine.expansionConnector().attach(&card);

    CHECK(machine.memory().readME0(0x3800) == 0x42); // in range
    CHECK(machine.memory().readME0(0x3FFF) == 0x42); // in range, top edge
    CHECK(machine.memory().readME0(0x0000) == 0xFF); // Y0, but outside the stub's sub-range
    CHECK(machine.memory().readME0(0x37FF) == 0xFF); // just below the sub-range

    machine.expansionConnector().detach();
    CHECK(machine.memory().readME0(0x3800) == 0xFF); // detach actually clears it
}

void test_expansion_connector_variant_routing() {
    // Ref/Shared/Expansion-Connectors.md §3.2: S5 (&6800-&6FFF) never reaches the
    // PC-1500's 40-pin connector at all -- no physical wire exists for it,
    // regardless of what the attached card wants to react to. On the
    // PC-1500A, the same named S5 block IS routed (via pin 18). This is
    // the one real per-variant asymmetry the connector's routing table
    // must reproduce (S1-S4 all route identically on both models, just via
    // different physical pins -- not independently testable through this
    // named-signal-level API, see plan discussion).
    StubCard s5Stub = makeReadStub(0x55, [](const PinState& p) { return p.pin[18]; });

    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.expansionConnector().attach(&s5Stub);
    CHECK(pc1500.memory().readME0(0x6800) == 0xFF); // S5 unreachable on PC-1500

    PC1500Machine pc1500a(PC1500Variant::PC1500A);
    pc1500a.expansionConnector().attach(&s5Stub);
    CHECK(pc1500a.memory().readME0(0x6800) == 0x55); // S5 reaches the connector on PC-1500A
}

void test_expansion_connector_pu_pv_gating() {
    // Ref/PC-1500/Memory-Architecture/PU-PV-Signals.md §4: PV/PU select which peripheral ROM table is
    // visible in Y2 (&8000-&BFFF). A card gated on "PU=1, PV=0" should only
    // respond in exactly that state.
    PC1500Machine machine(PC1500Variant::PC1500A);
    StubCard card = makeReadStub(0x99, [](const PinState& p) { return p.pin[19] && p.pin[2] && !p.pin[3]; });
    machine.expansionConnector().attach(&card);

    machine.memory().updatePUPV(false, false);
    CHECK(machine.memory().readME0(0x8000) == 0xFF);

    machine.memory().updatePUPV(true, false);
    CHECK(machine.memory().readME0(0x8000) == 0x99);

    machine.memory().updatePUPV(true, true);
    CHECK(machine.memory().readME0(0x8000) == 0xFF); // PV=1 no longer matches the card's gate
}

void test_inhibit_suppresses_rom() {
    PC1500Machine machine(PC1500Variant::PC1500A);
    std::vector<uint8_t> rom(0x4000, 0xAB);
    CHECK(machine.memory().loadROM(rom.data(), rom.size()));
    CHECK(machine.memory().readME0(0xC000) == 0xAB); // plain ROM read, no card at all

    // A card that asserts INHIBIT but doesn't itself respond: ROM must be
    // suppressed (open bus), not silently left readable.
    StubCard silentInhibit = StubCard(nullptr, nullptr, /*inhibit=*/true);
    machine.expansionConnector().attach(&silentInhibit);
    CHECK(machine.memory().readME0(0xC000) == 0xFF);

    // A card that asserts INHIBIT and substitutes its own byte.
    StubCard replacingInhibit = makeReadStub(0x77, [](const PinState& p) { return p.address == 0xC000; }, /*inhibit=*/true);
    machine.expansionConnector().attach(&replacingInhibit);
    CHECK(machine.memory().readME0(0xC000) == 0x77);

    machine.expansionConnector().detach();
    CHECK(machine.memory().readME0(0xC000) == 0xAB); // ROM restored once nothing asserts INHIBIT
}

void test_systembus_me1_access() {
    // Only the 60-pin connector's DME1/ME1 pins expose ME1-space access at
    // all (Ref/Shared/Expansion-Connectors.md §2.2) -- ExpansionConnector has no
    // equivalent method. 0x2000 is outside the LH5811 I/O-chip's own
    // decode window (addr & 0x3000 == 0x3000), so without a card it must
    // still fall through to the ME0 mirror (today's placeholder behavior).
    PC1500Machine machine(PC1500Variant::PC1500A);
    CHECK(machine.memory().readME1(0x2000) == machine.memory().readME0(0x2000)); // mirror, no card

    BusStub card = makeReadStub<BusStub>(0x64, [](const SystemBusPins& p) {
        return p.pin[Contact60::kMe1] && p.pin[Contact60::kDme1] && !p.pin[Contact60::kDme0] && p.address == 0x2000;
    });
    machine.systemBus().attach(&card);
    CHECK(machine.memory().readME1(0x2000) == 0x64);
    CHECK(machine.memory().readME0(0x2000) == 0xFF); // ME0 side is untouched -- me1 stub only matches me1 accesses
}

void test_systembus_daisy_chain() {
    // Models a CE-150-alike and a CE-158-alike sharing the 60-pin chain,
    // each claiming its own Y2 sub-range without conflicting -- see
    // Ref/Shared/Expansion-Connectors.md §2.2's note that the CE-150's 64-pin rear
    // connector lets a second peripheral (typically a CE-158) chain behind
    // it. The 40-pin connector has no equivalent (single slot only).
    PC1500Machine machine(PC1500Variant::PC1500A);
    BusStub ce150ish = makeReadStub<BusStub>(0x50, [](const SystemBusPins& p) { return p.pin[Contact60::kDme0] && p.address >= 0x8000 && p.address < 0x8100; });
    BusStub ce158ish = makeReadStub<BusStub>(0x58, [](const SystemBusPins& p) { return p.pin[Contact60::kDme0] && p.address >= 0x9000 && p.address < 0x9100; });
    machine.systemBus().attach(&ce150ish);
    machine.systemBus().attach(&ce158ish);

    CHECK(machine.memory().readME0(0x8050) == 0x50);
    CHECK(machine.memory().readME0(0x9050) == 0x58);
    CHECK(machine.memory().readME0(0xA000) == 0xFF); // neither claims this

    machine.systemBus().detach(&ce150ish);
    CHECK(machine.memory().readME0(0x8050) == 0xFF); // detaching one doesn't disturb the other
    CHECK(machine.memory().readME0(0x9050) == 0x58);
}

void test_expansion_connector_single_slot_replaces_not_chains() {
    // Real hardware: the 40-pin connector is exactly one slot -- attaching
    // a second card replaces the first, it never chains (contrast with
    // SystemBus's daisy chain above).
    PC1500Machine machine(PC1500Variant::PC1500A);
    StubCard a = makeReadStub(0xAA, [](const PinState& p) { return p.pin[4]; });
    StubCard b = makeReadStub(0xBB, [](const PinState& p) { return p.pin[4]; });
    machine.expansionConnector().attach(&a);
    CHECK(machine.memory().readME0(0x0000) == 0xAA);
    machine.expansionConnector().attach(&b);
    CHECK(machine.memory().readME0(0x0000) == 0xBB); // b replaced a, not layered on top of it
}

void test_two_independent_ports() {
    PC1500Machine machine(PC1500Variant::PC1500A);
    StubCard onFortyPin = makeReadStub(0x40, [](const PinState& p) { return p.pin[4]; });
    BusStub onSixtyPin = makeReadStub<BusStub>(0x60, [](const SystemBusPins& p) { return p.pin[Contact60::kDme0] && p.address >= 0x8000; });
    machine.expansionConnector().attach(&onFortyPin);
    machine.systemBus().attach(&onSixtyPin);

    CHECK(machine.memory().readME0(0x0000) == 0x40); // 40-pin card claims Y0
    CHECK(machine.memory().readME0(0x8000) == 0x60); // 60-pin card claims it (40-pin connector has no card there)
}

void test_card_chain_shell() {
    // The shell every connector dispatches through (CardChain.hpp), on its own.
    CardChain<ExpansionCard, PinState> chain;
    PinState pins;
    uint8_t v = 0;
    CHECK(chain.empty() && chain.front() == nullptr);
    CHECK(!chain.read(pins, v));
    CHECK(!chain.write(pins, 0x12).claimed); // nobody claims -> ignored

    StubCard first = makeReadStub(0x11, [](const PinState&) { return true; });
    StubCard second = makeReadStub(0x22, [](const PinState&) { return true; }, /*inhibit=*/true);
    chain.attach(&first);
    chain.attach(&first); // no duplicate
    chain.attach(&second);
    CHECK(chain.cards().size() == 2);
    CHECK(chain.read(pins, v) && v == 0x11); // first responder wins
    CHECK(chain.inhibitAsserted());          // only `second` declared InhibitSource

    chain.detach(&second);
    CHECK(!chain.inhibitAsserted());         // its inhibit entry went with it

    StubCard writer(nullptr, [](const PinState&, uint8_t) { return true; });
    chain.attach(&writer);
    CHECK(chain.write(pins, 0x34).stored);   // `first` ignores writes, `writer` takes it

    // Chain-of-one form: replace() leaves exactly the new card, inhibit included.
    chain.replace(&second);
    CHECK(chain.cards().size() == 1 && chain.front() == &second && chain.inhibitAsserted());
    chain.replace(nullptr);
    CHECK(chain.empty() && !chain.inhibitAsserted());
}

void test_systembus_contacts_pc1500() {
    // What an LH5801 cycle puts on the 60-pin plug: PU 15, PV 16 (measured),
    // DME0 for ME0, ME1 + DME1 for ME1; no 40-pin Y/S strobes.
    using namespace Contact60;
    SystemBusPins me0 = PC1500SignalDecode::systemBusPins(0x8000, false, /*me1=*/false, /*pu=*/true, /*pv=*/false);
    CHECK(me0.pin[kPU] && !me0.pin[kPV] && me0.pin[kDme0] && !me0.pin[kMe1] && !me0.pin[kDme1]);
    CHECK(!me0.pin[19] && !me0.pin[4] && !me0.pin[2] && !me0.pin[3]); // nothing on 40-pin numbers
    SystemBusPins me1 = PC1500SignalDecode::systemBusPins(0xB00F, true, /*me1=*/true, false, true);
    CHECK(me1.forWrite && me1.pin[kPV] && me1.pin[kMe1] && me1.pin[kDme1] && !me1.pin[kDme0]);
}

void test_systembus_contacts_lh5803() {
    // An LH5803 cycle on the PC-1600 plug: ELH̄ asserted, PU/PV straight
    // through, ME0 = MREQ + DME0, ME1 = IORQ, IOE only for xx00-xx0F and
    // 8000-FFFF.
    using namespace Contact60;
    SystemBusPins me0 = PC1600BusDrive::lh5803Pins(0xA000, false, /*me1=*/false, /*pu=*/false, /*pv=*/true);
    CHECK(me0.pin[kElh] && me0.pin[kMreq] && me0.pin[kDme0] && !me0.pin[kIorq] && !me0.pin[kIoe]);
    CHECK(!me0.pin[kPU] && me0.pin[kPV]);
    SystemBusPins hi = PC1600BusDrive::lh5803Pins(0xD200, false, /*me1=*/true, true, false);
    CHECK(hi.pin[kElh] && hi.pin[kIorq] && hi.pin[kIoe] && !hi.pin[kMreq] && !hi.pin[kDme0] && hi.pin[kPU]);
    CHECK(PC1600BusDrive::lh5803Pins(0x1203, false, true, false, false).pin[kIoe]);  // xx00-xx0F
    CHECK(!PC1600BusDrive::lh5803Pins(0x1213, false, true, false, false).pin[kIoe]); // neither range
    CHECK(PC1600BusDrive::lh5803Pins(0x1213, false, true, false, false).pin[kIorq]); // still an I/O cycle
}

void test_systembus_contacts_z80() {
    // A Z-80 cycle on the PC-1600 plug: page bank on PT/PU/PVOUT (MSB
    // first), MREQ or IORQ, ELH̄ not asserted.
    using namespace Contact60;
    SystemBusPins b5 = PC1600BusDrive::z80MemPins(0x4000, false, 5);
    CHECK(b5.pin[kMreq] && b5.pin[kPT] && !b5.pin[kPU] && b5.pin[kPV] && !b5.pin[kElh] && !b5.pin[kDme0]);
    SystemBusPins b6 = PC1600BusDrive::z80MemPins(0x4000, false, 6);
    CHECK(b6.pin[kPT] && b6.pin[kPU] && !b6.pin[kPV]);
    SystemBusPins io = PC1600BusDrive::z80IoPins(0x82, true);
    CHECK(io.forWrite && io.pin[kIorq] && !io.pin[kMreq] && !io.pin[kElh] && !io.pin[kIoe] && io.address == 0x82);
}

void test_each_card_answers_its_own_cpu() {
    // One plug, two CPUs: the CE-1600P (built for the SC7852) never answers
    // an LH5803 cycle, the CE-150 (built for the LH5801) never a Z-80 one.
    std::vector<uint8_t> lo(CE1600PCard::kRomHalfSize, 0x44), hi(CE1600PCard::kRomHalfSize, 0x55);
    CE1600PCard printer;
    CHECK(printer.loadRom(lo.data(), lo.size(), hi.data(), hi.size()));
    uint8_t v = 0;
    CHECK(printer.respondsToRead(PC1600BusDrive::z80MemPins(0x4000, false, 4), v) && v == 0x44);
    CHECK(printer.respondsToRead(PC1600BusDrive::z80MemPins(0x7FFF, false, 5), v) && v == 0x55);
    CHECK(!printer.respondsToRead(PC1600BusDrive::z80MemPins(0x4000, false, 6), v)); // PU high
    CHECK(!printer.respondsToRead(PC1600BusDrive::z80MemPins(0x8000, false, 4), v)); // not page 1
    CHECK(printer.respondsToRead(PC1600BusDrive::z80IoPins(0x82, false), v));
    CHECK(!printer.respondsToRead(PC1600BusDrive::lh5803Pins(0x4000, false, false, false, true), v));
    CHECK(!printer.respondsToRead(PC1600BusDrive::lh5803Pins(0xD082, false, /*me1=*/true, false, false), v));
    CHECK(!printer.respondsToWrite(PC1600BusDrive::lh5803Pins(0xD083, true, /*me1=*/true, false, false), 0x11));

    std::vector<uint8_t> rom(Ce150Card::kRomSize, 0x66);
    Ce150Card plotter;
    CHECK(plotter.loadRom(rom.data(), rom.size()));
    CHECK(plotter.respondsToRead(PC1600BusDrive::lh5803Pins(0xA000, false, false, false, false), v) && v == 0x66);
    CHECK(plotter.readHasSideEffects(PC1600BusDrive::lh5803Pins(0xB00F, false, /*me1=*/true, false, false)));
    CHECK(!plotter.respondsToRead(PC1600BusDrive::z80MemPins(0xA000, false, 0), v));
    CHECK(!plotter.respondsToRead(PC1600BusDrive::z80IoPins(0x0F, false), v));
}

} // namespace

// Returns the number of failed checks (0 = all passed), so the shared
// main() in lh5801_tests.cpp can fold this file's results into the overall
// exit code.
int run_connector_tests() {
    test_regression_no_card_attached();
    test_expansion_connector_y0_subrange_dispatch();
    test_expansion_connector_variant_routing();
    test_expansion_connector_pu_pv_gating();
    test_inhibit_suppresses_rom();
    test_systembus_me1_access();
    test_systembus_daisy_chain();
    test_systembus_contacts_pc1500();
    test_systembus_contacts_lh5803();
    test_systembus_contacts_z80();
    test_each_card_answers_its_own_cpu();
    test_expansion_connector_single_slot_replaces_not_chains();
    test_two_independent_ports();
    test_card_chain_shell();

    std::printf("connector_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
