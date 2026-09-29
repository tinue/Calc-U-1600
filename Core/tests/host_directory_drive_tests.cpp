// Headless C++ tests for HostDirectoryDrive -- the file-level half of the
// PC-1600 host-directory drive (S3: / Y:). Each test works on a fresh
// temporary directory. Same no-framework, assert-and-tally style as
// lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../Connector/HostDirectoryDrive.hpp"
#include "../Connector/PC1600HostDriveCard.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace fs = std::filesystem;
using Drive = HostDirectoryDrive;

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() / ("calcu-hostdrive-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        for (auto& e : fs::directory_iterator(path, ec))
            fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary);
    f << bytes;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

Drive::Request request(uint8_t fn, const std::string& name83, uint16_t fcbAddress = 0xF3C7) {
    Drive::Request req;
    req.function = fn;
    req.fcbAddress = fcbAddress;
    req.device = 0x42;
    std::memcpy(&req.fcb[Drive::kFcbName], name83.data(), 11);
    return req;
}

uint32_t size32(const std::array<uint8_t, Drive::kFcbImageSize>& fcb) {
    return fcb[0x25] | (fcb[0x26] << 8) | (fcb[0x27] << 16) | (static_cast<uint32_t>(fcb[0x28]) << 24);
}

std::vector<std::string> listNames(Drive& drive, const std::string& pattern) {
    std::vector<std::string> names;
    auto req = request(Drive::kSearchFirst, pattern);
    auto r = drive.execute(req);
    while (r.status == 0) {
        names.emplace_back(reinterpret_cast<const char*>(r.payload.data()), 11);
        req.function = Drive::kSearchNext;
        req.fcb = r.fcb;
        r = drive.execute(req);
    }
    CHECK(r.status == 0x01);  // end of listing: "not found", what FILES expects
    return names;
}

void test_name_mapping() {
    CHECK(Drive::toFcbName("prog.bas") == "PROG    BAS");
    CHECK(Drive::toFcbName("MAKEFILE") == "MAKEFILE   ");
    CHECK(Drive::toFcbName("A-B_{1}.$$$") == "A-B_{1} $$$");
    CHECK(Drive::toFcbName("toolongname.bas").empty());
    CHECK(Drive::toFcbName("x.basic").empty());
    CHECK(Drive::toFcbName(".hidden").empty());
    CHECK(Drive::toFcbName("a.b.c").empty());
    CHECK(Drive::toFcbName("sp ace.txt").empty());
    CHECK(Drive::toFcbName("wild?.txt").empty());
    CHECK(Drive::toFcbName("caf\xC3\xA9.txt").empty());
    CHECK(Drive::toHostName("PROG    BAS") == "PROG.BAS");
    CHECK(Drive::toHostName("MAKEFILE   ") == "MAKEFILE");
}

void test_timestamp_has_no_real_year() {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 8;   // September
    tm.tm_mday = 29;
    tm.tm_hour = 14;
    tm.tm_min = 35;
    tm.tm_sec = 42;
    tm.tm_isdst = -1;
    uint16_t time = 0, date = 0;
    Drive::packTimestamp(std::mktime(&tm), time, date);
    CHECK(time == ((14 << 11) | (35 << 5) | 21));
    CHECK(date == ((6 << 9) | (9 << 5) | 29));
}

void test_no_directory_is_no_media() {
    Drive drive;
    auto r = drive.execute(request(Drive::kOpen, "PROG    BAS"));
    CHECK(r.status == 0x20);
    CHECK(r.erl == Drive::kErlNoMedia);
    drive.setDirectory("/nonexistent/calcu-hostdrive");
    r = drive.execute(request(Drive::kSearchFirst, "???????????"));
    CHECK(r.status == 0x20);
}

void test_listing_filters_and_sorts() {
    TempDir dir;
    writeFile(dir.path / "zeta.txt", "z");
    writeFile(dir.path / "ALPHA.BAS", "a");
    writeFile(dir.path / "a-very-long-name.bas", "x");
    writeFile(dir.path / ".DS_Store", "x");
    fs::create_directories(dir.path / "SUBDIR");
    Drive drive;
    drive.setDirectory(dir.path);
    const auto names = listNames(drive, "???????????");
    CHECK(names.size() == 2);
    CHECK(names.size() == 2 && names[0] == "ALPHA   BAS");
    CHECK(names.size() == 2 && names[1] == "ZETA    TXT");
    const auto bas = listNames(drive, "????????BAS");
    CHECK(bas.size() == 1);
}

