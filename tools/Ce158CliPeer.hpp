#pragma once
// Shared by pc1500_cli / pc1600_cli: the CE-158's serial peer for a
// headless run (--ce158-pty / --ce158-rx / --ce158-rx-hold / --ce158-tx)
// and the end-of-run report (Centronics output, serial traffic).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../Core/Serial/PtySerialLink.hpp"

// --ce158-rx / --ce158-tx: a file-backed serial peer for repeatable runs.
class FileSerialLink final : public SerialLink {
public:
    std::vector<uint8_t> rx;
    size_t rxPos = 0;
    std::vector<uint8_t> tx;
    long holdPolls = 0; // --ce158-rx-hold: character times to stay silent first
    bool poll(uint8_t& out) override {
        if (holdPolls > 0) { --holdPolls; return false; }
        if (rxPos >= rx.size()) return false;
        out = rx[rxPos++];
        return true;
    }
    void send(uint8_t byte) override { tx.push_back(byte); }
};

inline void printCe158Bytes(const char* title, const std::vector<uint8_t>& bytes) {
    std::printf("%s (%zu bytes):\n", title, bytes.size());
    std::string text;
    for (uint8_t b : bytes) {
        if (b == '\r') continue;
        if (b == '\n' || (b >= 0x20 && b < 0x7F)) text += static_cast<char>(b);
        else { char hex[8]; std::snprintf(hex, sizeof hex, "<%02X>", b); text += hex; }
    }
    std::printf("%s\n", text.c_str());
}

class Ce158CliPeer {
public:
    static constexpr const char* kUsage = "--ce158-pty | --ce158-rx <file> [--ce158-rx-hold <n>] --ce158-tx <file>";

    /// Consumes argv[i] (and its value) if it is a --ce158-* option.
    bool parseArg(int argc, char** argv, int& i) {
        if (std::strcmp(argv[i], "--ce158-pty") == 0) { m_pty = true; return true; }
        if (i + 1 >= argc) return false;
        if (std::strcmp(argv[i], "--ce158-rx") == 0) { m_rxPath = argv[++i]; return true; }
        if (std::strcmp(argv[i], "--ce158-tx") == 0) { m_txPath = argv[++i]; return true; }
        if (std::strcmp(argv[i], "--ce158-rx-hold") == 0) { m_hold = std::strtol(argv[++i], nullptr, 10); return true; }
        return false;
    }

    /// Hands the peer to `machine` (before the preset attaches the card).
    template <typename Machine>
    bool attach(Machine& machine) {
        if (m_pty) {
            m_ptyLink = std::make_unique<PtySerialLink>(std::string{}, PtySerialLink::kCE158LinkName);
            if (!m_ptyLink->isOpen()) {
                std::fprintf(stderr, "CE-158 PTY: %s\n", m_ptyLink->lastError().c_str());
                return false;
            }
            std::fprintf(stderr, "CE-158 serial port: %s\n", m_ptyLink->preferredPath().c_str());
            machine.setCE158SerialLink(m_ptyLink.get());
        } else if (!m_rxPath.empty() || !m_txPath.empty()) {
            if (!m_rxPath.empty()) {
                std::FILE* f = std::fopen(m_rxPath.c_str(), "rb");
                if (!f) { std::fprintf(stderr, "cannot read %s\n", m_rxPath.c_str()); return false; }
                int ch;
                while ((ch = std::fgetc(f)) != EOF) m_file.rx.push_back(static_cast<uint8_t>(ch));
                std::fclose(f);
            }
            m_file.holdPolls = m_hold;
            machine.setCE158SerialLink(&m_file);
        }
        return true;
    }

    /// Prints the CE-158 summary and detaches the peer.
    template <typename Machine>
    bool report(Machine& machine) {
        bool ok = true;
        if (machine.ce158Attached()) {
            std::printf("CE-158: attached, baud=%d, UART status=%02X\n", machine.ce158Card()->baudRate(),
                        machine.ce158Card()->uartStatus());
            printCe158Bytes("CE-158 Centronics output", machine.drainCE158ParallelOutput());
            if (!m_rxPath.empty())
                std::printf("CE-158 serial: received %zu of %zu scripted bytes\n", m_file.rxPos, m_file.rx.size());
            if (!m_txPath.empty() || !m_rxPath.empty()) printCe158Bytes("CE-158 serial output", m_file.tx);
            if (!m_txPath.empty()) {
                std::FILE* f = std::fopen(m_txPath.c_str(), "wb");
                if (!f) { std::fprintf(stderr, "cannot write %s\n", m_txPath.c_str()); ok = false; }
                else { std::fwrite(m_file.tx.data(), 1, m_file.tx.size(), f); std::fclose(f); }
            }
        }
        machine.setCE158SerialLink(nullptr);
        return ok;
    }

private:
    bool m_pty = false;
    std::string m_rxPath, m_txPath;
    long m_hold = 0;
    std::unique_ptr<PtySerialLink> m_ptyLink;
    FileSerialLink m_file;
};
