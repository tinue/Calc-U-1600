// Headless C++ tests for Core/Connector/BatteryCardInstance.hpp -- the
// addressed-hex writer + battery-card instance splice logic used by the
// Qt6 prototype's "Name & Save" / autosave flow. Same no-framework,
// assert-and-tally style as memory_card_tests.cpp.
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../Connector/BatteryCardInstance.hpp"
#include "../Connector/FloppyImageFile.hpp"
#include "../Connector/MemoryCardCatalog.hpp"
#include "../Connector/MemoryCardDefinition.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

std::vector<uint8_t> uniformBank(size_t n, uint8_t v) { return std::vector<uint8_t>(n, v); }

void test_addressed_hex_roundtrip_uniform() {
    auto bank = uniformBank(0x4000, 0xFF);
    auto lines = formatAddressedHexLines(bank);
    CHECK(lines.size() == 1);  // whole bank collapses to one run line

    std::string text;
    for (const auto& l : lines) { text += l; text += "\n"; }
    std::vector<uint8_t> out;
    std::string err;
    CHECK(mcd_detail::parseAddressedHex(text, static_cast<uint32_t>(bank.size()), &out, &err));
    CHECK(out == bank);
}

void test_addressed_hex_roundtrip_mixed() {
    std::vector<uint8_t> bank(0x100, 0);
    for (size_t i = 0; i < bank.size(); ++i) bank[i] = static_cast<uint8_t>(i * 7 + 3);
    auto lines = formatAddressedHexLines(bank);
    CHECK(lines.size() == bank.size() / 16);  // no run collapsing possible

    std::string text;
    for (const auto& l : lines) { text += l; text += "\n"; }
    std::vector<uint8_t> out;
    std::string err;
    CHECK(mcd_detail::parseAddressedHex(text, static_cast<uint32_t>(bank.size()), &out, &err));
    CHECK(out == bank);
}

void test_addressed_hex_roundtrip_uniform_run_plus_tail() {
    // A long uniform run followed by a differing tail -- the run must
    // collapse exactly up to the first row that isn't uniform.
    std::vector<uint8_t> bank(0x40, 0x00);
    for (size_t i = 0x30; i < bank.size(); ++i) bank[i] = static_cast<uint8_t>(i);
    auto lines = formatAddressedHexLines(bank);
    // Rows 0x00,0x10,0x20 uniform-collapse into one run line; row 0x30 is
    // the mixed tail -> one more line.
    CHECK(lines.size() == 2);

    std::string text;
    for (const auto& l : lines) { text += l; text += "\n"; }
    std::vector<uint8_t> out;
    std::string err;
    CHECK(mcd_detail::parseAddressedHex(text, static_cast<uint32_t>(bank.size()), &out, &err));
    CHECK(out == bank);
}

void test_addressed_hex_single_differing_byte() {
    std::vector<uint8_t> bank(0x20, 0xAA);
    bank[5] = 0x11;
    auto lines = formatAddressedHexLines(bank);
    CHECK(lines.size() == 2);  // row 0 (mixed), row 1 (uniform)

    std::string text;
    for (const auto& l : lines) { text += l; text += "\n"; }
    std::vector<uint8_t> out;
    std::string err;
    CHECK(mcd_detail::parseAddressedHex(text, static_cast<uint32_t>(bank.size()), &out, &err));
    CHECK(out == bank);
}

void test_battery_card_block_keeps_uniform_banks() {
    // A uniform bank must still be written: dropped, it would reload at the
    // region's power-up-fill (0xFF here), not the 0x00 it held.
    std::vector<uint8_t> image;
    auto zeroBank = uniformBank(0x100, 0x00);
    std::vector<uint8_t> mixedBank(0x100, 0);
    mixedBank[10] = 0x42;
    image.insert(image.end(), zeroBank.begin(), zeroBank.end());
    image.insert(image.end(), mixedBank.begin(), mixedBank.end());

    auto lines = formatBatteryCardInitialContentBlock(2, image);
    bool sawBank0 = false;
    bool sawBank1 = false;
    for (const auto& l : lines) {
        if (l.find("- bank: 0") != std::string::npos) sawBank0 = true;
        if (l.find("- bank: 1") != std::string::npos) sawBank1 = true;
    }
    CHECK(sawBank0);
    CHECK(sawBank1);

    std::string yaml =
        "module-name: X\n"
        "compatible-hosts: [PC-1500]\n"
        "definition-terminology: PC-1500\n"
        "regions:\n"
        "  - name: sram\n"
        "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
        "    addressing: { chip-select: Y0 }\n"
        "    banking:\n"
        "      latch: { type: trigger-based, trigger: { pin: 16 }, source-domain: address, sampled-lines: [A0] }\n"
        "      bank-count: 2\n"
        "      bank-size: 0x0100\n"
        "      bank-window: { chip-select: Y0, span: 0x0100 }\n";
    for (const auto& l : lines) { yaml += l; yaml += "\n"; }
    MemoryCardDefinition def;
    std::string err;
    CHECK(parseMemoryCardDefinition(yaml, &def, &err));
    if (!err.empty()) std::fprintf(stderr, "  (parse error: %s)\n", err.c_str());
    if (!def.regions.empty()) {
        auto it = def.regions[0].initialContentByBank.find(0);
        CHECK(it != def.regions[0].initialContentByBank.end());
        if (it != def.regions[0].initialContentByBank.end()) CHECK(it->second == zeroBank);
    }
}

