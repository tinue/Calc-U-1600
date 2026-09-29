#pragma once
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__APPLE__)
#include <unistd.h>  // chflags
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// ── Host directory as a PC-1600 file device (S3: / Y:) ────────────────────
//
// The file-level half of the host-directory drive: the PC1600HostDriveCard
// hands every PC-1600 FILE IOCS call (C = 0FH..23H, DE = FCB) its driver ROM
// forwards over the bus to execute(), and sends the response back. The
// semantics follow the ROM's own file devices -- the CE-1600F module
// (PC1600-P1-B5 FDFILEHND 4267H) and the RAM disk (PC1600-P1-B3 B3FILEHND
// 49B2H) -- so BASIC's record layer (PC1600-P1-B3B 7A20H-7C64H) sees the
// same FCB fields it gets from a real drive. docs/PC1600-Host-Drive.md has
// the full contract.
//
// Visible files: regular files whose name fits 8.3 using the characters the
// ROM's file-name parser accepts (FNCHAROK 79E2H: letters, digits,
// "`@#$%^&()-_{}'"), upper-cased. Long names and dotfiles are not shown.
// Subdirectories with such names are reachable the MEP rev3 way: a current
// directory (CDIR) that every file call works in, and a directory mode in
// which SEARCH FIRST/NEXT list subdirectories (docs/PC1600-Host-Drive.md). The year is not part of the PC-1600 clock: directory entries
// carry year 1986 like the ROM's own FATTIME; the host file keeps its real
// time stamp (the OS writes it on every change).
//
// Stateless file I/O: every READ/WRITE opens the host file, transfers one
// 256-byte record at FCB record number * 256 and closes it again, so a
// power cut or an emulator crash never leaves a host file half-buffered.
class HostDirectoryDrive {
public:
    static constexpr size_t kFcbImageSize = 0x39;   // FCB+00H..+38H (the buffer at +39H is not sent)
    static constexpr size_t kRecordSize = 256;
    static constexpr size_t kDirEntrySize = 32;

    // FILE functions (PC1600-P1-B5 B5FILETAB 4210H).
    enum Function : uint8_t {
        kOpen = 0x0F, kClose = 0x10, kSearchFirst = 0x11, kSearchNext = 0x12,
        kDelete = 0x13, kSeqRead = 0x14, kSeqWrite = 0x15, kCreate = 0x16,
        kRename = 0x17, kSetDma = 0x1A, kGetAlloc = 0x1B, kSetAttr = 0x1E,
        kGetLength = 0x23,
    };

    // ERL codes (F89BH) the ROM's file devices use.
    enum Erl : uint8_t {
        kErlOk = 0x00, kErlExists = 0x97, kErlNotFound = 0x98, kErlNoDevice = 0x9B,
        kErlWildcard = 0x9D, kErlUnsupported = 0x9E, kErlProtected = 0x9F,
        kErlNoMedia = 0xA0, kErlEof = 0xA2, kErlIo = 0xA3, kErlDiskFull = 0xA4,
    };

    // Directory-entry attribute bits (PC1600-P1-B3B K_SET 7645H).
    static constexpr uint8_t kAttrProtected = 0x01;
    static constexpr uint8_t kAttrInvisible = 0x02;
    // DIRMODE entries: no attribute bits. FAT's 10H would hide them, as
    // FILES skips every entry with a bit of DCH set (PC1600-P1-B3B K_FILES
    // 6AD5H), and the MEP's LDIR is FILES in directory mode.
    static constexpr uint8_t kAttrDirectory = 0x00;
    static constexpr uint8_t kAttrArchive = 0x20;

    // FCB offsets used here (PC1600-P1-B3B record layer, B5 FDOPEN 4B14H).
    static constexpr size_t kFcbCount = 0x06, kFcbName = 0x09, kFcbAttr = 0x14,
                            kFcbSize = 0x25, kFcbNewName = 0x29, kFcbDirIndex = 0x2D,
                            kFcbRecLen = 0x2E, kFcbRecord = 0x30, kFcbModified = 0x36;

