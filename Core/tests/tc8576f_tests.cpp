// Headless C++ tests for the TC8576F UART model (Core/PC1600/TC8576F.hpp)
// and the sub-CPU parallel-port BUSY-window timing added to PC1600SubCpu.
// Same no-framework assert-and-tally style as sc7852_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

#include "../PC1600/PC1600SubCpu.hpp"
#include "../Serial/SerialLink.hpp"
#include "../PC1600/TC8576F.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// SSR / PSR bit positions (mirror TC8576F.cpp's anonymous-namespace consts).
constexpr uint8_t kSsrTxRDY = 0x01;
constexpr uint8_t kSsrRxRDY = 0x02;
constexpr uint8_t kSsrTxE   = 0x04;
constexpr uint8_t kSsrOE    = 0x10;
constexpr uint8_t kPsrBUSY  = 0x20;
// SIO connector inputs overlaid on the low PSR bits when a peer is attached.
constexpr uint8_t kPsrCS    = 0x01; // b0 = CTS
constexpr uint8_t kPsrCD    = 0x02; // b1 = DCD
constexpr uint8_t kPsrDS    = 0x04; // b2 = DSR

// One emulated character time in T-states at the pre-SETCOM default
// (divisor 8 -> 9600 baud, 8N1 -> 10 bits): 3'580'000 * 8 * 10 / 76'800.
constexpr int kDefaultCharTStates = 3729;

// A minimal SerialLink test double: a queue of bytes the "peer" will
// deliver, a log of bytes the UART sent, and settable modem lines.
struct FakeLink : SerialLink {
    std::deque<uint8_t> rx;      // peer -> UART
    std::vector<uint8_t> tx;     // UART -> peer
    Lines lines;                 // reported by getStatus()
    bool lastDtr = false, lastRts = false;
    int controlCalls = 0;
    int baudCalls = 0;
    uint32_t lastBaud = 0;

    bool poll(uint8_t& out) override {
        if (rx.empty()) return false;
        out = rx.front();
        rx.pop_front();
        return true;
    }
    void send(uint8_t b) override { tx.push_back(b); }
    void setControl(bool dtr, bool rts) override {
        lastDtr = dtr;
        lastRts = rts;
        controlCalls++;
    }
    void getStatus(Lines& in) override { in = lines; }
    void onBaud(uint32_t baud, int) override {
        baudCalls++;
        lastBaud = baud;
    }
};

void test_reset_status_is_idle_ready() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    // No serial peer: transmitter always ready + drained, nothing received,
    // no parallel device busy.
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK(uart.ssr() != 0xFF);
    CHECK(uart.psr() == 0x00);
}

void test_parameter_address_pointer_and_file() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    // ROM init pattern: OUT (23H),0xC0 selects param address 0, then the
    // byte goes to OUT (22H); 0xC1 -> address 1; etc. (romIV-6 a306-a322).
    uart.writeRegister(3, 0xC0);
    uart.writeRegister(2, 0x2A);
    uart.writeRegister(3, 0xC1);
    uart.writeRegister(2, 0x03);
    uart.writeRegister(3, 0xC5);
    uart.writeRegister(2, 0x8E);
    CHECK(uart.parameter(0) == 0x2A);
    CHECK(uart.parameter(1) == 0x03);
    CHECK(uart.parameter(5) == 0x8E);
}

void test_command_register_reset_bit_clears_file() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    uart.writeRegister(3, 0xC0);
    uart.writeRegister(2, 0x55);
    CHECK(uart.parameter(0) == 0x55);
    uart.writeRegister(3, 0xE0); // 11 1xxxxx -- b5 set = chip reset
    CHECK(uart.parameter(0) == 0x00);
}

void test_serial_transmit_reports_sent_immediately() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    uart.writeRegister(0, 0x41); // TxD = 'A'
    CHECK((uart.ssr() & kSsrTxRDY) != 0); // still ready for the next byte
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK((uart.readRegister(0) & 0xFF) == 0xFF); // RxD: nothing received
}

