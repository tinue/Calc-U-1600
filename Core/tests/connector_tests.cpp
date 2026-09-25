// Headless C++ tests for the connector layer
// (Core/Connector/ExpansionConnector.hpp, SystemBus.hpp, ExpansionCard.hpp).
// Same no-framework, assert-and-tally style as lh5801_tests.cpp -- see that
// file's header comment. Exercises PC1500Memory's dispatch into both
// connectors via PC1500Machine, using a small lambda-backed test-only
// ExpansionCard (StubCard) standing in for the "real" software-defined
// card Phase 7 will eventually build.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "../Connector/ExpansionCard.hpp"
#include "../PC1500/PC1500Machine.hpp"

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
class StubCard : public ExpansionCard {
public:
    using ReadFn = std::function<bool(const PinState&, uint8_t&)>;
    using WriteFn = std::function<bool(const PinState&, uint8_t)>;

    explicit StubCard(ReadFn read, WriteFn write = nullptr, bool inhibit = false)
        : m_read(std::move(read)), m_write(std::move(write)), m_inhibit(inhibit) {}

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        return m_read && m_read(pins, outValue);
    }
    bool respondsToWrite(const PinState& pins, uint8_t value) override {
        return m_write && m_write(pins, value);
    }
    bool assertsInhibit() const override { return m_inhibit; }
    bool mayAssertInhibit() const override { return m_inhibit; }

private:
    ReadFn m_read;
    WriteFn m_write;
    bool m_inhibit;
};

// A read-only stub that returns `value` whenever `predicate` matches --
// covers the common case (most tests below don't care about writes).
StubCard makeReadStub(uint8_t value, std::function<bool(const PinState&)> predicate, bool inhibit = false) {
    return StubCard(
        [value, predicate](const PinState& pins, uint8_t& out) -> bool {
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
    // Mirrors CE-155's own decode shape (PC-1500-Address-Decoding.md §3.2):
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
    // Expansion-Connectors.md §3.2: S5 (&6800-&6FFF) never reaches the
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
    // PU-PV-Signals.md §4: PV/PU select which peripheral ROM table is
    // visible in Y2 (&8000-&BFFF). A card gated on "PU=1, PV=0" should only
    // respond in exactly that state.
    PC1500Machine machine(PC1500Variant::PC1500A);
    StubCard card = makeReadStub(0x99, [](const PinState& p) { return p.pin[19] && p.pin[3] && !p.pin[2]; });
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
    // all (Expansion-Connectors.md §2.2) -- ExpansionConnector has no
    // equivalent method. 0x2000 is outside the LH5811 I/O-chip's own
    // decode window (addr & 0x3000 == 0x3000), so without a card it must
    // still fall through to the ME0 mirror (today's placeholder behavior).
    PC1500Machine machine(PC1500Variant::PC1500A);
    CHECK(machine.memory().readME1(0x2000) == machine.memory().readME0(0x2000)); // mirror, no card

    StubCard card = makeReadStub(0x64, [](const PinState& p) { return p.me1 && p.address == 0x2000; });
    machine.systemBus().attach(&card);
    CHECK(machine.memory().readME1(0x2000) == 0x64);
    CHECK(machine.memory().readME0(0x2000) == 0xFF); // ME0 side is untouched -- me1 stub only matches me1 accesses
}

void test_systembus_daisy_chain() {
    // Models a CE-150-alike and a CE-158-alike sharing the 60-pin chain,
    // each claiming its own Y2 sub-range without conflicting -- see
    // Expansion-Connectors.md §2.2's note that the CE-150's 64-pin rear
    // connector lets a second peripheral (typically a CE-158) chain behind
    // it. The 40-pin connector has no equivalent (single slot only).
    PC1500Machine machine(PC1500Variant::PC1500A);
    StubCard ce150ish = makeReadStub(0x50, [](const PinState& p) { return p.pin[19] && p.address >= 0x8000 && p.address < 0x8100; });
    StubCard ce158ish = makeReadStub(0x58, [](const PinState& p) { return p.pin[19] && p.address >= 0x9000 && p.address < 0x9100; });
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
    StubCard onSixtyPin = makeReadStub(0x60, [](const PinState& p) { return p.pin[19]; });
    machine.expansionConnector().attach(&onFortyPin);
    machine.systemBus().attach(&onSixtyPin);

    CHECK(machine.memory().readME0(0x0000) == 0x40); // 40-pin card claims Y0
    CHECK(machine.memory().readME0(0x8000) == 0x60); // 60-pin card claims Y2 (40-pin connector has no card there)
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
    test_expansion_connector_single_slot_replaces_not_chains();
    test_two_independent_ports();

    std::printf("connector_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
