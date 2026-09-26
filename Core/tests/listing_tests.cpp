// Headless tests for the listing parsers (Core/Debug/Listing/) and the
// source map (Core/Debug/SourceMap). Same assert-and-tally style as
// lh5801_tests.cpp. The fixtures under Core/tests/fixtures/listings/ are
// real assembler output:
//
//   sdas-lh5801/memtest.*       examples/machine-code/memtest.asm (ENTRY 0x40C5) through
//                               sdaslh5801 -plosgff + sdld (the .rst), plus
//                               its linked image memtest_stock.bin
//   sdas-lh5801/include_main.*  an .include, and a .db that wraps
//   sdas-z80/blink.*            sdasz80 -plosgff + sdldz80 -b CODE=0xC0C5:
//                               the .lst holds area offsets, the .rst the
//                               linked addresses
//   zasm/pc1600-rom-dumper.*    zasm -uwy of the PC-1600 ROM dumper (+ its .bin)
//   zasm/include_main.*         #include, and data lines that wrap
//   symbols.sym                 a .SYMBOLS: table
//
// Build & run: see tools/run_tests.sh (from the repo root)

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include "../Debug/Listing/Listing.hpp"
#include "../Debug/SourceMap.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

const std::string kDir = "Core/tests/fixtures/listings/";

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

const debug::ListingLine* lineAt(const debug::Listing& l, uint16_t addr) {
    for (const auto& line : l.lines)
        if (line.addr == addr) return &line;
    return nullptr;
}

std::string fileOf(const debug::Listing& l, const debug::ListingLine* line) {
    return line ? l.files[size_t(line->file)] : std::string();
}

size_t totalBytes(const debug::Listing& l) {
    size_t n = 0;
    for (const auto& line : l.lines) n += line.bytes.size();
    return n;
}

void test_sdas_lh5801_memtest() {
    debug::Listing l;
    std::string error;
    CHECK(debug::loadListing(kDir + "sdas-lh5801/memtest.rst", &l, &error));
    CHECK(l.format == "sdas");
    CHECK(!l.files.empty() && endsWith(l.files[0], "sdas-lh5801/memtest.asm"));
    CHECK(l.warnings.empty());
    const debug::ListingLine* first = lineAt(l, 0x40C5);
    CHECK(first && first->line == 74 && first->bytes == std::vector<uint8_t>({0x8E, 0x05}));
    const debug::ListingLine* sta = lineAt(l, 0x40CE);
    CHECK(sta && sta->line == 90 && sta->bytes.size() == 3);
    // Full-length names from the source column (the .sym table cuts them to 8).
    CHECK(l.symbols.count("ERR_ACTUAL") && l.symbols.at("ERR_ACTUAL") == 0x40CB);
    CHECK(l.symbols.count("MEMTEST") && l.symbols.at("MEMTEST") == 0x40CC);
    CHECK(l.symbols.count("ENTRY") && l.symbols.at("ENTRY") == 0x40C5);   // .equ
    CHECK(l.symbols.count("BOTTOM_H") && l.symbols.at("BOTTOM_H") == 0x7867);

    // Every byte agrees with the shipped binary built from the same source.
    std::ifstream bin("Core/tests/fixtures/listings/sdas-lh5801/memtest_stock.bin", std::ios::binary);
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
    CHECK(totalBytes(l) == image.size());
    bool same = !image.empty();
    for (const auto& line : l.lines)
        for (size_t k = 0; k < line.bytes.size(); k++) {
            const size_t off = size_t(line.addr - 0x40C5) + k;
            same = same && off < image.size() && image[off] == line.bytes[k];
        }
    CHECK(same);
}

void test_sdas_include_and_wrapped_data() {
    debug::Listing l;
    std::string error;
    CHECK(debug::loadListing(kDir + "sdas-lh5801/include_main.lst", &l, &error));
    CHECK(l.files.size() == 2 && endsWith(l.files[1], "include_part.asm"));
    const debug::ListingLine* sjp = lineAt(l, 0x40C5);
    CHECK(sjp && sjp->line == 4 && sjp->bytes == std::vector<uint8_t>({0xBE, 0x40, 0xC8})); // r markers dropped
    const debug::ListingLine* nop = lineAt(l, 0x40C8);
    CHECK(nop && endsWith(fileOf(l, nop), "include_part.asm") && nop->line == 3);
    const debug::ListingLine* data = lineAt(l, 0x40CA);
    CHECK(data && endsWith(fileOf(l, data), "include_main.asm") && data->line == 7 && data->bytes.size() == 15);
    const debug::ListingLine* bch = lineAt(l, 0x40D9);
    CHECK(bch && bch->line == 8 && endsWith(fileOf(l, bch), "include_main.asm"));
    CHECK(l.symbols.count("HELPER") && l.symbols.at("HELPER") == 0x40C8);
}