void test_parallel_out_drives_subcpu_command_and_busy_window() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    CHECK(!sub.busy());
    uart.writeRegister(1, 0x5A);        // 21H PVOUT = sub-CPU request 5AH (reset cause)
    CHECK(sub.busy());                  // BUSY asserted for the handshake window
    CHECK((uart.psr() & kPsrBUSY) != 0);
    CHECK((uart.psr() & 0x40) != 0);    // bit 6: still processing (the ROM's A98C wait)
    CHECK(!sub.answerReady());          // not readable until BUSY clears
    // The window is the sub-CPU's measured response time (~1.66 ms), not
    // just the TRM's ~20 us strobe pulse.
    sub.tickByTStates(PC1600SubCpu::kBusyTStates / 2);
    CHECK(sub.busy());
    // The answer register itself is filled synchronously (legacy callers).
    CHECK(sub.answerPending());

    sub.tickByTStates(PC1600SubCpu::kBusyTStates); // elapse the window
    CHECK(!sub.busy());
    CHECK((uart.psr() & kPsrBUSY) == 0);
    CHECK((uart.psr() & 0x40) == 0);
    CHECK(sub.answerReady());
    CHECK(sub.readAnswer() == 0xA0);    // cold-power-up reset cause
}

void test_interrupt_hook_fires_on_transmit_when_unmasked() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    int hits = 0;
    uart.setInterruptHook([&hits] { hits++; });
    uart.writeRegister(0, 0x31);        // TxD -> TxRDY, hook fires (TxINTM clear)
    CHECK(hits == 1);
    // Set pr[5] b1 (TxINTM) -> transmit no longer raises the line.
    uart.writeRegister(3, 0xC5);
    uart.writeRegister(2, 0x02);
    uart.writeRegister(0, 0x32);
    CHECK(hits == 1);
}

// ── Phase 2: a serial peer attached via setSerialLink() ─────────────────

void test_serial_tx_fifo_drains_one_byte_per_char_time() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    uart.setSerialLink(&link);
    uart.writeRegister(3, 0x05);          // SCR: TxEN | RxEN
    uart.writeRegister(0, 'H');           // queue two bytes
    uart.writeRegister(0, 'i');
    CHECK(link.tx.empty());               // nothing on the wire yet
    CHECK((uart.ssr() & kSsrTxE) == 0);   // transmitter not empty

    uart.tick(kDefaultCharTStates);
    CHECK(link.tx.size() == 1);
    CHECK(link.tx[0] == 'H');
    uart.tick(kDefaultCharTStates);
    CHECK(link.tx.size() == 2);
    CHECK(link.tx[1] == 'i');
    CHECK((uart.ssr() & kSsrTxE) != 0);   // drained
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
}

void test_serial_rx_latches_and_flags_overrun() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    link.rx = {0x41, 0x42};
    uart.setSerialLink(&link);
    uart.writeRegister(3, 0x05);          // TxEN | RxEN

    uart.tick(kDefaultCharTStates);       // pulls 0x41
    CHECK((uart.ssr() & kSsrRxRDY) != 0);
    CHECK((uart.ssr() & kSsrOE) == 0);

    uart.tick(kDefaultCharTStates);       // pulls 0x42 while 0x41 unread
    CHECK((uart.ssr() & kSsrOE) != 0);    // overrun
    CHECK((uart.readRegister(0) & 0xFF) == 0x41); // first byte kept
    CHECK((uart.ssr() & kSsrRxRDY) == 0); // cleared on read
}

void test_serial_rx_interrupt_needs_rxenable_unmasked() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    link.rx = {0x55};
    int hits = 0;
    uart.setSerialLink(&link);
    uart.setInterruptHook([&hits] { hits++; });

    uart.writeRegister(3, 0x00);          // RxEN clear
    uart.tick(kDefaultCharTStates);       // byte latches, but no INT
    CHECK((uart.ssr() & kSsrRxRDY) != 0);
    CHECK(hits == 0);

    (void)uart.readRegister(0);           // clear RxRDY
    link.rx.push_back(0x56);
    uart.writeRegister(3, 0x04);          // RxEN
    uart.tick(kDefaultCharTStates);
    CHECK(hits == 1);
}

