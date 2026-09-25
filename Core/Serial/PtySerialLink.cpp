#include "PtySerialLink.hpp"

#if defined(__APPLE__) || defined(__unix__)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <chrono>
#include <vector>

namespace {

// How long the reader waits before re-polling when poll() would return at
// once without anything to do: RX ring full, or no peer on the slave.
constexpr std::chrono::milliseconds kIdleBackoff{10};

// Best-effort <dir>/<name> -> target. When `dir` is empty the
// caller supplied no folder (the headless probe, tests): fall back to
// ~/Library/Application Support/Calc-U-1600. Returns the link path, or ""
// if it could not be created.
std::string makeStableSymlink(const std::string& target, const std::string& dir, const std::string& name) {
    std::string base = dir;
    if (base.empty()) {
        const char* home = ::getenv("HOME");
        if (!home || !*home) return "";
        const std::string support = std::string(home) + "/Library/Application Support";
        ::mkdir(support.c_str(), 0755); // ignore EEXIST
        base = support + "/Calc-U-1600";
    }
    ::mkdir(base.c_str(), 0755); // ignore EEXIST -- a user-granted folder already exists
    // A folder given as "/private/tmp/" must not yield "/private/tmp//...".
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (base == "/") base.clear();
    const std::string link = base + "/" + name;
    ::unlink(link.c_str());
    if (::symlink(target.c_str(), link.c_str()) != 0) return "";
    return link;
}

// Remove `link` only if it is still a symlink pointing at `slave` -- so a
// relink or teardown never deletes a file the user (or a stale run) put
// there. Best-effort: silently does nothing if the unlink is denied (e.g.
// a custom folder whose sandbox scope is no longer held).
void removeSymlinkIfOurs(const std::string& link, const std::string& slave) {
    if (link.empty()) return;
    char buf[1024];
    ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (slave == buf) ::unlink(link.c_str());
    }
}

} // namespace

PtySerialLink::PtySerialLink(std::string linkDir, std::string linkName)
    : m_linkDir(std::move(linkDir)), m_linkName(std::move(linkName)) {
    m_master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (m_master < 0) {
        m_error = std::string("posix_openpt: ") + std::strerror(errno);
        return;
    }
    if (::grantpt(m_master) != 0 || ::unlockpt(m_master) != 0) {
        m_error = std::string("grantpt/unlockpt: ") + std::strerror(errno);
        ::close(m_master);
        m_master = -1;
        return;
    }
    const char* sp = ::ptsname(m_master); // static storage -- copy now
    if (!sp) {
        m_error = "ptsname failed";
        ::close(m_master);
        m_master = -1;
        return;
    }
    m_slavePath = sp;

    int fl = ::fcntl(m_master, F_GETFL, 0);
    if (fl >= 0) ::fcntl(m_master, F_SETFL, fl | O_NONBLOCK);

    // Raw line discipline (no echo, no CR/LF or parity mangling) as a sane
    // default. A serial library that opens the slave will set its own
    // termios on top; setting it on the master avoids a phantom
    // open/close of the slave (which DCD detection would misread).
    struct termios t;
    if (::tcgetattr(m_master, &t) == 0) {
        ::cfmakeraw(&t);
        ::tcsetattr(m_master, TCSANOW, &t);
    }

    m_stablePath = makeStableSymlink(m_slavePath, m_linkDir, m_linkName);

    m_reader = std::thread(&PtySerialLink::readerLoop, this);
    m_writer = std::thread(&PtySerialLink::writerLoop, this);
}

PtySerialLink::~PtySerialLink() {
    m_stop.store(true, std::memory_order_relaxed);
    m_txCv.notify_all();
    if (m_reader.joinable()) m_reader.join();
    if (m_writer.joinable()) m_writer.join();

    removeSymlinkIfOurs(m_stablePath, m_slavePath);
    if (m_master >= 0) ::close(m_master);
    m_master = -1;
}

std::string PtySerialLink::stablePath() const {
    std::lock_guard<std::mutex> lk(m_pathMx);
    return m_stablePath;
}

std::string PtySerialLink::preferredPath() const {
    std::lock_guard<std::mutex> lk(m_pathMx);
    return m_stablePath.empty() ? m_slavePath : m_stablePath;
}