void test_sdas_z80_relocated() {
    debug::Listing rst, lst;
    std::string error;
    CHECK(debug::loadListing(kDir + "sdas-z80/blink.rst", &rst, &error));
    CHECK(endsWith(rst.files[0], "sdas-z80/blink.asm"));
    const debug::ListingLine* ld = lineAt(rst, 0xC0C5);
    CHECK(ld && ld->line == 8 && ld->bytes == std::vector<uint8_t>({0x21, 0xD8, 0xC0}));
    const debug::ListingLine* call = lineAt(rst, 0xC0CB);
    CHECK(call && call->line == 12 && call->bytes == std::vector<uint8_t>({0xCD, 0xD1, 0xC0}));
    const debug::ListingLine* djnz = lineAt(rst, 0xC0D4);
    CHECK(djnz && djnz->line == 19);
    CHECK(rst.symbols.count("start") && rst.symbols.at("start") == 0xC0C5);
    CHECK(rst.symbols.count("counter") && rst.symbols.at("counter") == 0xC0D8);
    CHECK(!rst.symbols.count("1$"));
    CHECK(totalBytes(rst) == 0xC0D9 - 0xC0C5);
    // The unlinked .lst holds area offsets.
    CHECK(debug::loadListing(kDir + "sdas-z80/blink.lst", &lst, &error));
    CHECK(lineAt(lst, 0x0000) && lineAt(lst, 0x0000)->line == 8);
}

void test_zasm_include_and_wrapped_data() {
    debug::Listing l;
    std::string error;
    CHECK(debug::loadListing(kDir + "zasm/include_main.lst", &l, &error));
    CHECK(l.format == "zasm");
    CHECK(l.files.size() == 2 && endsWith(l.files[0], "zasm/include_main.asm"));
    const debug::ListingLine* call = lineAt(l, 0xC0C5);
    CHECK(call && call->line == 3 && call->bytes.size() == 3);
    const debug::ListingLine* nop = lineAt(l, 0xC0C8);
    CHECK(nop && endsWith(fileOf(l, nop), "include_part.asm") && nop->line == 1);
    const debug::ListingLine* data = lineAt(l, 0xC0CA);
    CHECK(data && data->line == 5 && data->bytes.size() == 15);
    const debug::ListingLine* text = lineAt(l, 0xC0D9);
    CHECK(text && text->line == 6 && text->bytes.size() == 27);
    const debug::ListingLine* jr = lineAt(l, 0xC0F4);
    CHECK(jr && jr->line == 7 && endsWith(fileOf(l, jr), "include_main.asm"));
    CHECK(l.symbols.count("helper") && l.symbols.at("helper") == 0xC0C8);
}

void test_zasm_rom_dumper_against_its_symbol_table() {
    debug::Listing l;
    std::string error;
    CHECK(debug::loadListing(kDir + "zasm/pc1600-rom-dumper.lst", &l, &error));
    CHECK(l.warnings.empty());
    CHECK(l.lines.size() > 300);

    // Every listed byte is where zasm put it in the binary (loaded at C0C5).
    std::ifstream bin(kDir + "zasm/pc1600-rom-dumper.bin", std::ios::binary);
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
    bool same = image.size() > 2000;
    for (const auto& line : l.lines)
        for (size_t k = 0; k < line.bytes.size(); k++) {
            const size_t off = size_t(line.addr - 0xC0C5) + k;
            same = same && off < image.size() && image[off] == line.bytes[k];
        }
    CHECK(same);

    std::ifstream in(kDir + "zasm/pc1600-rom-dumper.lst");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // zasm's own symbol table names each label's file:line. Wherever code
    // starts at a label's address, our walk must have put it at or just
    // after that line (the label may sit alone on its line).
    const std::regex sym(R"((\w+)\s*= \$([0-9A-F]{4}) =\s*\d+\s+CODE\s+pc1600-rom-dumper\.asm:(\d+))");
    int compared = 0, agreed = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), sym); it != std::sregex_iterator(); ++it) {
        const uint16_t addr = uint16_t(std::stoul((*it)[2].str(), nullptr, 16));
        const int line = std::stoi((*it)[3].str());
        const debug::ListingLine* code = lineAt(l, addr);
        if (!code || (*it)[1].str() == "CODE") continue; // the segment, not a label
        compared++;
        if (code->line >= line && code->line <= line + 3) agreed++;
        else std::fprintf(stderr, "  %s @%04X: symbol line %d, placed at %d\n", (*it)[1].str().c_str(), addr, line, code->line);
    }
    CHECK(compared > 50);
    CHECK(agreed == compared);
}