// Minimal single-region, single-bank battery-style card definition, for
// splice/parse round-tripping without depending on a real bundled file.
// Real battery cards (CE-1638/CE-1601M/superram) are always *banked* --
// initial-content:'s `bank:` field is only valid on a banked region -- so
// this uses a trivial trigger-based banking with bank-count: 1 to match
// that shape rather than an unbanked region.
const char* kSyntheticCardYaml =
    "module-name: Synthetic\n"
    "compatible-hosts: [PC-1500]\n"
    "definition-terminology: PC-1500\n"
    "battery: true\n"
    "regions:\n"
    "  - name: sram\n"
    "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "    addressing: { chip-select: Y0 }\n"
    "    banking:\n"
    "      latch: { type: trigger-based, trigger: { pin: 16 }, source-domain: address, sampled-lines: [A0] }\n"
    "      bank-count: 1\n"
    "      bank-size: 0x0100\n"
    "      bank-window: { chip-select: Y0, span: 0x0100 }\n";

void test_battery_card_block_never_empty_when_all_banks_uniform() {
    // A freshly-attached, never-written-to card: every bank sits at its
    // power-up-fill value. Core's parseInitialContent() rejects an empty
    // 'blocks' list, so the writer must emit explicit blocks here too.
    std::vector<uint8_t> image(2 * 0x100, 0xFF);
    auto lines = formatBatteryCardInitialContentBlock(2, image);
    bool sawBankEntry = false;
    for (const auto& l : lines)
        if (l.find("- bank:") != std::string::npos) sawBankEntry = true;
    CHECK(sawBankEntry);

    // The resulting block must parse back through the real Core parser,
    // which is exactly what enforces the *.card.yaml non-empty-blocks
    // requirement.
    std::string yaml =
        "module-name: X\n"
        "compatible-hosts: [PC-1500]\n"
        "definition-terminology: PC-1500\n"
        "regions:\n"
        "  - name: sram\n"
        "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
        "    addressing: { chip-select: Y0 }\n"
        "    banking:\n"
        "      latch: { type: trigger-based, trigger: { pin: 16 }, source-domain: address, sampled-lines: [A0] }\n"
        "      bank-count: 2\n"
        "      bank-size: 0x0100\n"
        "      bank-window: { chip-select: Y0, span: 0x0100 }\n";
    for (const auto& l : lines) { yaml += l; yaml += "\n"; }
    MemoryCardDefinition def;
    std::string err;
    CHECK(parseMemoryCardDefinition(yaml, &def, &err));
    if (!err.empty()) std::fprintf(stderr, "  (parse error: %s)\n", err.c_str());
}

void test_battery_card_block_unbanked_region_omits_bank_key() {
    // An unbanked battery card (e.g. CE-1600M: "banking: none",
    // capacity: 0x8000) reports debugBankCount() == -1 (ExpansionCard's
    // "no bank concept" convention) -- formatBatteryCardInitialContentBlock
    // must then omit the 'bank:' key entirely (Core's parseInitialContent
    // rejects 'bank' on an unbanked region's blocks).
    std::vector<uint8_t> image(0x100, 0);
    image[3] = 0x99;
    auto lines = formatBatteryCardInitialContentBlock(-1, image);
    for (const auto& l : lines) CHECK(l.find("bank:") == std::string::npos);
    bool sawOffset = false;
    for (const auto& l : lines)
        if (l.find("- offset: 0x0000") != std::string::npos) sawOffset = true;
    CHECK(sawOffset);

    const char* unbankedCard =
        "module-name: X\n"
        "compatible-hosts: [PC-1500]\n"
        "definition-terminology: PC-1500\n"
        "regions:\n"
        "  - name: sram\n"
        "    capacity: 0x0100\n"
        "    banking: none\n"
        "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
        "    addressing: { chip-select: Y0, span: 0x0100 }\n";
    std::string yaml = unbankedCard;
    for (const auto& l : lines) { yaml += l; yaml += "\n"; }
    MemoryCardDefinition def;
    std::string err;
    CHECK(parseMemoryCardDefinition(yaml, &def, &err));
    if (!err.empty()) std::fprintf(stderr, "  (parse error: %s)\n", err.c_str());
    if (!def.regions.empty()) {
        auto it = def.regions[0].initialContentByBank.find(0);
        CHECK(it != def.regions[0].initialContentByBank.end());
        if (it != def.regions[0].initialContentByBank.end()) CHECK(it->second == image);
    }
}