void test_directory_entry_fields() {
    TempDir dir;
    writeFile(dir.path / "DATA.TXT", std::string(70000, 'x'));
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kSearchFirst, "DATA    TXT"));
    CHECK(r.status == 0);
    CHECK(r.payload.size() == Drive::kDirEntrySize);
    if (r.payload.size() == Drive::kDirEntrySize) {
        CHECK(std::memcmp(r.payload.data(), "DATA    TXT", 11) == 0);
        CHECK(r.payload[0x0B] == Drive::kAttrArchive);
        CHECK((r.payload[0x19] >> 1) == 6);  // year field: 1986
        const uint32_t size = r.payload[0x1C] | (r.payload[0x1D] << 8) | (r.payload[0x1E] << 16);
        CHECK(size == 70000);
    }
    CHECK(r.fcb[Drive::kFcbDirIndex] != 0xFF);
    auto next = request(Drive::kSearchNext, "DATA    TXT");
    next.fcb = r.fcb;
    r = drive.execute(next);
    CHECK(r.status == 0x01);
    CHECK(r.fcb[Drive::kFcbDirIndex] == 0xFF);
    next.fcb = r.fcb;
    r = drive.execute(next);
    CHECK(r.erl == Drive::kErlEof);  // SNEXT after the end, like FDSNEXT
}

void test_case_collision_keeps_one_file() {
    TempDir dir;
    writeFile(dir.path / "Prog.bas", "lower");
    writeFile(dir.path / "PROG.BAS", "upper");
    std::error_code ec;
    const bool caseSensitive = fs::file_size(dir.path / "Prog.bas", ec) == 5 && readFile(dir.path / "PROG.BAS") == "upper" &&
                               readFile(dir.path / "Prog.bas") == "lower";
    Drive drive;
    drive.setDirectory(dir.path);
    const auto names = listNames(drive, "???????????");
    CHECK(names.size() == 1);
    auto r = drive.execute(request(Drive::kOpen, "PROG    BAS"));
    CHECK(r.status == 0);
    CHECK(size32(r.fcb) == 5);
    if (caseSensitive) {
        auto rd = request(Drive::kSeqRead, "PROG    BAS");
        rd.fcb = r.fcb;
        r = drive.execute(rd);
        CHECK(r.status == 0 && std::memcmp(r.payload.data(), "upper", 5) == 0);
    }
}

void test_open_fills_the_fcb_like_the_floppy() {
    TempDir dir;
    writeFile(dir.path / "prog.bas", std::string(300, 'p'));
    Drive drive;
    drive.setDirectory(dir.path);
    auto req = request(Drive::kOpen, "PROG    BAS");
    req.fcb[Drive::kFcbCount] = 0x55;
    req.fcb[Drive::kFcbRecord] = 0x12;
    auto r = drive.execute(req);
    CHECK(r.status == 0);
    CHECK(r.fcb[Drive::kFcbCount] == 0);
    CHECK(r.fcb[Drive::kFcbAttr] == Drive::kAttrArchive);
    CHECK(size32(r.fcb) == 300);
    CHECK(r.fcb[Drive::kFcbRecLen] == 0x00 && r.fcb[Drive::kFcbRecLen + 1] == 0x01);
    CHECK(r.fcb[Drive::kFcbRecord] == 0 && r.fcb[Drive::kFcbRecord + 1] == 0);
    CHECK(std::memcmp(&r.fcb[Drive::kFcbName], "PROG    BAS", 11) == 0);  // left untouched
    r = drive.execute(request(Drive::kOpen, "MISSING BAS"));
    CHECK(r.status == 0x01);
    CHECK(r.erl == Drive::kErlNotFound);
}

// BASIC's own sequence: full records with FCB+06H = 0, the last partial
// one flushed as a whole record, then CLOSE with FCB+06H = its byte count.
std::array<uint8_t, Drive::kFcbImageSize> writeRecord(Drive& drive, const std::string& name,
                                                     std::array<uint8_t, Drive::kFcbImageSize> fcb,
                                                     uint8_t fill) {
    auto wr = request(Drive::kSeqWrite, name);
    wr.fcb = fcb;
    wr.fcb[Drive::kFcbCount] = 0;
    for (size_t i = 0; i < Drive::kRecordSize; ++i) wr.data[i] = static_cast<uint8_t>(fill + i);
    const auto r = drive.execute(wr);
    CHECK(r.status == 0);
    return r.fcb;
}