    // DSKF (PC1600-P1-B3 713CH) shifts BC left by log2(E) in 16 bits, then
    // multiplies by HL: 512 x 64 x 65535 clusters is the most it can show.
    static constexpr uint16_t kBytesPerSector = 512;
    static constexpr uint8_t kSectorsPerCluster = 64;

    struct Request {
        uint8_t function = 0;
        uint16_t fcbAddress = 0;  // DE: identifies a search across SFIRST/SNEXT
        uint8_t device = 0;       // DEVNAME (FC16H): 42H S3:, 43H Y:
        std::array<uint8_t, kFcbImageSize> fcb{};
        std::array<uint8_t, kRecordSize> data{};  // (DMA) for SEQWRITE
    };

    struct Response {
        uint8_t status = 0;  // IOCS status returned in A (0 = OK)
        uint8_t erl = 0;
        std::array<uint8_t, kFcbImageSize> fcb{};
        std::vector<uint8_t> payload;  // copied to (DMA)
        uint16_t bc = 0, de = 0, hl = 0;
    };

    // Module reset / power functions (+02H, A): 0 NEW, 1 power on, 2 on
    // after APO, 3 power off, 4 APO, 5 boot search, 6 boot, 7 reset.
    enum ResetFunction : uint8_t {
        kResetNew = 0, kResetPowerOn = 1, kResetResume = 2, kResetPowerOff = 3,
        kResetApo = 4, kResetBootSearch = 5, kResetBoot = 6, kResetReset = 7,
    };

    static constexpr size_t kPromptSize = 27;  // MEPPROMPT FB10H: up to 26 characters + CR

    void setDirectory(const std::filesystem::path& dir) {
        m_dir = dir;
        m_cwd.clear();
        reset(kResetReset);
    }
    const std::filesystem::path& directory() const { return m_dir; }
    bool mounted() const { return !m_dir.empty(); }

    /// Module reset / power function `function`: drops every search in
    /// progress and goes back to listing files. Power on, the power-on after
    /// an APO and reset also go back to the root: a MEP loses its current
    /// directory with the calculator's power. NEW and boot keep it.
    void reset(uint8_t function) {
        m_searches.clear();
        m_listDirectories = false;
        if (function == kResetPowerOn || function == kResetResume || function == kResetReset) m_cwd.clear();
    }

    // ── MEP fixed entries (4020H CDIR, 4023H DIRMODE, 4026H FILEMODE) ──
    // Software written for the MEP rev3 module (FILEX, and the MEP's own
    // CDIR / LDIR statements, which our ROM also has) navigates with them.

    /// DIRMODE (true) / FILEMODE (false): what SEARCH FIRST/NEXT list.
    void setListDirectories(bool on) { m_listDirectories = on; }

    /// The current directory below the mounted folder, as host names.
    const std::vector<std::string>& currentDirectory() const { return m_cwd; }

    /// CDIR: UNIX-style `path` -- "/" starts at the root, "." stays, ".."
    /// goes up (and stays at the root), components are visible 8.3
    /// subdirectories matched without regard to case. On success the
    /// payload is the kPromptSize-byte prompt ("S3:/DEV/ASM>", CR, spaces);
    /// on failure the current directory is unchanged (01H, ERL 98H).
    Response changeDirectory(const std::string& path) {
        Response r;
        std::error_code ec;
        if (!mounted() || !std::filesystem::is_directory(m_dir, ec)) return fail(r, kErlNoMedia);
        std::vector<std::string> cwd = path.empty() || path[0] != '/' ? m_cwd : std::vector<std::string>{};
        size_t pos = 0;
        while (pos <= path.size()) {
            const size_t slash = std::min(path.find('/', pos), path.size());
            const std::string part = path.substr(pos, slash - pos);
            pos = slash + 1;
            if (part.empty() || part == ".") continue;
            if (part == "..") {
                if (!cwd.empty()) cwd.pop_back();
                continue;
            }
            const std::string name = toFcbName(part);
            Entry dir;
            if (name.empty() || !findIn(listing(pathOf(cwd), true), name, dir)) return fail(r, kErlNotFound);
            cwd.push_back(dir.path.filename().string());
        }
        if (!std::filesystem::is_directory(pathOf(cwd), ec)) return fail(r, kErlNotFound);
        m_cwd = std::move(cwd);
        const std::string prompt = promptText();
        r.payload.assign(kPromptSize, ' ');
        std::memcpy(r.payload.data(), prompt.data(), prompt.size());
        r.payload[prompt.size()] = 0x0D;
        return r;
    }