void test_splice_first_save_inserts_header_and_content() {
    std::vector<uint8_t> image(0x100, 0);
    image[3] = 0x99;
    auto contentLines = formatBatteryCardInitialContentBlock(1, image);

    std::string spliced;
    std::string err;
    CHECK(spliceBatteryCardInstance(kSyntheticCardYaml, "MySave", "Synthetic", contentLines, &spliced,
                                    &err, "2026-01-01T00:00:00Z"));
    CHECK(spliced.find("# Battery-card instance -- generated by Calc-U-1600 from \"Synthetic\".") !=
          std::string::npos);
    CHECK(spliced.find("# created: 2026-01-01T00:00:00Z") != std::string::npos);
    CHECK(spliced.find("# last-saved: 2026-01-01T00:00:00Z") != std::string::npos);
    CHECK(spliced.find("module-name: \"MySave\"") != std::string::npos);
    CHECK(spliced.find("initial-content:") != std::string::npos);

    MemoryCardDefinition def;
    CHECK(parseMemoryCardDefinition(spliced, &def, &err));
    CHECK(def.moduleName == "MySave");
    CHECK(def.regions.size() == 1);
    auto it = def.regions[0].initialContentByBank.find(0);
    CHECK(it != def.regions[0].initialContentByBank.end());
    if (it != def.regions[0].initialContentByBank.end()) CHECK(it->second == image);
}

void test_splice_resave_preserves_created_updates_last_saved() {
    std::vector<uint8_t> image1(0x100, 0);
    auto contentLines1 = formatBatteryCardInitialContentBlock(1, image1);
    std::string firstSave;
    std::string err;
    CHECK(spliceBatteryCardInstance(kSyntheticCardYaml, "MySave", "Synthetic", contentLines1, &firstSave,
                                    &err, "2026-01-01T00:00:00Z"));

    std::vector<uint8_t> image2(0x100, 0);
    image2[7] = 0x55;
    auto contentLines2 = formatBatteryCardInitialContentBlock(1, image2);
    std::string resaved;
    CHECK(spliceBatteryCardInstance(firstSave, "MySave", "MySave", contentLines2, &resaved, &err,
                                    "2026-01-02T00:00:00Z"));

    CHECK(resaved.find("# created: 2026-01-01T00:00:00Z") != std::string::npos);
    // The origin names the template, not the instance it was re-saved from.
    CHECK(resaved.find("generated by Calc-U-1600 from \"Synthetic\".") != std::string::npos);
    CHECK(resaved.find("# last-saved: 2026-01-02T00:00:00Z") != std::string::npos);
    // Exactly one header block -- no accumulation across re-saves.
    size_t firstPos = resaved.find("# Battery-card instance");
    size_t secondPos = resaved.find("# Battery-card instance", firstPos + 1);
    CHECK(secondPos == std::string::npos);

    MemoryCardDefinition def;
    CHECK(parseMemoryCardDefinition(resaved, &def, &err));
    auto it = def.regions[0].initialContentByBank.find(0);
    CHECK(it != def.regions[0].initialContentByBank.end());
    if (it != def.regions[0].initialContentByBank.end()) CHECK(it->second == image2);
}

void test_card_template_key() {
    std::string err;
    MemoryCardDefinition def;
    CHECK(parseMemoryCardDefinition(kSyntheticCardYaml, &def, &err));
    CHECK(!def.isTemplate);
    const std::string templ = std::string("template: true\n") + kSyntheticCardYaml;
    MemoryCardDefinition templDef;
    CHECK(parseMemoryCardDefinition(templ, &templDef, &err));
    CHECK(templDef.isTemplate);
    MemoryCardDefinition bad;
    CHECK(!parseMemoryCardDefinition(std::string("template: maybe\n") + kSyntheticCardYaml, &bad, &err));
}

// Saving from a template yields an instance: the `template:` line is gone.
void test_splice_drops_template_key() {
    const std::string templ = std::string("template: true\n") + kSyntheticCardYaml;
    std::vector<uint8_t> image(0x100, 0);
    std::string spliced, err;
    CHECK(spliceBatteryCardInstance(templ, "MySave", "Synthetic", formatBatteryCardInitialContentBlock(1, image),
                                    &spliced, &err));
    CHECK(spliced.find("template:") == std::string::npos);
    MemoryCardDefinition def;
    CHECK(parseMemoryCardDefinition(spliced, &def, &err));
    CHECK(!def.isTemplate);
}