Drive::Response closeWithCount(Drive& drive, const std::string& name,
                               std::array<uint8_t, Drive::kFcbImageSize> fcb, uint8_t count) {
    auto cl = request(Drive::kClose, name);
    cl.fcb = fcb;
    cl.fcb[Drive::kFcbCount] = count;
    return drive.execute(cl);
}

void test_write_then_read_records_beyond_32k() {
    TempDir dir;
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kCreate, "BIG     DAT"));
    CHECK(r.status == 0);
    CHECK(fs::exists(dir.path / "BIG.DAT"));
    // 200 full records (51200 bytes, past the 7-bit record field) + 10 bytes.
    std::array<uint8_t, Drive::kFcbImageSize> fcb = r.fcb;
    for (int rec = 0; rec <= 200; ++rec) fcb = writeRecord(drive, "BIG     DAT", fcb, static_cast<uint8_t>(rec));
    CHECK(fs::file_size(dir.path / "BIG.DAT") == 201 * 256);
    CHECK((fcb[Drive::kFcbModified] & 0x03) == 0x03);
    r = closeWithCount(drive, "BIG     DAT", fcb, 10);
    CHECK(r.status == 0);
    CHECK(fs::file_size(dir.path / "BIG.DAT") == 200 * 256 + 10);
    CHECK(size32(r.fcb) == 200 * 256 + 10);
    CHECK(r.fcb[Drive::kFcbModified] == 0);

    r = drive.execute(request(Drive::kOpen, "BIG     DAT"));
    fcb = r.fcb;
    bool allMatch = true;
    for (int rec = 0; rec <= 200; ++rec) {
        auto rd = request(Drive::kSeqRead, "BIG     DAT");
        rd.fcb = fcb;
        r = drive.execute(rd);
        if (r.status != 0 || r.payload.size() != Drive::kRecordSize) { allMatch = false; break; }
        const size_t n = rec == 200 ? 10 : Drive::kRecordSize;
        for (size_t i = 0; i < n; ++i)
            if (r.payload[i] != static_cast<uint8_t>(rec + i)) allMatch = false;
        fcb = r.fcb;
    }
    CHECK(allMatch);
    // Record 201 = the 7-bit field wrapped once: 201 = 0x49 + 1 * 128.
    CHECK(fcb[Drive::kFcbRecord] == 0x49 && fcb[Drive::kFcbRecord + 1] == 1);
    auto rd = request(Drive::kSeqRead, "BIG     DAT");
    rd.fcb = fcb;
    r = drive.execute(rd);
    CHECK(r.status == 0x04);  // bit 2: end of file, what FREADREC and COPY test
    CHECK(r.erl == Drive::kErlEof);
}

void test_close_without_writes_keeps_the_file() {
    TempDir dir;
    writeFile(dir.path / "R.TXT", std::string(300, 'r'));
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kOpen, "R       TXT"));
    r = closeWithCount(drive, "R       TXT", r.fcb, 5);  // read-only FCB: no trim
    CHECK(r.status == 0);
    CHECK(fs::file_size(dir.path / "R.TXT") == 300);
}

void test_append_rewrite_truncates_after_the_last_record() {
    TempDir dir;
    writeFile(dir.path / "LOG.TXT", std::string("abc\x1A", 4));
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kOpen, "LOG     TXT"));
    // BASIC's APPENDPOS rewrites record 0 with the old bytes minus the 1AH.
    auto wr = request(Drive::kSeqWrite, "LOG     TXT");
    wr.fcb = r.fcb;
    std::memcpy(wr.data.data(), "abd", 3);
    r = drive.execute(wr);
    CHECK(r.status == 0);
    r = closeWithCount(drive, "LOG     TXT", r.fcb, 3);
    CHECK(readFile(dir.path / "LOG.TXT") == "abd");
}

