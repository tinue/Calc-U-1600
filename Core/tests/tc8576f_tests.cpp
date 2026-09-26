// Headless C++ tests for the TC8576F CPC model (Core/PC1600/TC8576F.hpp)
// and the sub-CPU parallel-port handshake in PC1600SubCpu. Expected
// behaviour: SharpPC1500Reference PC-1600/PC-1600-CPC-TC8576.md ("CPC §n").
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

// SSR / PSR bit positions (CPC §4; mirror TC8576F.cpp's constants).
constexpr uint8_t kSsrTxRDY = 0x01;
constexpr uint8_t kSsrRxRDY = 0x02;
constexpr uint8_t kSsrTxE   = 0x04;
constexpr uint8_t kSsrOE    = 0x10;
constexpr uint8_t kPsrFAULT = 0x01; // RS-232C CS, reads 0 when on
constexpr uint8_t kPsrSLCT  = 0x02; // RS-232C CD, reads 1 when on
constexpr uint8_t kPsrPE    = 0x04; // RS-232C DR, reads 1 when on
constexpr uint8_t kPsrP5V   = 0x08; // /P5V tied to GND
constexpr uint8_t kPsrPRIM  = 0x10;
constexpr uint8_t kPsrBUSY  = 0x20;
constexpr uint8_t kPsrXBUSY = 0x40;

// One character time in T-states at 9600 baud 8N1 with the ROM's
// prescaler: 3'580'000 * 8 * B(8) * K(2) * 10 bits / 1'228'800.
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

// What the boot code (P0-B0 078CH) does, then a 9600-baud 8N1 channel the
// way P2-B6 A306H loads it (PR5 = CWCOM byte OR 02H AND 3FH).
void bootInit(TC8576F& uart, uint16_t divisor = 8) {
    static const uint8_t kPr2to7[6] = {0x0F, 0x1F, 0x01, 0xCE, 0x00, 0x02};
    uart.writeRegister(3, 0xE0);
    for (int i = 0; i < 6; i++) {
        uart.writeRegister(3, static_cast<uint8_t>(0xC2 + i));
        uart.writeRegister(2, kPr2to7[i]);
    }
    uart.writeRegister(3, 0xB6);
    uart.writeRegister(3, 0xC0); uart.writeRegister(2, static_cast<uint8_t>(divisor & 0xFF));
    uart.writeRegister(3, 0xC1); uart.writeRegister(2, static_cast<uint8_t>(divisor >> 8));
    uart.writeRegister(3, 0xC5); uart.writeRegister(2, 0x0E);
}

void test_reset_status_is_idle_ready() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    // No serial peer: transmitter always ready + drained, nothing received.
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK(uart.ssr() != 0xFF);
    // Nothing connected: CS off (FAULT = 1), CD/DR off; /P5V grounded;
    // PRIME low (SIO) after B6H; the sub-CPU idle.
    CHECK(uart.psr() == (kPsrFAULT | kPsrP5V));
    CHECK(!uart.rs232Selected());
    CHECK(!uart.interruptOutput());       // every serial interrupt masked
}

void test_parameter_address_pointer_and_file() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    // ROM init pattern: OUT (23H),0xC0 selects param address 0, then the
    // byte goes to OUT (22H); 0xC1 -> address 1; etc. (P2-B6 A306H).
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

// CPC §7: a reset (here the parameter-address D5 bit) keeps PR0-PR7 and
// holds the chip until the next parameter-address write with D5 = 0.
void test_chip_reset_keeps_parameters_and_holds() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    uart.writeRegister(3, 0xC0);
    uart.writeRegister(2, 0x55);
    uart.writeRegister(3, 0x05);          // TxEN | RxEN
    uart.writeRegister(3, 0xB4);          // PRIME on
    CHECK(uart.rs232Selected());
    uart.writeRegister(3, 0xE0);          // 111xxxxx: reset, held
    CHECK(uart.parameter(0) == 0x55);
    CHECK(!uart.rs232Selected());         // PRIME back low
    uart.writeRegister(2, 0x77);          // ignored while held
    CHECK(uart.parameter(0) == 0x55);
    uart.writeRegister(3, 0xC0);          // released
    uart.writeRegister(2, 0x77);
    CHECK(uart.parameter(0) == 0x77);
}

void test_serial_transmit_reports_sent_immediately() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    uart.writeRegister(0, 0x41); // TxD = 'A'
    CHECK((uart.ssr() & kSsrTxRDY) != 0); // still ready for the next byte
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK((uart.readRegister(0) & 0xFF) == 0xFF); // RxD: nothing received
}