    Response execute(const Request& req) {
        Response r;
        r.fcb = req.fcb;
        r.bc = req.function;     // registers a call doesn't define: C = function,
        r.de = req.fcbAddress;   // DE = FCB, as the caller passed them
        if (req.function >= 0x80) return r;  // disk IOCS: not a file call, ignored like FDBADFN
        std::error_code ec;
        if (!mounted() || !std::filesystem::is_directory(m_dir, ec)) return fail(r, kErlNoMedia);
        // The current directory vanished on the host: nothing may land in
        // another folder, so every call fails until the next CDIR.
        if (!std::filesystem::is_directory(here(), ec)) return fail(r, kErlNotFound);
        switch (req.function) {
            case kOpen:        return doOpen(req, r);
            case kClose:       return doClose(req, r);
            case kSearchFirst: return doSearchFirst(req, r);
            case kSearchNext:  return doSearchNext(req, r);
            case kDelete:      return doDelete(req, r);
            case kSeqRead:     return doRead(req, r);
            case kSeqWrite:    return doWrite(req, r);
            case kCreate:      return doCreate(req, r);
            case kRename:      return doRename(req, r);
            case kSetDma:      return r;  // the ROM keeps FC46H itself
            case kGetAlloc:    return doGetAlloc(r);
            case kSetAttr:     return doSetAttr(req, r);
            case kGetLength:   return doGetLength(req, r);
            default:           return fail(r, kErlUnsupported);
        }
    }

    // ── Name mapping (public for the tests) ────────────────────────────

    /// The 11-byte FCB form ("NAME    EXT") of a host file name, or "" when
    /// the PC-1600 cannot address the file.
    static std::string toFcbName(const std::string& hostName) {
        const size_t dot = hostName.find('.');
        const std::string base = hostName.substr(0, dot);
        const std::string ext = dot == std::string::npos ? "" : hostName.substr(dot + 1);
        if (base.empty() || base.size() > 8 || ext.size() > 3) return {};
        if (ext.find('.') != std::string::npos) return {};
        std::string out(11, ' ');
        for (size_t i = 0; i < base.size(); ++i) {
            const char c = upper(base[i]);
            if (!nameCharOk(c)) return {};
            out[i] = c;
        }
        for (size_t i = 0; i < ext.size(); ++i) {
            const char c = upper(ext[i]);
            if (!nameCharOk(c)) return {};
            out[8 + i] = c;
        }
        return out;
    }

    /// Host file name ("NAME.EXT") for an 11-byte FCB name without wildcards.
    static std::string toHostName(const std::string& fcbName) {
        std::string base = fcbName.substr(0, 8), ext = fcbName.substr(8, 3);
        base.erase(base.find_last_not_of(' ') + 1);
        ext.erase(ext.find_last_not_of(' ') + 1);
        return ext.empty() ? base : base + "." + ext;
    }

    static bool matches(const std::string& pattern, const std::string& name) {
        for (size_t i = 0; i < 11; ++i)
            if (pattern[i] != '?' && pattern[i] != name[i]) return false;
        return true;
    }