void test_serial_baud_divisor_sets_cadence_and_notifies_peer() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    uart.setSerialLink(&link);
    // pr[0]/pr[1] = 32 -> 76800/32 = 2400 baud; pr[5] b3:b2 = 11 -> 8 bits.
    uart.writeRegister(3, 0xC0); uart.writeRegister(2, 32);
    uart.writeRegister(3, 0xC1); uart.writeRegister(2, 0);
    uart.writeRegister(3, 0xC5); uart.writeRegister(2, 0x0C);
    CHECK(link.baudCalls >= 1);
    CHECK(link.lastBaud == 2400);

    uart.writeRegister(3, 0x05);          // TxEN | RxEN
    uart.writeRegister(0, 'X');
    // char time = 3'580'000 * 32 * 10 / 76'800 = 14916 T-states.
    uart.tick(14915);
    CHECK(link.tx.empty());
    uart.tick(2);
    CHECK(link.tx.size() == 1);
}

void test_serial_psr_overlays_peer_modem_lines() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    link.lines.cts = false;
    link.lines.dcd = true;
    link.lines.dsr = false;
    uart.setSerialLink(&link);

    uart.tick(1);                         // refresh line cache
    CHECK((uart.psr() & kPsrCS) == 0);
    CHECK((uart.psr() & kPsrCD) != 0);
    CHECK((uart.psr() & kPsrDS) == 0);
}

void test_serial_command_register_forwards_rts_dtr() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    uart.setSerialLink(&link);

    uart.writeRegister(3, 0x22);          // SCR: b1 DTR | b5 RTS
    CHECK(link.lastDtr);
    CHECK(link.lastRts);
    uart.writeRegister(3, 0x00);
    CHECK(!link.lastDtr);
    CHECK(!link.lastRts);
}

void test_serial_cts_low_holds_the_transmitter() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    link.lines.cts = false;
    uart.setSerialLink(&link);
    uart.writeRegister(3, 0x05);          // TxEN | RxEN
    uart.writeRegister(0, 'Z');

    uart.tick(kDefaultCharTStates * 3);
    CHECK(link.tx.empty());               // CTS low -> nothing shifts out
    link.lines.cts = true;
    uart.tick(kDefaultCharTStates);
    CHECK(link.tx.size() == 1);
}

void test_no_link_preserves_standalone_behaviour() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    uart.writeRegister(0, 0x41);          // TxD -- instant "sent"
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK((uart.readRegister(0) & 0xFF) == 0xFF);
    uart.tick(100000);                    // must be a no-op with no peer
    CHECK((uart.readRegister(0) & 0xFF) == 0xFF);
    CHECK(uart.psr() == 0x00);
}

void test_chip_reset_keeps_peer_attached() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    uart.setSerialLink(&link);
    CHECK(uart.serialLink() == &link);
    uart.writeRegister(3, 0xE0);          // command-register chip reset (b5)
    CHECK(uart.serialLink() == &link);
    uart.setSerialLink(nullptr);
    CHECK(uart.serialLink() == nullptr);
}

} // namespace

int run_tc8576f_tests() {
    g_pass = g_fail = 0;
    test_reset_status_is_idle_ready();
    test_parameter_address_pointer_and_file();
    test_command_register_reset_bit_clears_file();
    test_serial_transmit_reports_sent_immediately();
    test_parallel_out_drives_subcpu_command_and_busy_window();
    test_interrupt_hook_fires_on_transmit_when_unmasked();
    test_serial_tx_fifo_drains_one_byte_per_char_time();
    test_serial_rx_latches_and_flags_overrun();
    test_serial_rx_interrupt_needs_rxenable_unmasked();
    test_serial_baud_divisor_sets_cadence_and_notifies_peer();
    test_serial_psr_overlays_peer_modem_lines();
    test_serial_command_register_forwards_rts_dtr();
    test_serial_cts_low_holds_the_transmitter();
    test_no_link_preserves_standalone_behaviour();
    test_chip_reset_keeps_peer_attached();
    std::printf("tc8576f: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
