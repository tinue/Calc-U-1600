#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "PC1600SystemBus.hpp"

// ── CE-1600F floppy drive, docked onto the CE-1600P's 50-pin sub-port ────
//
// The CE-1600F carries no ROM of its own -- its driver software lives in
// the CE-1600P's bank-5 ROM (already loaded by CE1600PCard). This class
// therefore only emulates the hardware register interface that ROM code
// pokes, not any IOCS/FAT logic -- exactly the way CE1600PCard emulates
// the plotter's motor-phase ports rather than reimplementing PLOT/LPRINT.
//
// Register map confirmed by direct disassembly of the real, already-dumped
// roms/PC1600-P1-B5-CE1600P-OR-F.bin (see
// ~/Development/sharp/pc1600/disasm/z80/romce1600-2.asm -- not from any
// other emulator). The ROM addresses four registers off a per-drive base
// port (0x78 for drive X:, 0x70 for drive Y: -- this emulation has one
// physical drive, so both ranges alias the same state):
//   base+0 (0x78/0x70) -- command/status. Write 0x40=read sector,
//     0x60=write sector, 0xA0=format. Read: bit7 = engine-not-started,
//     bit6 = no-write-protect, bit3 = disk-in-drive.
//   base+1 (0x79/0x71) -- sector register, read/write, selects 0-127
//     (also used by the ROM's CNCTDRV drive-presence probe: write then
//     read back).
//   base+2 (0x7A/0x72) -- motor/status. Write bit7=1 starts the motor.
//     Read: bit7/bit0=busy (file offset 0x4e4/0x685 poll these clearing),
//     bit1=ready, bit6=changed-disk (see markDiskChanged()'s comment).
//   base+3 (0x7B/0x73) -- data, one byte per access at an
//     auto-incrementing offset into the selected 512-byte sector, within
//     whichever side is currently selected (see setSide()).
// Port 0x81 bit0 write (active-low, not base-relative) resets all latched
// command state -- confirmed at the same disassembly's file offset 0xf7
// (`xor a` / `out (081h),a`). This doesn't collide with CE1600PCard's own
// use of port 0x81, which only claims the *read* (pen-home bit).
//
// The real CE-1600F media is double-sided (Service Manual §1: "the floppy
// disk drive is for one-sided operation, though both sides of the media
// can be used" -- the head only reads one side at a time, and the user
// manual has the operator physically eject and flip the disk between
// formatting each side). This class models that as one 128KB image (two
// 64KB sides, back to back) plus a `m_side` selector standing in for
// "which physical side is currently facing the head" -- setSide() is the
// software analogue of the user ejecting, flipping, and reinserting the
// disk.
class CE1600FCard : public PC1600ExpansionCard {
public:
    static constexpr size_t kSectorSize = 512;
    static constexpr size_t kSectorCount = 128;              // 16 tracks * 8 sectors/track, per side
    static constexpr size_t kSideSize = kSectorSize * kSectorCount;  // 65536
    static constexpr size_t kImageSize = kSideSize * 2;              // 131072, both sides

    CE1600FCard() { insertBlankDisk(); }

    /// Zero-fills the disk image (an unformatted blank floppy), resets to
    /// side A, and bumps the revision -- the "auto-insert a blank disk"
    /// default.
    void insertBlankDisk() {
        m_image.fill(0);
        m_dirty = false;
        m_side = 0;
        markDiskChanged();
        ++m_revision;
        resetLatchedState();
    }

    /// `data` must be exactly kImageSize bytes (both sides). Resets to
    /// side A.
    bool loadImage(const uint8_t* data, size_t size) {
        if (size != kImageSize) return false;
        std::memcpy(m_image.data(), data, kImageSize);
        m_dirty = false;
        m_side = 0;
        markDiskChanged();
        ++m_revision;
        resetLatchedState();
        return true;
    }

    std::vector<uint8_t> imageForSave() const {
        return std::vector<uint8_t>(m_image.begin(), m_image.end());
    }

    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }
    uint64_t revision() const { return m_revision; }

    /// 0 = side A, 1 = side B. Selecting a *different* side is itself a
    /// disk-change event on real hardware (the drive door has to be
    /// opened to flip a physical disk), so it re-arms the changed-disk
    /// latch exactly like insertBlankDisk()/loadImage() -- see
    /// markDiskChanged()'s comment. Re-selecting the side already active
    /// is a no-op (no spurious change event).
    void setSide(int side) {
        const int clamped = side != 0 ? 1 : 0;
        if (clamped == m_side) return;
        m_side = clamped;
        markDiskChanged();
        ++m_revision;
    }
    int side() const { return m_side; }

    /// Drive-active indicator ("the green lamp") -- true while the motor
    /// is spinning, so a GUI can show the user when it's safe to eject
    /// and flip the disk (per the user manual: "When the green lamp goes
    /// off, eject the disk, turn it over").
    bool motorOn() const { return m_motorOn; }

    bool respondsToRead(const PC1600BusPins& pins, uint8_t& outValue) const override {
        if (!pins.io) return false;
        if (pins.address >= 0x70 && pins.address <= 0x7F) {
            outValue = readRegister(pins.address);
            return true;
        }
        return false;
    }

    bool respondsToWrite(const PC1600BusPins& pins, uint8_t value) override {
        if (!pins.io) return false;
        if (pins.address == 0x81) {
            if (!(value & 0x01)) resetLatchedState();  // active-low FD reset
            return true;
        }
        if (pins.address >= 0x70 && pins.address <= 0x7F) {
            writeRegister(pins.address, value);
            return true;
        }
        return false;
    }

