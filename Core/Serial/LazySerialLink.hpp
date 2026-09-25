#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "SerialLink.hpp"

// ── A SerialLink that builds its real transport on first use ────────────
//
// Lets a host hand every machine it builds the same link up front while
// the transport itself (e.g. a PtySerialLink, which opens a host PTY and
// its symlink) only comes into being once an emulated port actually talks
// to it -- a card that is never attached never touches it, so never opens
// anything. Once built, `Link` lives as long as this object.
template <class Link>
class LazySerialLink final : public SerialLink {
public:
    explicit LazySerialLink(std::function<std::unique_ptr<Link>()> make) : m_make(std::move(make)) {}

    /// The transport, or nullptr while nothing has used the link yet.
    Link* link() const { return m_link.get(); }

    bool poll(uint8_t& out) override { return open().poll(out); }
    void send(uint8_t byte) override { open().send(byte); }
    void setControl(bool dtr, bool rts) override { open().setControl(dtr, rts); }
    void getStatus(Lines& in) override { open().getStatus(in); }
    void onBaud(uint32_t baud, int bitsPerChar) override { open().onBaud(baud, bitsPerChar); }

private:
    Link& open() {
        if (!m_link) m_link = m_make();
        return *m_link;
    }

    std::function<std::unique_ptr<Link>()> m_make;
    std::unique_ptr<Link> m_link;
};