void test_create_existing_file_keeps_its_host_spelling() {
    TempDir dir;
    writeFile(dir.path / "notes.txt", "old contents");
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kCreate, "NOTES   TXT"));
    CHECK(r.status == 0);
    CHECK(size32(r.fcb) == 0);
    CHECK(fs::file_size(dir.path / "notes.txt") == 0);
    int count = 0;
    for (auto& e : fs::directory_iterator(dir.path)) { (void)e; ++count; }
    CHECK(count == 1);
    r = drive.execute(request(Drive::kCreate, "BAD?    TXT"));
    CHECK(r.erl == Drive::kErlWildcard);
}

void test_protect_attribute_round_trips_through_permissions() {
    TempDir dir;
    writeFile(dir.path / "KEEP.BAS", "k");
    writeFile(dir.path / "DROP.BAS", "d");
    Drive drive;
    drive.setDirectory(dir.path);
    auto set = request(Drive::kSetAttr, "KEEP    BAS", 0xF400);
    set.fcb[Drive::kFcbAttr] = Drive::kAttrArchive | Drive::kAttrProtected;
    CHECK(drive.execute(set).status == 0);
    auto r = drive.execute(request(Drive::kOpen, "KEEP    BAS"));
    CHECK(r.fcb[Drive::kFcbAttr] & Drive::kAttrProtected);

    // Protected: CREATE and WRITE refused; DELETE of *.BAS keeps it, removes the rest.
    CHECK(drive.execute(request(Drive::kCreate, "KEEP    BAS")).erl == Drive::kErlProtected);
    r = drive.execute(request(Drive::kDelete, "????????BAS"));
    CHECK(r.erl == Drive::kErlProtected);
    CHECK(r.status == 0x48);
    CHECK(fs::exists(dir.path / "KEEP.BAS"));
    CHECK(!fs::exists(dir.path / "DROP.BAS"));

    set.fcb[Drive::kFcbAttr] = Drive::kAttrArchive;
    CHECK(drive.execute(set).status == 0);
    CHECK(drive.execute(request(Drive::kDelete, "KEEP    BAS")).status == 0);
    CHECK(!fs::exists(dir.path / "KEEP.BAS"));
    CHECK(drive.execute(request(Drive::kDelete, "KEEP    BAS")).erl == Drive::kErlNotFound);
    auto wild = request(Drive::kSetAttr, "????????BAS");
    CHECK(drive.execute(wild).erl == Drive::kErlWildcard);
}

void test_rename() {
    TempDir dir;
    writeFile(dir.path / "old.bas", "1");
    writeFile(dir.path / "TAKEN.BAS", "2");
    Drive drive;
    drive.setDirectory(dir.path);
    auto req = request(Drive::kRename, "OLD     BAS");
    std::memcpy(&req.fcb[Drive::kFcbNewName], "TAKEN   BAS", 11);
    CHECK(drive.execute(req).erl == Drive::kErlExists);
    std::memcpy(&req.fcb[Drive::kFcbNewName], "NEW     ???", 11);  // '?' keeps the old character
    CHECK(drive.execute(req).status == 0);
    CHECK(fs::exists(dir.path / "NEW.BAS"));
    CHECK(!fs::exists(dir.path / "old.bas"));
    CHECK(readFile(dir.path / "NEW.BAS") == "1");
    CHECK(drive.execute(req).erl == Drive::kErlNotFound);
}

void test_get_alloc_fits_dskf() {
    TempDir dir;
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kGetAlloc, "           "));
    CHECK(r.status == 0);
    CHECK(r.bc == 512);
    CHECK((r.de & 0xFF) == 64);
    CHECK(static_cast<uint32_t>(r.bc) * (r.de & 0xFF) <= 0xFFFF);  // DSKF's 16-bit shift
    CHECK(r.hl > 0);
}

void test_get_length_in_records() {
    TempDir dir;
    writeFile(dir.path / "L.DAT", std::string(513, 'l'));
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kGetLength, "L       DAT"));
    CHECK(r.status == 0);
    CHECK(r.de == 0 && r.hl == 3);
}

void test_reset_ends_searches() {
    TempDir dir;
    writeFile(dir.path / "A.TXT", "a");
    writeFile(dir.path / "B.TXT", "b");
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(Drive::kSearchFirst, "???????????"));
    CHECK(r.status == 0);
    drive.reset();
    auto next = request(Drive::kSearchNext, "???????????");
    next.fcb = r.fcb;
    CHECK(drive.execute(next).erl == Drive::kErlEof);
}