void test_symbols_file() {
    debug::Listing l;
    std::string error;
    CHECK(debug::loadSymbolFile(kDir + "symbols.sym", &l, &error));
    CHECK(l.symbols.size() == 3);
    CHECK(l.symbols.at("BANKSEL_1500") == 0x5808 && l.symbols.at("FIRMWARE_START") == 0x00C5);
    CHECK(!debug::loadSymbolFile(kDir + "sdas-lh5801/memtest.asm", &l, &error));
}

void test_include_target() {
    CHECK(debug::includeTarget("\t.include \"lib/math.asm\"") == "lib/math.asm");
    CHECK(debug::includeTarget("#include \"a.inc\" ; x") == "a.inc");
    CHECK(debug::includeTarget("  include 'b.s'") == "b.s");
    CHECK(debug::includeTarget("; .include \"no.asm\"").empty());
    CHECK(debug::includeTarget("\tld a,(hl)").empty());
}

// ── SourceMap ─────────────────────────────────────────────────────────────

void test_source_map() {
    debug::Listing memtest;
    std::string error;
    CHECK(debug::loadListing(kDir + "sdas-lh5801/memtest.rst", &memtest, &error));
    const std::string asmFile = memtest.files[0];
    std::ifstream bin("Core/tests/fixtures/listings/sdas-lh5801/memtest_stock.bin", std::ios::binary);
    std::vector<uint8_t> mem(65536, 0);
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
    for (size_t i = 0; i < image.size(); i++) mem[0x40C5 + i] = image[i];
    const debug::CodePeek peek = [&mem](int, uint16_t a, uint8_t* v) { *v = mem[a]; return true; };
    int pvNow = 0;
    const debug::BankMatch match = [&pvNow](int, const debug::BankKey& k, uint16_t) { return k.pv < 0 || k.pv == pvNow; };

    debug::SourceMap map;
    const uint16_t hi = uint16_t(0x40C5 + image.size() - 1);
    const int id = map.addLoaded(1, memtest, {}, 0x40C5, hi, "memtest.rst");
    CHECK(map.verify(id, match, peek) == 0 && !map.binding(id)->stale);

    debug::SourceLocation loc;
    CHECK(map.lookup(1, 0x40CE, match, &loc) && loc.file == asmFile && loc.line == 90);
    CHECK(map.lookup(1, 0x40CF, match, &loc) && loc.line == 90); // inside the instruction
    CHECK(!map.lookup(2, 0x40CE, match, &loc));                  // other CPU
    CHECK(!map.lookup(1, 0x5000, match, &loc));

    int resolved = 0;
    auto addrs = map.addressesFor(asmFile, 86, &resolved);       // "MEMTEST:" -- no code on the label line
    CHECK(resolved == 89 && addrs.size() == 1 && addrs[0].addr == 0x40CC && addrs[0].thread == 1);
    addrs = map.addressesFor(asmFile, 1, &resolved);             // the header comment
    CHECK(resolved == 74 && addrs.size() == 1 && addrs[0].addr == 0x40C5);
    CHECK(map.addressesFor(asmFile, 9999, &resolved).empty() && resolved == 0);
    CHECK(map.knowsFile(asmFile) && !map.knowsFile("/nowhere.asm"));

    uint16_t v = 0;
    CHECK(map.symbolValue("ERR_FLAG", &v) && v == 0x40C7);
    CHECK(map.symbolAt(1, 0x40CC) == "MEMTEST");

    // Overwritten code: a live lookup with peek falls back; verify marks it stale.
    mem[0x40CE] = 0x00;
    CHECK(!map.lookup(1, 0x40CE, match, &loc, peek));
    CHECK(map.lookup(1, 0x40CC, match, &loc, peek));             // other lines still match
    CHECK(map.verify(id, match, peek) == 1 && map.binding(id)->stale);
    CHECK(!map.lookup(1, 0x40CC, match, &loc));
    CHECK(!map.symbolValue("ERR_FLAG", &v));                     // a stale listing's symbols don't count
    CHECK(map.addressesFor(asmFile, 89, &resolved).empty());
    mem[0x40CE] = 0xAE;
    CHECK(map.verify(id, match, peek) == 0);
    CHECK(map.symbolValue("ERR_FLAG", &v) && v == 0x40C7);

    // A static listing with a PV qualifier, underneath the loaded one.
    const int rom = map.addStatic(1, memtest, {-1, -1, -1, 1}, "rom-at-pv1");
    CHECK(map.lookup(1, 0x40CE, match, &loc) && loc.binding == id); // loaded wins
    map.remove(id);
    CHECK(!map.lookup(1, 0x40CE, match, &loc));                      // pv is 0
    pvNow = 1;
    CHECK(map.lookup(1, 0x40CE, match, &loc) && loc.binding == rom);
    addrs = map.addressesFor(asmFile, 90, &resolved);
    CHECK(addrs.size() == 1 && addrs[0].key.pv == 1);

    // A newer load replaces only the loads it overlaps.
    debug::SourceMap loads;
    const int a = loads.addLoaded(1, memtest, {}, 0x40C5, 0x40FF, "a");
    const int b = loads.addLoaded(1, memtest, {}, 0x4100, hi, "b");
    CHECK(loads.bindings().size() == 2);
    const int c = loads.addLoaded(1, memtest, {}, 0x40F0, 0x4110, "c");
    CHECK(loads.bindings().size() == 1 && !loads.binding(a) && !loads.binding(b) && loads.binding(c));
    loads.addLoaded(2, memtest, {}, 0x40C5, hi, "other cpu");
    CHECK(loads.bindings().size() == 2);
}

