#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "SerialLink.hpp"

// ── Host pseudo-terminal backing for an emulated serial port ────────────
//
// Opens a PTY. The emulator drives the master end through the SerialLink
// interface; a Mac serial application -- SharpDataExchange, `screen`, a
// terminal program -- opens the slave end at `slavePath()`, or the stable
// symlink at `stablePath()` (`<linkDir>/<linkName>`), which survives
// across runs even though the /dev/ttysNNN name does not. Each emulated
// port passes its own `linkName` so several can share one `linkDir`: the
// PC-1600's built-in port is `kPC1600LinkName`, the CE-158 interface
// `kCE158LinkName`.
//
// `linkDir` (the constructor argument) is the directory the symlink is
// created in -- the host app passes a folder the user has granted (see
// AppSettings.serialLinkDirectory()); an empty string, used by the
// headless probe and any other non-app caller, falls back to
// ~/Library/Application Support/Calc-U-1600.
//
// Two I/O threads own the master fd: a reader draining it into an RX ring
// (with a high-water mark that lets the pty buffer fill and back-pressure
// a well-behaved sender -- the PTY substitute for a real RTS drop), and a
// writer flushing a TX ring. `poll()` / `send()` are the emulation
// thread's lock-guarded view of those rings.
//
// A raw PTY carries no RS-232C modem lines, so `getStatus()` reports CTS
// and DSR permanently asserted; DCD approximates "a peer currently holds
// the slave open". POSIX only -- on a non-POSIX build every operation is
// a no-op and `isOpen()` is false.
class PtySerialLink final : public SerialLink {
public:
    static constexpr const char* kPC1600LinkName = "calcu1600.serial";
    static constexpr const char* kCE158LinkName = "calcu1600-ce158.serial";

    explicit PtySerialLink(std::string linkDir = {}, std::string linkName = kPC1600LinkName);
    ~PtySerialLink() override;
    PtySerialLink(const PtySerialLink&) = delete;
    PtySerialLink& operator=(const PtySerialLink&) = delete;

    bool isOpen() const { return m_master >= 0; }
    const std::string& slavePath() const { return m_slavePath; }
    const std::string& lastError() const { return m_error; }
    // Returned by value: `relink()` can rewrite the stable path from
    // another thread. Empty stable path => none could be made.
    std::string stablePath() const;
    // The path a caller should show/use: the stable symlink when one was
    // made, else the raw (renumbering-on-relaunch) slave path.
    std::string preferredPath() const;

    // Re-create the stable symlink in `newDir` (empty => the App Support
    // fallback), removing the old one. Call when the user picks a
    // different link folder while the port is already live. Returns true
    // if the new symlink was made. Safe to call from the host UI thread
    // while the I/O threads run.
    bool relink(std::string newDir);

    // ── SerialLink ────────────────────────────────────────────────────
    bool poll(uint8_t& out) override;
    void send(uint8_t byte) override;
    void getStatus(Lines& in) override;

private:
    void readerLoop();
    void writerLoop();

    int m_master = -1;
    std::string m_slavePath;             // set once in the ctor
    std::string m_error;                 // set once in the ctor
    // m_linkDir / m_stablePath are rewritten by relink() -- guarded by
    // m_pathMx against the host UI thread reading stablePath()/preferredPath().
    mutable std::mutex m_pathMx;
    std::string m_linkDir;               // directory for the stable symlink; "" = App Support fallback
    std::string m_linkName;              // the symlink's file name inside m_linkDir; set once in the ctor
    std::string m_stablePath;

    std::thread m_reader;
    std::thread m_writer;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_peerOpen{true};

    std::mutex m_rxMx;
    std::deque<uint8_t> m_rxRing;
    static constexpr std::size_t kRxHighWater = 8192;

    std::mutex m_txMx;
    std::condition_variable m_txCv;
    std::deque<uint8_t> m_txRing;
};