// CPC §8.1 / §9.4: the 21H write sets XBUSY at once; KI reaches the sub-CPU
// after the DSTB delay, which then stays busy (Z10) for its response time
// and ACKs (Z9, clearing XBUSY) when its answer is ready.
void test_parallel_out_drives_subcpu_command_and_handshake() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    const int td = uart.dstbDelayTStates();
    CHECK(td == 99);                      // 17 tSYS = 27.7 us at 3.58 MHz
    CHECK(!sub.busy());
    uart.writeRegister(1, 0x5A);          // ~A5H: IOCS 15H, reset cause
    CHECK((uart.psr() & kPsrXBUSY) != 0); // XBUSY from the /WR edge
    CHECK((uart.psr() & kPsrBUSY) == 0);  // KI not there yet
    CHECK(sub.answerPending());           // latched synchronously
    CHECK(!sub.answerReady());

    uart.tick(td); sub.tickByTStates(td);
    CHECK((uart.psr() & kPsrBUSY) != 0);  // Z10 low: running the command
    CHECK((uart.psr() & kPsrXBUSY) != 0);

    uart.tick(PC1600SubCpu::kBusyTStates); sub.tickByTStates(PC1600SubCpu::kBusyTStates);
    CHECK((uart.psr() & kPsrBUSY) == 0);
    CHECK((uart.psr() & kPsrXBUSY) == 0); // ACKed with the answer
    CHECK(sub.answerReady());
    CHECK(sub.readAnswer() == 0xA0);      // cold-power-up reset cause
}

// A command without an answer (a parameter nibble) ACKs on receipt and
// then keeps the sub-CPU busy (Service Manual §4-3 type (ii)).
void test_no_answer_command_acks_on_receipt() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    const int td = uart.dstbDelayTStates();
    uart.writeRegister(1, 0x0F);          // ~F0H: first nibble 0
    uart.tick(td); sub.tickByTStates(td);
    CHECK((uart.psr() & kPsrXBUSY) == 0);
    CHECK((uart.psr() & kPsrBUSY) != 0);
    sub.tickByTStates(PC1600SubCpu::kBusyTStates);
    CHECK((uart.psr() & kPsrBUSY) == 0);
}

// CPC §8.3: B4H/B5H/B6H drive PRIME = PRIM (RS-232C / SIO select); B6H
// also clears XBUSY.
void test_parallel_command_drives_prime_and_clears_xbusy() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    uart.writeRegister(3, 0xB4);
    CHECK(uart.rs232Selected());
    CHECK((uart.psr() & kPsrPRIM) != 0);
    uart.writeRegister(3, 0xB5);          // one-shot: ends low
    CHECK(!uart.rs232Selected());
    uart.writeRegister(1, 0x5A);
    CHECK((uart.psr() & kPsrXBUSY) != 0);
    uart.writeRegister(3, 0xB6);
    CHECK((uart.psr() & kPsrXBUSY) == 0);
}

// CPC §6.5: the transmit interrupt needs TxEN, /CTS = 0 (tied to GND on
// the PC-1600), an empty buffer and TxINTM clear -- a freshly reset chip
// does not interrupt.
void test_interrupt_output_follows_tx_equation() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    uart.setSerialLink(&link);
    std::vector<bool> edges;
    uart.setInterruptHook([&edges](bool level) { edges.push_back(level); });
    uart.tick(1);
    CHECK(!uart.interruptOutput());       // TxEN clear after reset
    uart.writeRegister(3, 0x01);          // TxEN
    CHECK(uart.interruptOutput());        // PR5 = 0: TxINTM clear
    uart.writeRegister(3, 0xC5);
    uart.writeRegister(2, 0x02);          // TxINTM
    CHECK(!uart.interruptOutput());
    CHECK(edges.size() == 2 && edges[0] && !edges[1]);
}

// ── A serial peer attached via setSerialLink() ──────────────────────────

void test_serial_tx_fifo_drains_one_byte_per_char_time() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
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
    bootInit(uart);
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
    CHECK((uart.ssr() & kSsrOE) != 0);    // the read leaves the error set
    uart.writeRegister(3, 0x05);          // SCR without ERS: still set
    CHECK((uart.ssr() & kSsrOE) != 0);
    uart.writeRegister(3, 0x15);          // SCR b4 ERS: error reset
    CHECK((uart.ssr() & kSsrOE) == 0);
}