bool PtySerialLink::relink(std::string newDir) {
    if (m_master < 0) return false; // never opened -- nothing to link
    std::lock_guard<std::mutex> lk(m_pathMx);
    if (newDir == m_linkDir && !m_stablePath.empty()) return true; // unchanged
    removeSymlinkIfOurs(m_stablePath, m_slavePath);
    m_linkDir = std::move(newDir);
    m_stablePath = makeStableSymlink(m_slavePath, m_linkDir, m_linkName);
    return !m_stablePath.empty();
}

void PtySerialLink::readerLoop() {
    uint8_t buf[512];
    while (!m_stop.load(std::memory_order_relaxed)) {
        struct pollfd pfd;
        pfd.fd = m_master;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 200);
        if (m_stop.load(std::memory_order_relaxed)) break;
        if (pr <= 0) continue; // timeout / EINTR

        // Back-pressure: while the RX ring is deep, leave the bytes in the
        // pty buffer so a blocking sender stalls (stand-in for RTS drop).
        // poll() stays readable meanwhile, so back off instead of spinning
        // straight back into it.
        bool full;
        {
            std::lock_guard<std::mutex> lk(m_rxMx);
            full = m_rxRing.size() >= kRxHighWater;
        }
        if (full) {
            std::this_thread::sleep_for(kIdleBackoff);
            continue;
        }

        ssize_t n = ::read(m_master, buf, sizeof buf);
        if (n > 0) {
            m_peerOpen.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(m_rxMx);
            for (ssize_t i = 0; i < n; ++i) m_rxRing.push_back(buf[i]);
        } else if (n == 0 || errno == EIO) {
            // No process holds the slave open (EIO is the BSD/macOS
            // behaviour). poll() keeps reporting the hangup at once, so
            // back off until a peer opens the slave.
            m_peerOpen.store(false, std::memory_order_relaxed);
            std::this_thread::sleep_for(kIdleBackoff);
        } else if (errno == EAGAIN || errno == EINTR) {
            // spurious wakeup -- retry
        } else {
            break; // unexpected, fatal for this link
        }
    }
}

void PtySerialLink::writerLoop() {
    std::vector<uint8_t> chunk;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(m_txMx);
            m_txCv.wait(lk, [this] {
                return m_stop.load(std::memory_order_relaxed) || !m_txRing.empty();
            });
            if (m_stop.load(std::memory_order_relaxed) && m_txRing.empty()) return;
            chunk.assign(m_txRing.begin(), m_txRing.end());
            m_txRing.clear();
        }
        std::size_t off = 0;
        while (off < chunk.size() && !m_stop.load(std::memory_order_relaxed)) {
            ssize_t n = ::write(m_master, chunk.data() + off, chunk.size() - off);
            if (n > 0) {
                off += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                struct pollfd pfd;
                pfd.fd = m_master;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                ::poll(&pfd, 1, 200);
                continue;
            }
            break; // EIO (no reader) or fatal -- drop the rest of the chunk
        }
    }
}

bool PtySerialLink::poll(uint8_t& out) {
    std::lock_guard<std::mutex> lk(m_rxMx);
    if (m_rxRing.empty()) return false;
    out = m_rxRing.front();
    m_rxRing.pop_front();
    return true;
}

void PtySerialLink::send(uint8_t byte) {
    {
        std::lock_guard<std::mutex> lk(m_txMx);
        m_txRing.push_back(byte);
    }
    m_txCv.notify_one();
}

void PtySerialLink::getStatus(Lines& in) {
    in.cts = true;  // a raw PTY carries no modem lines
    in.dsr = true;
    in.dcd = m_peerOpen.load(std::memory_order_relaxed);
    in.ri = false;
}

#else // non-POSIX: inert stub

PtySerialLink::PtySerialLink(std::string linkDir, std::string linkName)
    : m_linkDir(std::move(linkDir)), m_linkName(std::move(linkName)) {
    m_error = "PTY serial link not supported on this platform";
}
PtySerialLink::~PtySerialLink() = default;
std::string PtySerialLink::stablePath() const { return m_stablePath; }
std::string PtySerialLink::preferredPath() const { return m_slavePath; }
bool PtySerialLink::relink(std::string) { return false; }
bool PtySerialLink::poll(uint8_t&) { return false; }
void PtySerialLink::send(uint8_t) {}
void PtySerialLink::getStatus(Lines& in) { in = Lines{}; }
void PtySerialLink::readerLoop() {}
void PtySerialLink::writerLoop() {}

#endif