void test_mep_change_directory_knows_only_the_root() {
    TempDir dir;
    fs::create_directories(dir.path / "SUBDIR");
    Drive drive;
    CHECK(drive.changeDirectory("/").erl == Drive::kErlNoMedia);
    drive.setDirectory(dir.path);
    auto r = drive.changeDirectory("/");
    CHECK(r.status == 0 && r.erl == Drive::kErlOk);
    CHECK(drive.changeDirectory("").status == 0);
    r = drive.changeDirectory("/SUBDIR");  // host subdirectories stay hidden
    CHECK(r.status == 0x01);
    CHECK(r.erl == Drive::kErlNotFound);
}

void test_mep_directory_mode_lists_nothing() {
    TempDir dir;
    writeFile(dir.path / "A.TXT", "a");
    fs::create_directories(dir.path / "SUBDIR");
    Drive drive;
    drive.setDirectory(dir.path);
    drive.setListDirectories(true);
    CHECK(listNames(drive, "???????????").empty());
    drive.setListDirectories(false);
    CHECK(listNames(drive, "???????????").size() == 1);
    drive.setListDirectories(true);
    drive.reset();  // power / reset: back to listing files
    CHECK(listNames(drive, "???????????").size() == 1);
}

// The MEP fixed entries' bus protocol, as the driver ROM speaks it.
void test_card_mep_commands() {
    TempDir dir;
    writeFile(dir.path / "A.TXT", "a");
    PC1600HostDriveCard card;
    card.drive().setDirectory(dir.path);
    auto out = [&card](uint8_t port, uint8_t value) {
        PC1600BusPins pins;
        pins.io = true;
        pins.forWrite = true;
        pins.address = port;
        CHECK(card.respondsToWrite(pins, value));
    };
    auto in = [&card]() {
        PC1600BusPins pins;
        pins.io = true;
        pins.address = PC1600HostDriveCard::kDataPort;
        uint8_t v = 0;
        CHECK(card.respondsToRead(pins, v));
        return v;
    };
    auto cdir = [&](const std::string& path, uint8_t& status, uint8_t& erl) {
        out(PC1600HostDriveCard::kCommandPort, PC1600HostDriveCard::kChangeDirCommand);
        out(PC1600HostDriveCard::kDataPort, static_cast<uint8_t>(path.size()));
        for (char c : path) out(PC1600HostDriveCard::kDataPort, static_cast<uint8_t>(c));
        status = in();
        erl = in();
    };
    uint8_t status = 0xFF, erl = 0xFF;
    cdir("/", status, erl);
    CHECK(status == 0 && erl == 0);
    cdir("/DOCS", status, erl);
    CHECK(status == 0x01 && erl == Drive::kErlNotFound);
    cdir("", status, erl);
    CHECK(status == 0 && erl == 0);

    out(PC1600HostDriveCard::kCommandPort, PC1600HostDriveCard::kDirModeCommand);
    CHECK(listNames(card.drive(), "???????????").empty());
    out(PC1600HostDriveCard::kCommandPort, PC1600HostDriveCard::kFileModeCommand);
    CHECK(listNames(card.drive(), "???????????").size() == 1);
}

void test_unsupported_function() {
    TempDir dir;
    Drive drive;
    drive.setDirectory(dir.path);
    auto r = drive.execute(request(0x21, "           "));
    CHECK(r.status == 0x10);
    CHECK(r.erl == Drive::kErlUnsupported);
}

}  // namespace

int run_host_directory_drive_tests() {
    test_name_mapping();
    test_timestamp_has_no_real_year();
    test_no_directory_is_no_media();
    test_listing_filters_and_sorts();
    test_directory_entry_fields();
    test_case_collision_keeps_one_file();
    test_open_fills_the_fcb_like_the_floppy();
    test_write_then_read_records_beyond_32k();
    test_close_without_writes_keeps_the_file();
    test_append_rewrite_truncates_after_the_last_record();
    test_create_existing_file_keeps_its_host_spelling();
    test_protect_attribute_round_trips_through_permissions();
    test_rename();
    test_get_alloc_fits_dskf();
    test_get_length_in_records();
    test_reset_ends_searches();
    test_mep_change_directory_knows_only_the_root();
    test_mep_directory_mode_lists_nothing();
    test_card_mep_commands();
    test_unsupported_function();

    std::printf("host_directory_drive_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