void test_symbols_per_binding() {
    // Symbols carry the thread and bank key of the binding that defines
    // them; a stale binding's symbols don't count.
    auto reader = [](const std::string& path, std::vector<std::string>* lines) {
        if (path.find("lh.sym") != std::string::npos) *lines = {".SYMBOLS:", "C003 LOOP", "C010 ONLY_LH"};
        else if (path.find("z.sym") != std::string::npos) *lines = {".SYMBOLS:", "0100 LOOP"};
        else return false;
        return true;
    };
    debug::Listing lh, z;
    std::vector<std::string> warnings;
    CHECK(debug::loadListingWithSymbols({}, {}, {"/x/lh.sym"}, &lh, &warnings, reader) && warnings.empty());
    CHECK(debug::loadListingWithSymbols({}, {}, {"/x/z.sym", "/x/missing.sym"}, &z, &warnings, reader));
    CHECK(warnings.size() == 1 && lh.symbols.size() == 2 && z.symbols.size() == 1);
    debug::Listing none;
    CHECK(!debug::loadListingWithSymbols({}, {}, {"/x/missing.sym"}, &none, &warnings, reader));

    debug::SourceMap map;
    map.addStatic(2, lh, {-1, -1, -1, 1}, "lh.sym");
    const int zid = map.addLoaded(1, z, {}, 0x0100, 0x01FF, "z.sym");
    debug::SourceMap::SymbolInfo info;
    CHECK(map.findSymbol("LOOP", &info) && info.thread == 1 && info.value == 0x0100 && info.binding == zid); // loaded first
    CHECK(map.findSymbol("ONLY_LH", &info) && info.thread == 2 && info.value == 0xC010 && info.key.pv == 1);
    // A symbols-only binding has no lines, so it is never stale.
    CHECK(map.verify(zid, {}, [](int, uint16_t, uint8_t*) { return false; }) == 0 && !map.binding(zid)->stale);
    CHECK(!map.findSymbol("NOPE", &info));
}

} // namespace

int run_listing_tests() {
    test_sdas_lh5801_memtest();
    test_sdas_include_and_wrapped_data();
    test_sdas_z80_relocated();
    test_zasm_include_and_wrapped_data();
    test_zasm_rom_dumper_against_its_symbol_table();
    test_symbols_file();
    test_include_target();
    test_source_map();
    test_symbols_per_binding();
    std::printf("listing tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
