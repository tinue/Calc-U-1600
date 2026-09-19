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
//   base+0 (0x78/0x70) -- command/status. Commands (bank-5 Z80 addresses):
//     0x01 = restore/seek track 0 (0x449b), 0x20 = seek to the track held
//     in the DATA register (0x4933-0x494c), 0x40 = read sector (0x482e),
//     0x60 = write sector (0x4892), 0x80 = read ID (0x4979: three bytes,
//     the first compared against the seek target -- see readIdByte()),
//     0xA0 = format track (0x4555: eight 3-byte ID fields track/sector/1
//     written through DATA). Read: bit7 = engine-not-started, bit6 =
//     no-write-protect, bit3 = disk-in-drive.
//   base+1 (0x79/0x71) -- sector register, 0-7 within the current track
//     (the track comes from the head position the seek commands set;
//     also used by the ROM's CNCTDRV drive-presence probe: write then
//     read back).
//   base+2 (0x7A/0x72) -- motor/status. Write bit7=1 starts the motor.
//     Read: bit7/bit0 = busy, bit1 = data request (a byte is ready /
//     wanted on DATA), bit4 = record not found, bit2 = lost data, bit6 =
//     changed-disk (see markDiskChanged()'s comment).
//   base+3 (0x7B/0x73) -- data. During a transfer command, one byte per
//     access; otherwise a plain latch (the seek target).
//
// Busy semantics, from the ROM's transfer loops (e.g. 0x4841-0x4866):
// a transfer command stays busy *while* its bytes are moved -- the loop
// treats busy dropping mid-transfer as an error (jump to 0x4881) -- and
// must go idle within the short `sub_44e4` b=1 poll (~9ms) after the last
// byte. Seek/restore commands instead stay busy for the real mechanical
// step time (advance()).
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
    static constexpr size_t kTracksPerSide = 16;
    static constexpr size_t kSectorsPerTrack = 8;
    static constexpr size_t kSectorCount = kTracksPerSide * kSectorsPerTrack;  // 128 per side
    static constexpr size_t kSideSize = kSectorSize * kSectorCount;  // 65536
    static constexpr size_t kImageSize = kSideSize * 2;              // 131072, both sides

    // Mirrors PC1600Machine::kTStateHz (static_assert'd there -- this
    // header can't include PC1600Machine.hpp, which includes it). Real FDU-250 timing, from the
    // Service Manual's own spec sheet (§1 "Access time"/"Motor startup
    // time"): 0.5s to spin up, 80ms per step + 50ms settling per seek.
    // Seeks tick down in SC7852 T-states (advance()), driving the same
    // base+0 bit7 ("engine not started")/base+2 bit0+bit7 (busy) the ROM
    // polls. This is the "green lamp" duration a real user would watch
    // for before ejecting/flipping the disk.
    static constexpr uint32_t kTStateHz = 3580000;
    static constexpr uint32_t kMotorStartupTStates = kTStateHz / 2;          // 0.5s
    static constexpr uint32_t kStepTStates = (kTStateHz * 8) / 100;          // 80ms per track
    static constexpr uint32_t kSettleTStates = (kTStateHz * 5) / 100;        // 50ms head settling
    static constexpr uint32_t kStepAccessTStates = kStepTStates + kSettleTStates;  // one-track seek
    // A transfer command the ROM abandons part-way (e.g. the verify-compare
    // bail-out at 0x4909, which then polls up to ~35ms) completes on its
    // own once the sector has passed under the head.
    static constexpr uint32_t kTransferIdleTStates = kTStateHz / 50;         // 20ms

    CE1600FCard() = default;  // drive starts empty -- see hasDisk()

    /// Called once per emulated SC7852 instruction (PC1600Machine::step()'s
    /// `cost`) so the motor-startup and seek busy windows above tick down
    /// in real emulated time rather than completing instantly.
    void advance(uint32_t tStates) {
        if (m_motorStartRemaining > 0) {
            m_motorStartRemaining = (tStates >= m_motorStartRemaining) ? 0 : m_motorStartRemaining - tStates;
        }
        if (m_busyRemaining > 0) {
            if (tStates >= m_busyRemaining) {
                m_busyRemaining = 0;
                if (m_xfer != Xfer::None) m_status |= kStatusLostData;  // sector passed, bytes left
                finishCommand();
            } else {
                m_busyRemaining -= tStates;
            }
        }
    }

    /// Inserts a zero-filled (unformatted) disk, side A up.
    void insertBlankDisk() {
        m_image.fill(0);
        onDiskChange(true);
    }

    /// Removes the disk: base+0 bit3 (disk-in-drive) reads 0, which the
    /// ROM's motor-start/restore/seek paths (bank-5 0x4423, 0x4495, 0x4941)
    /// turn into "no disk" (ERROR 160). Also a disk-change event.
    void ejectDisk() {
        m_image.fill(0);
        onDiskChange(false);
    }

    bool hasDisk() const { return m_diskPresent; }

    /// `data` must be exactly kImageSize bytes (both sides). Resets to
    /// side A.
    bool loadImage(const uint8_t* data, size_t size) {
        if (size != kImageSize) return false;
        std::memcpy(m_image.data(), data, kImageSize);
        onDiskChange(true);
        return true;
    }

    std::vector<uint8_t> imageForSave() const {
        return std::vector<uint8_t>(m_image.begin(), m_image.end());
    }

    uint64_t revision() const { return m_revision; }

    /// 0 = side A, 1 = side B. Selecting a *different* side is itself a
    /// disk-change event on real hardware (the drive door has to be
    /// opened to flip a physical disk), so it re-arms the changed-disk
    /// latch exactly like insertBlankDisk()/loadImage() -- see
    /// markDiskChanged()'s comment. Re-selecting the side already active
    /// is a no-op (no spurious change event).
    void setSide(int side) {
        const int clamped = side != 0 ? 1 : 0;
        if (!m_diskPresent || clamped == m_side) return;
        m_side = clamped;
        markDiskChanged();
        ++m_revision;
    }
    int side() const { return m_side; }

    /// Current head position (track 0-15), as set by the seek commands.
    int track() const { return m_track; }

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
    // base+2 status bits (see the class comment).
    static constexpr uint8_t kStatusBusy = 0x81;
    static constexpr uint8_t kStatusDataRequest = 0x02;
    static constexpr uint8_t kStatusLostData = 0x04;
    static constexpr uint8_t kStatusNotFound = 0x10;
    static constexpr uint8_t kStatusDiskChanged = 0x40;

    static constexpr size_t kIdFieldSize = 3;  // track, sector, size code
    static constexpr uint8_t kIdSizeCode = 1;  // what FORMAT writes (0x4551 `ld d,001h`)

    enum class Xfer { None, ReadSector, WriteSector, ReadId, FormatTrack };

    // Ports 0x78-0x7B (drive X:) and 0x70-0x73 (drive Y:) both index the
    // one physical drive's state below -- offset = address & 0x03, using
    // only the low two bits the ROM's base+0..+3 addressing actually uses.
    static uint8_t regOffset(uint8_t address) { return address & 0x03; }

    /// Arms the changed-disk latch (base+2 bit6) -- modeled on a real
    /// floppy controller's DSKCHG behavior: a physical door-open/insert
    /// event sets it, and it stays set until the next command (see
    /// writeRegister()'s case 0). Any insertBlankDisk()/loadImage()/
    /// setSide() call re-arms it, since each is a "new disk in the drive"
    /// event from the drive's perspective.
    void markDiskChanged() { m_diskChanged = true; }

    uint8_t readRegister(uint8_t address) const {
        switch (regOffset(address)) {
            case 0: {  // command/status
                uint8_t status = 0;
                // "Engine not started": true while the motor is off, or
                // while it's on but still within its kMotorStartupTStates
                // spin-up window (see advance()).
                if (!m_motorOn || m_motorStartRemaining > 0) status |= 0x80;
                status |= 0x40;  // never write-protected
                if (m_diskPresent) status |= 0x08;  // disk in drive
                return status;
            }
            case 1:
                return m_sectorReg;
            case 2: {
                uint8_t status = m_status;
                if (m_busy) status |= kStatusBusy;
                if (m_xfer != Xfer::None) status |= kStatusDataRequest;
                if (m_diskChanged) status |= kStatusDiskChanged;
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
            case 0:
                issueCommand(value);
                break;
            case 1:
                m_sectorReg = value;
                break;
            case 2:  // motor/status
                if ((value & 0x80) != 0 && !m_motorOn) {
                    // Off -> on transition: real spin-up time, per the
                    // Service Manual's "Motor startup time: 0.5 second".
                    m_motorStartRemaining = kMotorStartupTStates;
                }
                m_motorOn = (value & 0x80) != 0;
                if (!m_motorOn) m_motorStartRemaining = 0;
                break;
            case 3:
                writeDataByte(value);
                break;
            default:
                break;
        }
    }

    void issueCommand(uint8_t command) {
        m_status = 0;
        m_xfer = Xfer::None;
        m_byteOffset = 0;
        m_busy = true;
        // Confirmed by register-level tracing: the very first command
        // DSKINIT issues is 0x01 (restore), and the *next* thing it does
        // is poll base+2 -- whose bit6 (changed-disk) the caller treats as
        // a "media changed, abort" condition unless it's already clear. So
        // the latch clears on *any* command write.
        m_diskChanged = false;

        switch (command) {
            case 0x01:  // restore
                startSeek(0);
                break;
            case 0x20:  // seek to the track latched in DATA
                startSeek(m_dataReg);
                break;
            case 0x40:
                startTransfer(Xfer::ReadSector, kSectorSize);
                break;
            case 0x60:
                startTransfer(Xfer::WriteSector, kSectorSize);
                break;
            case 0x80:
                // The next ID field passing under the head.
                m_idSector = static_cast<uint8_t>((m_idSector + 1) % kSectorsPerTrack);
                startTransfer(Xfer::ReadId, kIdFieldSize);
                break;
            case 0xA0:
                startTransfer(Xfer::FormatTrack, kIdFieldSize * kSectorsPerTrack);
                break;
            default:  // unknown -- just occupy the drive for one access time
                m_busyRemaining = kStepAccessTStates;
                break;
        }
    }

    void startSeek(uint8_t target) {
        const int clamped = target < kTracksPerSide ? target : static_cast<int>(kTracksPerSide) - 1;
        const int distance = clamped > m_track ? clamped - m_track : m_track - clamped;
        m_track = clamped;
        m_busyRemaining = kSettleTStates + static_cast<uint32_t>(distance > 0 ? distance : 1) * kStepTStates;
    }

    void startTransfer(Xfer kind, size_t length) const {
        // No disk: no ID field ever passes under the head.
        if (!m_diskPresent ||
            ((kind == Xfer::ReadSector || kind == Xfer::WriteSector) && m_sectorReg >= kSectorsPerTrack)) {
            m_status |= kStatusNotFound;
            finishCommand();
            return;
        }
        m_xfer = kind;
        m_xferRemaining = length;
        m_busyRemaining = kTransferIdleTStates;
    }

    /// Ends the current command: busy and data-request both drop.
    void finishCommand() const {
        m_busy = false;
        m_xfer = Xfer::None;
        m_xferRemaining = 0;
        m_busyRemaining = 0;
    }

    /// Accounts for one DATA-register byte of the running transfer.
    void consumeTransferByte() const {
        ++m_byteOffset;
        if (--m_xferRemaining == 0) {
            finishCommand();
        } else {
            m_busyRemaining = kTransferIdleTStates;
        }
    }

    size_t sectorBase() const {
        return static_cast<size_t>(m_side) * kSideSize +
               (static_cast<size_t>(m_track) * kSectorsPerTrack + m_sectorReg) * kSectorSize;
    }

    uint8_t readDataByte() const {
        uint8_t value = m_dataReg;
        switch (m_xfer) {
            case Xfer::ReadSector:
                value = m_image[sectorBase() + m_byteOffset];
                break;
            case Xfer::ReadId:
                value = readIdByte();
                break;
            default:
                return value;
        }
        consumeTransferByte();
        return value;
    }

    /// The ID field every formatted sector carries: track, sector, size
    /// code -- the same three bytes FORMAT writes per sector (0x4569-
    /// 0x4583). The seek-verify at 0x4976-0x49a9 compares the first one
    /// against the track it just sought to. The image stores sector data
    /// only, so every track reads back as formatted with standard IDs.
    uint8_t readIdByte() const {
        switch (m_byteOffset) {
            case 0: return static_cast<uint8_t>(m_track);
            case 1: return m_idSector;
            default: return kIdSizeCode;
        }
    }

    void writeDataByte(uint8_t value) {
        m_dataReg = value;
        switch (m_xfer) {
            case Xfer::WriteSector:
                m_image[sectorBase() + m_byteOffset] = value;
                ++m_revision;
                break;
            case Xfer::FormatTrack:
                // The ID bytes themselves aren't stored (see readIdByte());
                // the track's data is laid down once the last one is in.
                if (m_xferRemaining == 1) formatTrack();
                break;
            default:
                return;
        }
        consumeTransferByte();
    }

    void formatTrack() {
        const size_t base = static_cast<size_t>(m_side) * kSideSize +
                            static_cast<size_t>(m_track) * kSectorsPerTrack * kSectorSize;
        std::memset(m_image.data() + base, 0, kSectorsPerTrack * kSectorSize);
        ++m_revision;
    }

    // Shared tail of insertBlankDisk()/loadImage()/ejectDisk(): a disk goes
    // in side A up (or comes out), with the changed-disk latch armed.
    void onDiskChange(bool present) {
        m_diskPresent = present;
        m_side = 0;
        markDiskChanged();
        ++m_revision;
        resetLatchedState();
    }

    void resetLatchedState() {
        m_sectorReg = 0;
        m_dataReg = 0;
        m_motorOn = false;
        m_motorStartRemaining = 0;
        m_status = 0;
        finishCommand();
    }

    std::array<uint8_t, kImageSize> m_image{};
    uint8_t m_sectorReg = 0;
    uint8_t m_dataReg = 0;   // DATA latch outside transfers (the seek target)
    int m_track = 0;         // head position, 0..kTracksPerSide-1
    int m_side = 0;          // 0 = A, 1 = B
    bool m_motorOn = false;
    uint32_t m_motorStartRemaining = 0;  // T-states left in motor spin-up
    bool m_diskChanged = true;
    bool m_diskPresent = false;
    uint64_t m_revision = 0;
    // Command/transfer state. Mutable because reading DATA during a read
    // transfer advances it, and respondsToRead() is const.
    mutable bool m_busy = false;
    mutable uint8_t m_status = 0;             // error bits of the last command
    mutable Xfer m_xfer = Xfer::None;
    mutable size_t m_xferRemaining = 0;       // bytes left in the transfer
    mutable uint16_t m_byteOffset = 0;        // bytes moved so far
    mutable uint32_t m_busyRemaining = 0;     // seek time, or transfer idle timeout
    mutable uint8_t m_idSector = 0;           // sector of the last ID field read
};