// Every bundled card and floppy declares itself a template -- the app never
// writes to them (and the bundle may be read-only anyway).
void test_bundled_files_are_templates() {
    const char* dir = "Qt6/resources/cards";
    std::string err;
    const auto cards = scanMemoryCardDirectory(dir, &err);
    if (cards.empty()) {
        std::fprintf(stderr, "SKIP test_bundled_files_are_templates: %s not found\n", dir);
        return;
    }
    CHECK(err.empty());
    for (const auto& e : cards) {
        if (!e.isTemplate) std::fprintf(stderr, "  not a template: %s\n", e.filePath.c_str());
        CHECK(e.isTemplate);
    }
    const auto disks = scanFloppyDirectory(dir, &err);
    CHECK(!disks.empty());
    for (const auto& e : disks) {
        if (!e.isTemplate) std::fprintf(stderr, "  not a template: %s\n", e.filePath.c_str());
        CHECK(e.isTemplate);
    }
    MemoryCardCatalogEntry one;
    CHECK(readMemoryCardCatalogEntry(cards.front().filePath, &one, &err));
    CHECK(one.isTemplate && one.moduleName == cards.front().moduleName);
    FloppyCatalogEntry disk;
    CHECK(readFloppyCatalogEntry(disks.front().filePath, &disk, &err));
    CHECK(disk.isTemplate && disk.diskName == disks.front().diskName);
}

// A saved floppy is an instance: formatFloppyFile() never writes `template:`.
void test_saved_floppy_is_instance() {
    const std::vector<uint8_t> image(CE1600FCard::kImageSize, 0xE5);
    FloppyFile disk;
    std::string err;
    CHECK(parseFloppyFile(formatFloppyFile("Mine", image), &disk, &err));
    CHECK(disk.diskName == "Mine" && !disk.isTemplate);
}

void test_splice_fails_without_module_name() {
    const char* noModuleName =
        "compatible-hosts: [PC-1500]\n"
        "definition-terminology: PC-1500\n"
        "regions:\n"
        "  - name: sram\n";
    std::string outText, err;
    CHECK(!spliceBatteryCardInstance(noModuleName, "X", "X", {}, &outText, &err));
    CHECK(!err.empty());
}

void test_splice_fails_without_regions() {
    const char* noRegions =
        "module-name: X\n"
        "compatible-hosts: [PC-1500]\n"
        "definition-terminology: PC-1500\n";
    std::string outText, err;
    CHECK(!spliceBatteryCardInstance(noRegions, "X", "X", {}, &outText, &err));
    CHECK(!err.empty());
}

void test_splice_roundtrip_through_real_bundled_file_if_present() {
    const char* path = "Qt6/resources/cards/ce1638.card.yaml";
    std::ifstream probe(path);
    if (!probe) {
        std::fprintf(stderr, "SKIP test_splice_roundtrip_through_real_bundled_file_if_present: %s not found\n",
                      path);
        return;
    }
    std::stringstream ss;
    ss << probe.rdbuf();
    const std::string sourceText = ss.str();

    MemoryCardDefinition orig;
    std::string err;
    CHECK(parseMemoryCardDefinition(sourceText, &orig, &err));
    if (orig.regions.empty()) return;

    const int bankCount = static_cast<int>(orig.regions[0].banking.bankCount);
    const size_t bankSize = orig.regions[0].banking.bankSize;
    std::vector<uint8_t> image(bankSize * static_cast<size_t>(bankCount), 0);
    if (!image.empty()) image[0] = 0x7E;  // ensure at least one non-uniform bank

    auto contentLines = formatBatteryCardInitialContentBlock(bankCount, image);
    std::string spliced;
    CHECK(spliceBatteryCardInstance(sourceText, "MyCE1638", orig.moduleName, contentLines, &spliced, &err));

    MemoryCardDefinition parsed;
    CHECK(parseMemoryCardDefinition(spliced, &parsed, &err));
    CHECK(parsed.moduleName == "MyCE1638");
    CHECK(!parsed.isTemplate);
}

}  // namespace

int run_battery_card_instance_tests() {
    test_addressed_hex_roundtrip_uniform();
    test_addressed_hex_roundtrip_mixed();
    test_addressed_hex_roundtrip_uniform_run_plus_tail();
    test_addressed_hex_single_differing_byte();
    test_battery_card_block_keeps_uniform_banks();
    test_battery_card_block_never_empty_when_all_banks_uniform();
    test_battery_card_block_unbanked_region_omits_bank_key();

    test_splice_first_save_inserts_header_and_content();
    test_splice_resave_preserves_created_updates_last_saved();
    test_card_template_key();
    test_splice_drops_template_key();
    test_bundled_files_are_templates();
    test_saved_floppy_is_instance();
    test_splice_fails_without_module_name();
    test_splice_fails_without_regions();
    test_splice_roundtrip_through_real_bundled_file_if_present();

    std::printf("battery_card_instance_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