private:
    // Ports 0x78-0x7B (drive X:) and 0x70-0x73 (drive Y:) both index the
    // one physical drive's state below -- offset = address & 0x03, using
    // only the low two bits the ROM's base+0..+3 addressing actually uses.
    static uint8_t regOffset(uint8_t address) { return address & 0x03; }

    /// Arms the changed-disk latch (base+2 bit6) -- modeled on a real
    /// floppy controller's DSKCHG behavior: a physical door-open/insert
    /// event sets it, and it stays set until the drive sees a step pulse
    /// (this ROM's command 0x20 -- see writeRegister()'s case 0), which
    /// is the software's standard "I saw the change, go look again"
    /// acknowledgement. Any insertBlankDisk()/loadImage()/setSide() call
    /// re-arms it, since each is a "new disk in the drive" event from the
    /// drive's perspective.
    void markDiskChanged() { m_diskChanged = true; }

    uint8_t readRegister(uint8_t address) const {
        switch (regOffset(address)) {
            case 0: {  // command/status
                uint8_t status = 0;
                if (!m_motorOn) status |= 0x80;  // engine not started
                if (!m_writeProtect) status |= 0x40;
                status |= 0x08;  // disk always present (auto-inserted blank)
                return status;
            }
            case 1:
                return m_sectorReg;
            case 2: {
                // motor/status. bit0/bit7 = busy (file offset 0x4e4/0x685
                // poll these clearing after a command); bit1 = ready;
                // bit6 = changed-disk (see markDiskChanged()).
                uint8_t status = 0;
                if (m_busy) status |= 0x81;
                status |= 0x02;
                if (m_diskChanged) status |= 0x40;
                return status;
            }
            case 3:
                return readDataByte();
            default:
                return 0xFF;
        }
    }

    void writeRegister(uint8_t address, uint8_t value) {
        switch (regOffset(address)) {
            case 0:  // command/status
                m_cmdReg = value;
                m_byteOffset = 0;
                m_busy = false;  // this emulation completes commands instantly
                if (value == 0xA0) formatDisk();
                // 0x20 is the step-pulse command (confirmed by disassembly,
                // file offset 0x94a/0x94c) -- a real drive's DSKCHG latch
                // clears the instant it sees a step, so this does too.
                if (value == 0x20) m_diskChanged = false;
                break;
            case 1:
                m_sectorReg = value;
                m_byteOffset = 0;
                break;
            case 2:  // motor/status
                m_motorOn = (value & 0x80) != 0;
                break;
            case 3:
                writeDataByte(value);
                break;
            default:
                break;
        }
    }

    size_t sideBase() const { return static_cast<size_t>(m_side) * kSideSize; }

    uint8_t readDataByte() const {
        const size_t index = sideBase() + static_cast<size_t>(m_sectorReg) * kSectorSize + m_byteOffset;
        uint8_t value = (index < kImageSize) ? m_image[index] : 0xFF;
        if (m_byteOffset + 1 < kSectorSize) ++m_byteOffset;
        return value;
    }

    void writeDataByte(uint8_t value) {
        const size_t index = sideBase() + static_cast<size_t>(m_sectorReg) * kSectorSize + m_byteOffset;
        if (index < kImageSize) {
            m_image[index] = value;
            m_dirty = true;
            ++m_revision;
        }
        if (m_byteOffset + 1 < kSectorSize) ++m_byteOffset;
    }

    void formatDisk() {
        const size_t base = sideBase() + static_cast<size_t>(m_sectorReg) * kSectorSize;
        if (base + kSectorSize <= kImageSize) {
            std::memset(m_image.data() + base, 0, kSectorSize);
            m_dirty = true;
            ++m_revision;
        }
    }

    void resetLatchedState() {
        m_cmdReg = 0;
        m_sectorReg = 0;
        m_byteOffset = 0;
        m_motorOn = false;
        m_busy = false;
    }

    std::array<uint8_t, kImageSize> m_image{};
    mutable uint16_t m_byteOffset = 0;  // read/write both auto-increment it
    uint8_t m_cmdReg = 0;
    uint8_t m_sectorReg = 0;
    int m_side = 0;  // 0 = A, 1 = B
    bool m_motorOn = false;
    bool m_busy = false;
    bool m_writeProtect = false;
    bool m_dirty = false;
    bool m_diskChanged = true;
    uint64_t m_revision = 0;
};
