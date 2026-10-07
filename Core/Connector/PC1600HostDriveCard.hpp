#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include "HostDirectoryDrive.hpp"
#include "SystemBusCard.hpp"

// ── Host-directory drive, attached to the PC-1600's 60-pin system bus ─────
//
// Calc-U-1600's own peripheral: a host directory as file device "S3:"
// (alias "Y:"). Real PC-1600 hardware has the same shape in the MEP rev3
// module (ROM at page-1 bank 7, a microcontroller behind I/O port 90H); this
// card uses the same bank and its own protocol. docs/PC1600-Host-Drive.md.
//
// This card claims:
//   - Page B bank 7 ROM read window (4000-7FFF): the driver ROM
//     (firmware/pc1600-hostdrive/, PC1600-P1-B7-HOSTDRIVE.bin).
//   - I/O port 0x91 write: start a transaction with the FILE function byte
//     (FFH = module reset: the +02H function code follows), or one of the
//     MEP fixed entries: FEH DIRMODE / FDH FILEMODE (nothing follows), FCH
//     CDIR (length and path follow; status, ERL and on success the 27-byte
//     prompt back).
//   - I/O port 0x90 write: the request stream -- FCB address (DE) lo/hi,
//     DEVNAME, FCB+00H..+38H, and for SEQUENTIAL WRITE the 256-byte record.
//     The last byte runs the call on HostDirectoryDrive.
//   - I/O port 0x90 read: the response stream -- status, ERL, FCB+00H..+38H,
//     payload length lo/hi, the payload (for the DMA buffer), BC, DE, HL
//     (lo/hi each). Reads past the end, or with nothing pending, give FFH.
//   - I/O port 0x91 read: 00H (the host is always ready -- each call
//     completes within the OUT that ends its request).
class PC1600HostDriveCard : public SystemBusCard {
public:
    static constexpr size_t kRomSize = 0x4000;
    static constexpr uint8_t kRomBank = 7;
    static constexpr uint8_t kDataPort = 0x90;
    static constexpr uint8_t kCommandPort = 0x91;
    static constexpr uint8_t kResetCommand = 0xFF;
    static constexpr uint8_t kDirModeCommand = 0xFE;
    static constexpr uint8_t kFileModeCommand = 0xFD;
    static constexpr uint8_t kChangeDirCommand = 0xFC;

    bool loadRom(const uint8_t* data, size_t size) {
        if (size != kRomSize) return false;
        std::memcpy(m_rom.data(), data, kRomSize);
        m_romLoaded = true;
        return true;
    }

    HostDirectoryDrive& drive() { return m_drive; }
    const HostDirectoryDrive& drive() const { return m_drive; }

    bool respondsToRead(const SystemBusPins& pins, uint8_t& outValue) const override {
        if (Sc7852Decode::page1Memory(pins)) {
            if (!m_romLoaded || pins.forWrite || Sc7852Decode::bank(pins) != kRomBank) return false;
            outValue = m_rom[pins.address & 0x3FFF];
            return true;
        }
        if (!Sc7852Decode::io(pins)) return false;
        const uint8_t port = Sc7852Decode::port(pins);
        if (port == kCommandPort) {
            outValue = 0x00;
            return true;
        }
        if (port != kDataPort) return false;
        outValue = m_txPos < m_tx.size() ? m_tx[m_txPos++] : 0xFF;
        return true;
    }

    WriteResult respondsToWrite(const SystemBusPins& pins, uint8_t value) override {
        if (!Sc7852Decode::io(pins)) return WriteResult::ignored();  // ROM window: read-only
        const uint8_t port = Sc7852Decode::port(pins);
        if (port == kCommandPort) {
            begin(value);
            return WriteResult::taken();
        }
        if (port != kDataPort) return WriteResult::ignored();
        if (m_rx.size() < m_rxExpected) {
            m_rx.push_back(value);
            if (m_function == kChangeDirCommand && m_rx.size() == 1) m_rxExpected += value;  // the path length
            if (m_rx.size() == m_rxExpected) {
                run();
                m_rx.clear();
                m_rxExpected = 0;
                m_txPos = 0;
            }
        }
        return WriteResult::taken();
    }

private:
    static constexpr size_t kRequestHeader = 3;  // DE lo, DE hi, DEVNAME

    void begin(uint8_t function) {
        m_rx.clear();
        m_tx.clear();
        m_txPos = 0;
        m_rxExpected = 0;
        m_function = function;
        switch (function) {
            case kResetCommand: m_rxExpected = 1; return;
            case kDirModeCommand: m_drive.setListDirectories(true); return;
            case kFileModeCommand: m_drive.setListDirectories(false); return;
            case kChangeDirCommand: m_rxExpected = 1; return;
            default: break;
        }
        m_rxExpected = kRequestHeader + HostDirectoryDrive::kFcbImageSize +
                       (function == HostDirectoryDrive::kSeqWrite ? HostDirectoryDrive::kRecordSize : 0);
    }

    // Runs the complete request; respondsToWrite() then resets the transfer.
    void run() {
        if (m_function == kResetCommand) {
            m_drive.reset(m_rx[0]);
            return;
        }
        if (m_function == kChangeDirCommand) {
            const auto r = m_drive.changeDirectory(std::string(m_rx.begin() + 1, m_rx.end()));
            m_tx = {r.status, r.erl};
            m_tx.insert(m_tx.end(), r.payload.begin(), r.payload.end());
            return;
        }
        HostDirectoryDrive::Request req;
        req.function = m_function;
        req.fcbAddress = static_cast<uint16_t>(m_rx[0] | (m_rx[1] << 8));
        req.device = m_rx[2];
        std::memcpy(req.fcb.data(), &m_rx[kRequestHeader], req.fcb.size());
        if (m_function == HostDirectoryDrive::kSeqWrite)
            std::memcpy(req.data.data(), &m_rx[kRequestHeader + req.fcb.size()], req.data.size());
        const auto r = m_drive.execute(req);

        m_tx = {r.status, r.erl};
        m_tx.insert(m_tx.end(), r.fcb.begin(), r.fcb.end());
        const auto len = static_cast<uint16_t>(r.payload.size());
        m_tx.push_back(static_cast<uint8_t>(len));
        m_tx.push_back(static_cast<uint8_t>(len >> 8));
        m_tx.insert(m_tx.end(), r.payload.begin(), r.payload.end());
        for (uint16_t reg : {r.bc, r.de, r.hl}) {
            m_tx.push_back(static_cast<uint8_t>(reg));
            m_tx.push_back(static_cast<uint8_t>(reg >> 8));
        }
    }

    std::array<uint8_t, kRomSize> m_rom{};
    bool m_romLoaded = false;
    HostDirectoryDrive m_drive;
    uint8_t m_function = 0;
    std::vector<uint8_t> m_rx;
    size_t m_rxExpected = 0;
    std::vector<uint8_t> m_tx;
    mutable size_t m_txPos = 0;  // the response is consumed by reads
};