    /// The date/time words of a directory entry (+16H time, +18H date);
    /// the year field is always 6 (1986), like the ROM's FATTIME.
    static void packTimestamp(std::time_t t, uint16_t& time, uint16_t& date) {
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
        date = static_cast<uint16_t>((6 << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    }

private:
    struct Entry {
        std::string fcbName;          // 11 bytes, upper case
        std::filesystem::path path;
        uint8_t attr = kAttrArchive;
        uint32_t size = 0;
        std::time_t mtime = 0;
    };

    struct Search {
        std::vector<Entry> entries;
        size_t next = 0;
    };

    static char upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

    static bool nameCharOk(char c) {
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
        return c != '\0' && std::strchr("`@#$%^&()-_{}'", c) != nullptr;
    }

    static std::string fcbName(const std::array<uint8_t, kFcbImageSize>& fcb, size_t offset) {
        std::string s(11, ' ');
        for (size_t i = 0; i < 11; ++i) s[i] = upper(static_cast<char>(fcb[offset + i]));
        return s;
    }

    static bool hasWildcard(const std::string& name) { return name.find('?') != std::string::npos; }

    static Response& fail(Response& r, uint8_t erl) {
        r.erl = erl;
        r.status = statusFor(erl);
        return r;
    }

    // Reading past the end: the floppy's internal 96H, reported as ERL A2H
    // with status bit 2 -- what FREADREC (7B8DH) and COPY (758CH) test.
    static Response& failEof(Response& r) {
        r.erl = kErlEof;
        r.status = 0x04;
        return r;
    }

    // IOCS status per ERL (PC1600-P1-B5 FDERRTAB 4254H).
    static uint8_t statusFor(uint8_t erl) {
        switch (erl) {
            case kErlOk:          return 0x00;
            case kErlNotFound:    return 0x01;
            case kErlDiskFull:    return 0x02;
            case kErlNoDevice:    return 0x80;
            case kErlUnsupported: return 0x10;
            case kErlProtected:   return 0x48;
            case kErlNoMedia:     return 0x20;
            default:              return 0x08;
        }
    }

    static uint8_t hostAttributes(const std::filesystem::path& p) {
        uint8_t attr = kAttrArchive;
        std::error_code ec;
        const auto perms = std::filesystem::status(p, ec).permissions();
        if (!ec && (perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
            attr |= kAttrProtected;
#if defined(__APPLE__)
        struct stat st{};
        if (::stat(p.c_str(), &st) == 0 && (st.st_flags & UF_HIDDEN)) attr |= kAttrInvisible;
#endif
        return attr;
    }

    static std::time_t hostMtime(const std::filesystem::path& p) {
        struct stat st{};
        return ::stat(p.string().c_str(), &st) == 0 ? st.st_mtime : std::time(nullptr);
    }

    /// The host folder for a current directory `cwd`.
    std::filesystem::path pathOf(const std::vector<std::string>& cwd) const {
        std::filesystem::path p = m_dir;
        for (const auto& part : cwd) p /= part;
        return p;
    }

    /// The host folder file calls work in.
    std::filesystem::path here() const { return pathOf(m_cwd); }

    /// "S3:/DEV/ASM>" in the 8.3 spelling; one that does not fit keeps the
    /// end of the path ("S3:../ASM>"). The MEP has the stick's volume name
    /// in front, which the manual asks to be "S3".
    std::string promptText() const {
        std::string path = "/";
        for (size_t i = 0; i < m_cwd.size(); ++i) path += (i ? "/" : "") + toHostName(toFcbName(m_cwd[i]));
        const size_t room = kPromptSize - 1 - std::string("S3:>").size();
        if (path.size() > room) path = ".." + path.substr(path.size() - (room - 2));
        return "S3:" + path + ">";
    }

    /// Every file (or, with `directories`, every subdirectory) of `dir` the
    /// PC-1600 can see, sorted by FCB name. A second host entry that folds
    /// to the same 8.3 name (case-sensitive file systems) is hidden; the
    /// exact upper-case spelling wins. Symlinked directories are left out,
    /// so a current directory never leaves the mounted folder.
    std::vector<Entry> listing(const std::filesystem::path& dir, bool directories = false) const {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code fec;
            const bool wanted = directories ? it->is_directory(fec) && !it->is_symlink(fec)
                                            : it->is_regular_file(fec);
            if (wanted) files.push_back(it->path());
        }
        // Exact upper-case spellings first, then by host name: the first
        // file claiming an 8.3 name keeps it.
        const auto rank = [](const std::filesystem::path& p) {
            const std::string host = p.filename().string();
            const std::string name = toFcbName(host);
            return std::make_pair(name.empty() || host != toHostName(name), host);
        };
        std::sort(files.begin(), files.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
        std::map<std::string, Entry> byName;
        for (const auto& path : files) {
            const std::string name = toFcbName(path.filename().string());
            if (name.empty() || byName.count(name)) continue;
            Entry e;
            e.fcbName = name;
            e.path = path;
            e.mtime = hostMtime(path);
            if (directories) {
                e.attr = kAttrDirectory;
            } else {
                e.attr = hostAttributes(path);
                std::error_code fec;
                const auto size = std::filesystem::file_size(path, fec);
                e.size = fec ? 0 : static_cast<uint32_t>(std::min<uintmax_t>(size, 0xFFFFFFFFu));
            }
            byName.emplace(name, std::move(e));
        }
        std::vector<Entry> out;
        out.reserve(byName.size());
        for (auto& [name, e] : byName) out.push_back(std::move(e));
        return out;
    }

    std::vector<Entry> find(const std::string& pattern, bool directories = false) const {
        std::vector<Entry> out;
        for (auto& e : listing(here(), directories))
            if (matches(pattern, e.fcbName)) out.push_back(e);
        return out;
    }

    static bool findIn(const std::vector<Entry>& entries, const std::string& name, Entry& out) {
        for (const auto& e : entries)
            if (e.fcbName == name) {
                out = e;
                return true;
            }
        return false;
    }

    bool findOne(const std::string& name, Entry& out) const {
        const auto found = find(name);
        if (found.empty()) return false;
        out = found.front();
        return true;
    }

    static std::array<uint8_t, kDirEntrySize> dirEntry(const Entry& e) {
        std::array<uint8_t, kDirEntrySize> d{};
        std::memcpy(d.data(), e.fcbName.data(), 11);
        if (d[0] == 0xE5) d[0] = 0x05;
        d[0x0B] = e.attr;
        uint16_t time = 0, date = 0;
        packTimestamp(e.mtime, time, date);
        put16(d, 0x16, time);
        put16(d, 0x18, date);
        put16(d, 0x1C, static_cast<uint16_t>(e.size));
        put16(d, 0x1E, static_cast<uint16_t>(e.size >> 16));
        return d;
    }

    template <size_t N>
    static void put16(std::array<uint8_t, N>& a, size_t off, uint16_t v) {
        a[off] = static_cast<uint8_t>(v);
        a[off + 1] = static_cast<uint8_t>(v >> 8);
    }

    static uint32_t fcbSize(const std::array<uint8_t, kFcbImageSize>& fcb) {
        return static_cast<uint32_t>(fcb[kFcbSize]) | (static_cast<uint32_t>(fcb[kFcbSize + 1]) << 8) |
               (static_cast<uint32_t>(fcb[kFcbSize + 2]) << 16) | (static_cast<uint32_t>(fcb[kFcbSize + 3]) << 24);
    }

    static void setFcbSize(std::array<uint8_t, kFcbImageSize>& fcb, uint32_t size) {
        for (int i = 0; i < 4; ++i) fcb[kFcbSize + i] = static_cast<uint8_t>(size >> (8 * i));
    }

    // Record number: FCB+30H low 7 bits, +31H/+32H the rest (FDRECPOS 5267H).
    static uint32_t fcbRecord(const std::array<uint8_t, kFcbImageSize>& fcb) {
        return (fcb[kFcbRecord] & 0x7Fu) | (static_cast<uint32_t>(fcb[kFcbRecord + 1]) << 7) |
               (static_cast<uint32_t>(fcb[kFcbRecord + 2]) << 15);
    }

    static void setFcbRecord(std::array<uint8_t, kFcbImageSize>& fcb, uint32_t record) {
        fcb[kFcbRecord] = static_cast<uint8_t>(record & 0x7F);
        fcb[kFcbRecord + 1] = static_cast<uint8_t>(record >> 7);
        fcb[kFcbRecord + 2] = static_cast<uint8_t>(record >> 15);
    }

    // FDOPEN 4B14H: directory entry +0BH..+1FH -> FCB+14H..+28H, record
    // length 256 at +2EH, record 0, clean buffer state.
    static void fillOpenFcb(std::array<uint8_t, kFcbImageSize>& fcb, const Entry& e) {
        const auto d = dirEntry(e);
        fcb[kFcbCount] = 0;
        std::memcpy(&fcb[kFcbAttr], &d[0x0B], 0x15);
        fcb[0x29] = fcb[0x2A] = fcb[0x2B] = fcb[0x2C] = 0;
        fcb[kFcbDirIndex] = 0;
        fcb[kFcbRecLen] = 0x00;
        fcb[kFcbRecLen + 1] = 0x01;
        setFcbRecord(fcb, 0);
        fcb[kFcbModified] = fcb[kFcbModified + 1] = 0;
    }

    std::filesystem::path pathFor(const std::string& name) const {
        Entry e;
        if (findOne(name, e)) return e.path;
        return here() / toHostName(name);
    }

    Response& doOpen(const Request& req, Response& r) {
        const std::string name = fcbName(req.fcb, kFcbName);
        Entry e;
        if (!findOne(name, e)) return fail(r, kErlNotFound);
        fillOpenFcb(r.fcb, e);
        return r;
    }

    Response& doCreate(const Request& req, Response& r) {
        const std::string name = fcbName(req.fcb, kFcbName);
        if (hasWildcard(name)) return fail(r, kErlWildcard);
        Entry e;
        if (findOne(name, e)) {
            if (e.attr & kAttrProtected) return fail(r, kErlProtected);
        } else {
            Entry dir;
            if (findIn(listing(here(), true), name, dir)) return fail(r, kErlExists);
            e.fcbName = name;
            e.path = here() / toHostName(name);
        }
        std::ofstream f(e.path, std::ios::binary | std::ios::trunc);
        if (!f) return fail(r, kErlIo);
        f.close();
        e.attr = hostAttributes(e.path);
        e.size = 0;
        e.mtime = hostMtime(e.path);
        fillOpenFcb(r.fcb, e);
        return r;
    }

    Response& doRead(const Request& req, Response& r) {
        const std::filesystem::path path = pathFor(fcbName(req.fcb, kFcbName));
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) return fail(r, kErlNotFound);
        const uint32_t record = fcbRecord(req.fcb);
        const uint64_t pos = static_cast<uint64_t>(record) * kRecordSize;
        if (pos >= size) return failEof(r);
        r.payload.assign(kRecordSize, 0);
        std::ifstream f(path, std::ios::binary);
        if (!f) return fail(r, kErlIo);
        f.seekg(static_cast<std::streamoff>(pos));
        f.read(reinterpret_cast<char*>(r.payload.data()), kRecordSize);
        setFcbRecord(r.fcb, record + 1);
        return r;
    }

    // One whole 256-byte record at record * 256, like the floppy's FDWRITE:
    // the file ends after it (output is sequential; an append, APPENDPOS
    // 7B1CH, rewrites the last record). FCB+36H bits 0/1 mark the file
    // written; CLOSE trims the last record to FCB+06H bytes.
    Response& doWrite(const Request& req, Response& r) {
        const std::string name = fcbName(req.fcb, kFcbName);
        Entry e;
        if (!findOne(name, e)) return fail(r, kErlNotFound);
        if (e.attr & kAttrProtected) return fail(r, kErlProtected);
        const uint32_t record = fcbRecord(req.fcb);
        const uint64_t pos = static_cast<uint64_t>(record) * kRecordSize;
        const uint64_t end = pos + kRecordSize;
        if (end > 0xFFFFFFFFu) return fail(r, kErlDiskFull);
        {
            std::fstream f(e.path, std::ios::binary | std::ios::in | std::ios::out);
            if (!f) return fail(r, kErlIo);
            f.seekp(static_cast<std::streamoff>(pos));
            f.write(reinterpret_cast<const char*>(req.data.data()), kRecordSize);
            f.flush();
            if (!f) return fail(r, kErlDiskFull);
        }
        std::error_code ec;
        if (std::filesystem::file_size(e.path, ec) != end) std::filesystem::resize_file(e.path, end, ec);
        if (ec) return fail(r, kErlIo);
        setFcbSize(r.fcb, static_cast<uint32_t>(end));
        setFcbRecord(r.fcb, record + 1);
        r.fcb[kFcbModified] |= 0x03;
        return r;
    }

    // FDCLOSE 4B66H: a written file whose last record holds FCB+06H/+37H =
    // n bytes (1..255) loses the 256 - n it was padded with. BASIC's
    // CLOSEFCB (7A83H) leaves n there after the final flush; COPY (7584H)
    // puts the source's size mod 256 there.
    Response& doClose(const Request& req, Response& r) {
        const bool written = (req.fcb[kFcbModified] & 0x03) == 0x03;
        r.fcb[kFcbModified] = 0;
        const uint32_t count = req.fcb[kFcbCount] | (req.fcb[kFcbModified + 1] << 8);
        if (!written || count == 0 || count >= kRecordSize) return r;
        Entry e;
        if (!findOne(fcbName(req.fcb, kFcbName), e)) return fail(r, kErlIo);  // A3H, as FDCLOSE
        if (e.size < kRecordSize - count) return r;
        const uint32_t size = e.size - (kRecordSize - count);
        std::error_code ec;
        std::filesystem::resize_file(e.path, size, ec);
        if (ec) return fail(r, kErlIo);
        setFcbSize(r.fcb, size);
        return r;
    }

    Response& doSearchFirst(const Request& req, Response& r) {
        Search s;
        s.entries = find(fcbName(req.fcb, kFcbName), m_listDirectories);
        m_searches[req.fcbAddress] = std::move(s);
        return nextEntry(req, r);
    }

    Response& doSearchNext(const Request& req, Response& r) {
        if (req.fcb[kFcbDirIndex] == 0xFF || m_searches.find(req.fcbAddress) == m_searches.end())
            return fail(r, kErlEof);  // FDSNEXT 4DCBH: no search in progress
        return nextEntry(req, r);
    }

    // The ROM marks the end of a search with FCB+2DH = FFH (FDSFIRST 4D9EH).
    Response& nextEntry(const Request& req, Response& r) {
        Search& s = m_searches[req.fcbAddress];
        if (s.next >= s.entries.size()) {
            m_searches.erase(req.fcbAddress);
            r.fcb[kFcbDirIndex] = 0xFF;
            return fail(r, kErlNotFound);
        }
        r.fcb[kFcbDirIndex] = static_cast<uint8_t>(s.next % 0xFF);
        const auto d = dirEntry(s.entries[s.next++]);
        r.payload.assign(d.begin(), d.end());
        return r;
    }

    // All matching files; protected ones are kept and reported (FDDELETE 4DD1H).
    Response& doDelete(const Request& req, Response& r) {
        const auto found = find(fcbName(req.fcb, kFcbName));
        if (found.empty()) return fail(r, kErlNotFound);
        uint8_t erl = kErlOk;
        for (const auto& e : found) {
            if (e.attr & kAttrProtected) { erl = kErlProtected; continue; }
            std::error_code ec;
            std::filesystem::remove(e.path, ec);
            if (ec) erl = kErlIo;
        }
        return erl == kErlOk ? r : fail(r, erl);
    }

    // All matching files; '?' in the new name keeps the old character; a
    // new name that already exists stops with 97H (FDRENAME 4E2CH).
    Response& doRename(const Request& req, Response& r) {
        const std::string pattern = fcbName(req.fcb, kFcbName);
        const std::string target = fcbName(req.fcb, kFcbNewName);
        const auto found = find(pattern);
        if (found.empty()) return fail(r, kErlNotFound);
        uint8_t erl = kErlOk;
        for (const auto& e : found) {
            if (e.attr & kAttrProtected) { erl = kErlProtected; continue; }
            std::string name = target;
            for (size_t i = 0; i < 11; ++i)
                if (name[i] == '?') name[i] = e.fcbName[i];
            Entry existing;
            if (name != e.fcbName && findOne(name, existing)) return fail(r, kErlExists);
            std::error_code ec;
            std::filesystem::rename(e.path, here() / toHostName(name), ec);
            if (ec) return fail(r, kErlIo);
        }
        return erl == kErlOk ? r : fail(r, erl);
    }

    Response& doGetAlloc(Response& r) {
        std::error_code ec;
        const auto space = std::filesystem::space(m_dir, ec);
        const uint64_t cluster = static_cast<uint64_t>(kBytesPerSector) * kSectorsPerCluster;
        const uint64_t clusters = ec ? 0 : space.available / cluster;
        r.bc = kBytesPerSector;
        r.de = kSectorsPerCluster;
        r.hl = static_cast<uint16_t>(std::min<uint64_t>(clusters, 0xFFFF));
        return r;
    }

    // P (protect) <-> owner write permission; I (invisible) <-> the macOS
    // hidden flag. Only bits 27H of FCB+14H count (FDSETATTR 4F41H).
    Response& doSetAttr(const Request& req, Response& r) {
        const std::string name = fcbName(req.fcb, kFcbName);
        if (hasWildcard(name)) return fail(r, kErlWildcard);
        Entry e;
        if (!findOne(name, e)) return fail(r, kErlNotFound);
        const uint8_t attr = req.fcb[kFcbAttr];
        std::error_code ec;
        std::filesystem::permissions(e.path, std::filesystem::perms::owner_write,
                                     (attr & kAttrProtected) ? std::filesystem::perm_options::remove
                                                             : std::filesystem::perm_options::add,
                                     ec);
        if (ec) return fail(r, kErlIo);
#if defined(__APPLE__)
        struct stat st{};
        if (::stat(e.path.c_str(), &st) == 0) {
            const auto flags = (attr & kAttrInvisible) ? (st.st_flags | UF_HIDDEN) : (st.st_flags & ~UF_HIDDEN);
            if (flags != st.st_flags && ::chflags(e.path.c_str(), flags) != 0) return fail(r, kErlIo);
        }
#endif
        return r;
    }

    // File size in records of FCB+2EH bytes (0 = 256), rounded up, in DE:HL
    // (FDGETLEN 4EFEH).
    Response& doGetLength(const Request& req, Response& r) {
        Entry e;
        if (!findOne(fcbName(req.fcb, kFcbName), e)) return fail(r, kErlNotFound);
        uint32_t recLen = req.fcb[kFcbRecLen] | (req.fcb[kFcbRecLen + 1] << 8);
        if (recLen == 0) recLen = kRecordSize;
        const uint32_t records = (e.size + recLen - 1) / recLen;
        r.de = static_cast<uint16_t>(records >> 16);
        r.hl = static_cast<uint16_t>(records);
        return r;
    }

    std::filesystem::path m_dir;
    std::vector<std::string> m_cwd;  // host names below m_dir (CDIR)
    std::map<uint16_t, Search> m_searches;
    bool m_listDirectories = false;
};
