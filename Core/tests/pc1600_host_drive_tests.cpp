// Headless C++ tests for the PC-1600 host-directory drive (S3: / Y:) end
// to end: the real PC-1600 ROM, BASIC typed at the prompt, our driver ROM in
// page-1 bank 7 (firmware/pc1600-hostdrive/) and PC1600HostDriveCard on the
// 60-pin bus, checked through the files that land in a temporary host
// directory. Same no-framework, assert-and-tally style as lh5801_tests.cpp.
//
// Build & run: see tools/run_tests.sh (from the repo root: roms/ and
// firmware/ are looked up relative to it).

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace fs = std::filesystem;

const std::vector<std::string> kHostDriveRomDirs = {"firmware/pc1600-hostdrive"};

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("calcu-pc1600-hostdrive-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
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

// Boots a PC-1600 with the host drive serving `dir`. False (test skipped)
// without the PC-1600 ROM images.
bool bootWithHostDrive(PC1600Machine& m, const fs::path& dir, bool withCE1600P = false) {
    std::string error;
    if (!loadPC1600Roms(m)) return false;
    if (!BundledRoms::attachHostDrive(m, kHostDriveRomDirs, dir, &error)) {
        std::fprintf(stderr, "FAIL host drive attach: %s\n", error.c_str());
        g_fail++;
        return false;
    }
    if (withCE1600P && !BundledRoms::attachCE1600P(m, {"roms"}, "new", &error)) return false;
    m.allReset();
    runBootToPrompt(m);
    tapKey(m, "mode"); waitIdle(m, PC1600Machine::kTStateHz);  // as the COM1 test: settle
    tapKey(m, "mode"); waitIdle(m, PC1600Machine::kTStateHz);  // the prompt, back in RUN
    return true;
}

void type(PC1600Machine& m, const std::string& line) {
    std::string err;
    CHECK(typeLine(m, line, /*pressEnter=*/true, &err));
    waitUntilBasicIdle(m, 600ull * PC1600Machine::kTStateHz);
    waitIdle(m, PC1600Machine::kTStateHz);
}

// NEW, program text typed in PRO mode, back to RUN mode.
void enterProgram(PC1600Machine& m, const std::string& text) {
    tapKey(m, "mode");  // RUN -> PRO
    waitIdle(m, PC1600Machine::kTStateHz);
    type(m, "NEW");
    const BasicTypeResult r = typeBasicProgramText(m, text);
    CHECK(r.rejectedLines.empty());
    tapKey(m, "mode");  // PRO -> RUN
    waitIdle(m, PC1600Machine::kTStateHz);
}

// enterProgram() + RUN.
void runProgram(PC1600Machine& m, const std::string& text) {
    enterProgram(m, text);
    type(m, "RUN");
}

// Runs a program that writes the error code (0 = none) of `statement` to
// S3:E.TXT, and returns that code.
int errorOf(PC1600Machine& m, const fs::path& dir, const std::string& statement) {
    enterProgram(m,
                 "5 MAXFILES=1\n"
                 "10 ON ERROR GOTO 100\n"
                 "20 " + statement + "\n"
                 "30 E=0:GOTO 110\n"
                 "100 E=ERN\n"
                 "110 OPEN \"S3:E.TXT\" FOR OUTPUT AS #1:PRINT #1,E:CLOSE #1\n");
    fs::remove(dir / "E.TXT");
    type(m, "RUN");
    const std::string text = readFile(dir / "E.TXT");
    return text.empty() ? -1 : std::atoi(text.c_str());
}

void test_sequential_files_round_trip() {
    TempDir dir;
    writeFile(dir.path / "in.txt", "HELLO\r\n\x1A");
    PC1600Machine m;
    if (!bootWithHostDrive(m, dir.path)) {
        std::fprintf(stderr, "SKIP test_sequential_files_round_trip: PC-1600 ROM images not found\n");
        return;
    }
    runProgram(m,
               "10 MAXFILES=2\n"
               "20 OPEN \"S3:IN.TXT\" FOR INPUT AS #1:INPUT #1,A$:CLOSE #1\n"
               "30 OPEN \"S3:OUT.TXT\" FOR OUTPUT AS #2:PRINT #2,A$;\"!\":CLOSE #2\n");
    CHECK(fs::exists(dir.path / "OUT.TXT"));
    CHECK(readFile(dir.path / "OUT.TXT") == "HELLO!\r\n\x1A");

    runProgram(m,
               "10 MAXFILES=1\n"
               "20 OPEN \"S3:OUT.TXT\" FOR APPEND AS #1:PRINT #1,\"MORE\":CLOSE #1\n");
    CHECK(readFile(dir.path / "OUT.TXT") == "HELLO!\r\nMORE\r\n\x1A");

    // Past 32 KB (the 7-bit record field wraps): 3000 lines of 17 bytes.
    runProgram(m,
               "10 MAXFILES=1\n"
               "20 OPEN \"S3:BIG.TXT\" FOR OUTPUT AS #1\n"
               "30 FOR I=1 TO 3000:PRINT #1,\"0123456789ABCDE\":NEXT I:CLOSE #1\n"
               "40 N=0:OPEN \"S3:BIG.TXT\" FOR INPUT AS #1\n"
               "50 FOR I=1 TO 3000:INPUT #1,B$:N=N+LEN(B$):NEXT I:CLOSE #1\n"
               "60 OPEN \"S3:N.TXT\" FOR OUTPUT AS #1:PRINT #1,N:CLOSE #1\n");
    std::error_code ec;
    CHECK(fs::file_size(dir.path / "BIG.TXT", ec) == 3000 * 17 + 1);
    CHECK(readFile(dir.path / "N.TXT").find("45000") != std::string::npos);

    runProgram(m,
               "10 MAXFILES=1\n"
               "20 OPEN \"S3:D.TXT\" FOR OUTPUT AS #1:PRINT #1,DSKF(\"S3:\"):CLOSE #1\n");
    CHECK(std::atof(readFile(dir.path / "D.TXT").c_str()) > 0);
}

void test_save_load_kill_name_copy() {
    TempDir dir;
    writeFile(dir.path / "a-long-host-name.bas", "not visible");
    PC1600Machine m;
    if (!bootWithHostDrive(m, dir.path)) {
        std::fprintf(stderr, "SKIP test_save_load_kill_name_copy: PC-1600 ROM images not found\n");
        return;
    }
    enterProgram(m, "10 A=42\n20 PRINT A\n");
    type(m, "SAVE \"S3:T.BAS\"");
    const std::string saved = readFile(dir.path / "T.BAS");
    CHECK(saved.size() > 16 && static_cast<uint8_t>(saved[0]) == 0xFF && saved[1] == 0x10);

    tapKey(m, "mode");
    type(m, "NEW");
    tapKey(m, "mode");
    type(m, "LOAD \"S3:T.BAS\"");
    type(m, "SAVE \"S3:T2.BAS\"");
    CHECK(readFile(dir.path / "T2.BAS") == saved);

    type(m, "KILL \"S3:T2.BAS\"");
    CHECK(!fs::exists(dir.path / "T2.BAS"));
    type(m, "NAME \"S3:T.BAS\" AS \"U.BAS\"");
    CHECK(fs::exists(dir.path / "U.BAS"));
    CHECK(!fs::exists(dir.path / "T.BAS"));
    const int copyErr = errorOf(m, dir.path, "MAXFILES=2:COPY \"S3:U.BAS\" TO \"S3:V.BAS\"");
    CHECK(copyErr == 0);
    CHECK(readFile(dir.path / "V.BAS") == saved);
    CHECK(readFile(dir.path / "a-long-host-name.bas") == "not visible");
}

void test_protect_and_errors() {
    TempDir dir;
    writeFile(dir.path / "KEEP.TXT", "keep");
    PC1600Machine m;
    if (!bootWithHostDrive(m, dir.path)) {
        std::fprintf(stderr, "SKIP test_protect_and_errors: PC-1600 ROM images not found\n");
        return;
    }
    type(m, "SET \"S3:KEEP.TXT\",\"P\"");
    CHECK((fs::status(dir.path / "KEEP.TXT").permissions() & fs::perms::owner_write) == fs::perms::none);
    CHECK(errorOf(m, dir.path, "KILL \"S3:KEEP.TXT\"") == 0x9F);
    CHECK(fs::exists(dir.path / "KEEP.TXT"));
    type(m, "SET \"S3:KEEP.TXT\",\" \"");
    CHECK((fs::status(dir.path / "KEEP.TXT").permissions() & fs::perms::owner_write) != fs::perms::none);

    CHECK(errorOf(m, dir.path, "OPEN \"S3:NONE.TXT\" FOR INPUT AS #1") == 0x98);
    // INIT on the host drive is refused, and the directory stays as it was.
    CHECK(errorOf(m, dir.path, "INIT \"S3:\"") > 0);
    CHECK(readFile(dir.path / "KEEP.TXT") == "keep");
}

void test_y_alias_follows_the_floppy() {
    {
        TempDir dir;
        PC1600Machine m;
        if (!bootWithHostDrive(m, dir.path)) {
            std::fprintf(stderr, "SKIP test_y_alias_follows_the_floppy: PC-1600 ROM images not found\n");
            return;
        }
        enterProgram(m, "10 END\n");
        type(m, "SAVE \"Y:Y.BAS\"");
        CHECK(fs::exists(dir.path / "Y.BAS"));  // no CE-1600F: Y: is the host drive
    }
    {
        TempDir dir;
        PC1600Machine m;
        if (!bootWithHostDrive(m, dir.path, /*withCE1600P=*/true)) {
            std::fprintf(stderr, "SKIP test_y_alias_follows_the_floppy (CE-1600P): ROM images not found\n");
            return;
        }
        enterProgram(m, "10 END\n");
        type(m, "SAVE \"S3:S.BAS\"");
        CHECK(fs::exists(dir.path / "S.BAS"));
        CHECK(errorOf(m, dir.path, "SAVE \"Y:Y.BAS\"") > 0);  // the floppy's Y: (no disk)
        CHECK(!fs::exists(dir.path / "Y.BAS"));
    }
}

void test_unmounted_drive_creates_nothing() {
    TempDir dir;
    PC1600Machine m;
    if (!bootWithHostDrive(m, dir.path)) {
        std::fprintf(stderr, "SKIP test_unmounted_drive_creates_nothing: PC-1600 ROM images not found\n");
        return;
    }
    CHECK(m.hostDriveDirectory() == dir.path);
    // While unmounted the OPEN fails (no media, unit-tested in
    // host_directory_drive_tests) and creates nothing; the drive works
    // again as soon as the directory is back.
    m.setHostDriveDirectory({});
    runProgram(m,
               "10 MAXFILES=1:ON ERROR GOTO 100\n"
               "20 OPEN \"S3:X.TXT\" FOR OUTPUT AS #1:CLOSE #1\n"
               "100 END\n");
    CHECK(!fs::exists(dir.path / "X.TXT"));
    m.setHostDriveDirectory(dir.path);
    CHECK(errorOf(m, dir.path, "OPEN \"S3:X.TXT\" FOR OUTPUT AS #1:CLOSE #1") == 0);
    CHECK(fs::exists(dir.path / "X.TXT"));
}

}  // namespace

int run_pc1600_host_drive_tests() {
    test_sequential_files_round_trip();
    test_save_load_kill_name_copy();
    test_protect_and_errors();
    test_y_alias_follows_the_floppy();
    test_unmounted_drive_creates_nothing();

    std::printf("pc1600_host_drive_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