// CPC §6.2/§6.5: with RxEN clear the receiver ignores the line (bytes wait
// in the link) and cannot interrupt.
void test_serial_receiver_needs_rxenable() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    link.rx = {0x55};
    std::vector<bool> edges;
    uart.setSerialLink(&link);
    uart.setInterruptHook([&edges](bool level) { edges.push_back(level); });

    uart.writeRegister(3, 0x00);          // RxEN clear
    uart.tick(kDefaultCharTStates * 3);
    CHECK((uart.ssr() & kSsrRxRDY) == 0);
    CHECK(link.rx.size() == 1);           // still waiting in the link
    CHECK(!uart.interruptOutput());

    uart.writeRegister(3, 0x04);          // RxEN
    uart.tick(kDefaultCharTStates);
    CHECK(uart.interruptOutput());        // held while the byte waits
    CHECK((uart.readRegister(0) & 0xFF) == 0x55);
    CHECK(!uart.interruptOutput());       // reading RxD drops it
    CHECK(edges.size() == 2 && edges[0] && !edges[1]);
}

// A receive error interrupts too when ERINTM is clear (CPC §6.5).
void test_serial_error_interrupt() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    link.rx = {0x41, 0x42};
    uart.setSerialLink(&link);
    uart.writeRegister(3, 0xC5); uart.writeRegister(2, 0x8E); // RxINTM: only errors
    uart.writeRegister(3, 0x04);          // RxEN
    uart.tick(kDefaultCharTStates);
    CHECK(!uart.interruptOutput());       // RxRDY is masked
    uart.tick(kDefaultCharTStates);       // overrun
    CHECK(uart.interruptOutput());
    uart.writeRegister(3, 0x14);          // ERS
    CHECK(!uart.interruptOutput());
}

void test_serial_baud_divisor_sets_cadence_and_notifies_peer() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    uart.setSerialLink(&link);
    // B = 32 at PR7 = 2 -> 1'228'800 / 2 / 8 / 32 = 2400 baud; 8 bits.
    uart.writeRegister(3, 0xC0); uart.writeRegister(2, 32);
    uart.writeRegister(3, 0xC1); uart.writeRegister(2, 0);
    uart.writeRegister(3, 0xC5); uart.writeRegister(2, 0x0E);
    CHECK(link.baudCalls >= 1);
    CHECK(link.lastBaud == 2400);

    uart.writeRegister(3, 0x05);          // TxEN | RxEN
    uart.writeRegister(0, 'X');
    // char time = 3'580'000 * 8 * 32 * 2 * 10 / 1'228'800 = 14916 T-states.
    uart.tick(14915);
    CHECK(link.tx.empty());
    uart.tick(2);
    CHECK(link.tx.size() == 1);

    // PR7 = 1 passes XCLK straight through: twice the rate.
    uart.writeRegister(3, 0xC7); uart.writeRegister(2, 0x01);
    uart.writeRegister(3, 0xC5); uart.writeRegister(2, 0x0E);
    CHECK(link.lastBaud == 4800);
    // B = 1 stops the generator: nothing shifts out.
    uart.writeRegister(3, 0xC0); uart.writeRegister(2, 1);
    uart.writeRegister(0, 'Y');
    uart.tick(1000000);
    CHECK(link.tx.size() == 1);
}

// CPC §9.5: CS reads 0 when on (FAULT, not inverted), CD/DR read 1 when on.
void test_serial_psr_carries_peer_modem_lines() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    link.lines.cts = true;
    link.lines.dcd = true;
    link.lines.dsr = false;
    uart.setSerialLink(&link);

    uart.tick(1);                         // refresh line cache
    CHECK((uart.psr() & kPsrFAULT) == 0);
    CHECK((uart.psr() & kPsrSLCT) != 0);
    CHECK((uart.psr() & kPsrPE) == 0);
    // The ROM's CESND gating (P2-B6 A524H) flips bit 0 and then sees
    // "on" as 1 for all three.
    CHECK(((uart.psr() ^ 0x01) & 0x07) == 0x03);

    link.lines.cts = false;
    link.lines.dsr = true;
    uart.tick(1);
    CHECK((uart.psr() & kPsrFAULT) != 0);
    CHECK((uart.psr() & kPsrPE) != 0);
}

// CI goes to the sub-CPU (Q1), which reports it in SRINP bit 5, inverted.
void test_serial_ci_reaches_the_subcpu() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    uart.setSerialLink(&link);
    uart.tick(1);
    sub.strobe(0xA3);                     // SRINP
    CHECK((sub.readAnswer() & 0x20) != 0);
    link.lines.ri = true;
    uart.tick(1);
    sub.strobe(0xA3);
    CHECK((sub.readAnswer() & 0x20) == 0);
    uart.setSerialLink(nullptr);
    sub.strobe(0xA3);
    CHECK((sub.readAnswer() & 0x20) != 0);
}

void test_serial_command_register_forwards_rts_dtr() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    uart.setSerialLink(&link);

    uart.writeRegister(3, 0x22);          // SCR: b1 DTR | b5 RTS
    CHECK(link.lastDtr);
    CHECK(link.lastRts);
    uart.writeRegister(3, 0x00);
    CHECK(!link.lastDtr);
    CHECK(!link.lastRts);
}

// The PC-1600 ties the chip's /CTS to GND (Service Manual §9-5), so the
// peer's CS never holds the transmitter, TxRDY or the Tx interrupt; only
// the ROM sees it, through PSR FAULT. SSR DSR is the RXD pin, not the
// peer's DSR.
void test_serial_peer_cs_does_not_gate_the_chip() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    link.lines.cts = false;
    link.lines.dsr = true;
    uart.setSerialLink(&link);
    uart.writeRegister(3, 0x05);          // TxEN | RxEN
    uart.writeRegister(0, 'Z');

    uart.tick(kDefaultCharTStates);
    CHECK(link.tx.size() == 1);           // CS off, the byte still goes out
    CHECK((uart.psr() & kPsrFAULT) != 0); // ...and the ROM can see CS off
    CHECK((uart.ssr() & 0x80) == 0);      // SSR DSR: RXD idles at mark

    uart.writeRegister(3, 0xC5);
    uart.writeRegister(2, 0x00);          // TxINTM clear: TxRDY = Tx int condition
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
    CHECK(uart.interruptOutput());
}

void test_no_link_preserves_standalone_behaviour() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    uart.writeRegister(0, 0x41);          // TxD -- instant "sent"
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK((uart.readRegister(0) & 0xFF) == 0xFF);
    uart.tick(100000);                    // must be a no-op with no peer
    CHECK((uart.readRegister(0) & 0xFF) == 0xFF);
    CHECK(uart.psr() == (kPsrFAULT | kPsrP5V));
}

void test_chip_reset_keeps_peer_attached() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    uart.reset();
    FakeLink link;
    uart.setSerialLink(&link);
    CHECK(uart.serialLink() == &link);
    uart.writeRegister(3, 0xE0);          // parameter-address chip reset (D5)
    CHECK(uart.serialLink() == &link);
    uart.setSerialLink(nullptr);
    CHECK(uart.serialLink() == nullptr);
}

// Detaching the peer mid-transmit must not leave TxRDY low: without a
// link tick() never drains the FIFO, so a ROM transmit poll would hang.
void test_detach_with_queued_bytes_restores_ready() {
    PC1600SubCpu sub;
    TC8576F uart(sub);
    bootInit(uart);
    FakeLink link;
    link.lines.dsr = true;
    uart.setSerialLink(&link);
    uart.writeRegister(3, 0x05);          // TxEN | RxEN
    uart.tick(kDefaultCharTStates);       // pick up DSR from the peer
    CHECK((uart.psr() & kPsrPE) != 0);
    for (int i = 0; i < 600; i++) uart.writeRegister(0, 0x41); // fill the FIFO
    CHECK((uart.ssr() & kSsrTxRDY) == 0);
    uart.setSerialLink(nullptr);
    CHECK((uart.ssr() & kSsrTxRDY) != 0);
    CHECK((uart.ssr() & kSsrTxE) != 0);
    CHECK((uart.psr() & kPsrPE) == 0);    // DSR back to the no-peer level
    uart.setSerialLink(&link);
    uart.tick(kDefaultCharTStates);
    CHECK(link.tx.size() <= 1);           // nothing stale left to send
}

} // namespace

int run_tc8576f_tests() {
    g_pass = g_fail = 0;
    test_reset_status_is_idle_ready();
    test_parameter_address_pointer_and_file();
    test_chip_reset_keeps_parameters_and_holds();
    test_serial_transmit_reports_sent_immediately();
    test_parallel_out_drives_subcpu_command_and_handshake();
    test_no_answer_command_acks_on_receipt();
    test_parallel_command_drives_prime_and_clears_xbusy();
    test_interrupt_output_follows_tx_equation();
    test_serial_tx_fifo_drains_one_byte_per_char_time();
    test_serial_rx_latches_and_flags_overrun();
    test_serial_receiver_needs_rxenable();
    test_serial_error_interrupt();
    test_serial_baud_divisor_sets_cadence_and_notifies_peer();
    test_serial_psr_carries_peer_modem_lines();
    test_serial_ci_reaches_the_subcpu();
    test_serial_command_register_forwards_rts_dtr();
    test_serial_peer_cs_does_not_gate_the_chip();
    test_no_link_preserves_standalone_behaviour();
    test_chip_reset_keeps_peer_attached();
    test_detach_with_queued_bytes_restores_ready();
    std::printf("tc8576f: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
