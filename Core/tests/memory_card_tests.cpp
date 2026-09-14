// Headless C++ tests for the universal software-defined memory module:
//   * Core/Yaml.hpp                  -- the YAML-subset reader
//   * Core/Connector/MemoryCardDefinition.hpp -- parse + validate + resolve
//   * Core/Connector/SoftwareDefinedCard.hpp  -- the ExpansionCard itself
//   * the `- modulespec:` preset hook and the OUT (28H) -> Slot 2 route
//
// Same no-framework, assert-and-tally style as lh5801_tests.cpp.
// Build & run: see tools/run_tests.sh
//
// The card-definition inputs here are inline strings, not the files under
// examples/memory-cards/ (those are demo assets that get renamed/edited);
// the one file-based test SKIPs if its example is absent.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include "../Connector/CE155Card.hpp"
#include "../Connector/MemoryCardCatalog.hpp"
#include "../Connector/MemoryCardDefinition.hpp"
#include "../Connector/PlainRamCard.hpp"
#include "../Connector/SoftwareDefinedCard.hpp"
#include "../PC1500/PresetFile.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600PresetLoader.hpp"
#include "../Yaml.hpp"
#include "PresetTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ── inline card definitions (CE-155 / CE-1600M / CE-1601M shapes) ───────

const char* kCe155Yaml =
    "module-name: CE-155\n"
    "compatible-hosts: [PC-1500, PC-1500A, PC-1600-Slot-1]\n"
    "definition-terminology: PC-1500\n"
    "regions:\n"
    "  - name: sram-8k\n"
    "    capacity: 0x2000\n"
    "    banking: none\n"
    "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "    addressing:\n"
    "      any-of:\n"
    "        - all-of:\n"
    "            - chip-select: Y0\n"
    "            - address-bits: { A13: 1, A12: 1, A11: 1 }\n"
    "          span: 0x0800\n"
    "          maps-to: 0x0000\n"
    "        - { chip-select: S1, span: 0x0800, maps-to: 0x0800 }\n"
    "        - { chip-select: S2, span: 0x0800, maps-to: 0x1000 }\n"
    "        - { chip-select: S3, span: 0x0800, maps-to: 0x1800 }\n";

const char* kCe1600mYaml =
    "module-name: CE-1600M\n"
    "compatible-hosts: [PC-1600-Slot-1, PC-1600-Slot-2]\n"
    "definition-terminology: PC-1600-Slot-1\n"
    "regions:\n"
    "  - name: sram-32k\n"
    "    capacity: 0x8000\n"
    "    banking: none\n"
    "    pc1600-module-class: plain-ram-no-header\n"
    "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "    addressing:\n"
    "      any-of:\n"
    "        - all-of: [ { chip-select: RAM2 }, { signal-negated: PVOUT } ]\n"
    "          span: 0x4000\n"
    "          maps-to: 0x0000\n"
    "        - all-of: [ { chip-select: RAM2 }, { signal: PVOUT } ]\n"
    "          span: 0x4000\n"
    "          maps-to: 0x4000\n";

const char* kCe1601mYaml =
    "module-name: CE-1601M\n"
    "compatible-hosts: [PC-1600-Slot-2]\n"
    "definition-terminology: PC-1600-Slot-2\n"
    "regions:\n"
    "  - name: vram-64k\n"
    "    pc1600-module-class: plain-ram-no-header\n"
    "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "    addressing: { chip-select: RAM1 }\n"
    "    banking:\n"
    "      latch:\n"
    "        type: trigger-based\n"
    "        trigger: { io-port: 0x28 }\n"
    "        sampled-lines: [D0, D1, D2]\n"
    "        source-domain: data\n"
    "      bank-count: 2\n"
    "      bank-size: 0x8000\n"
    "      bank-window:\n"
    "        any-of:\n"
    "          - all-of: [ { chip-select: RAM1 }, { signal-negated: PVOUT } ]\n"
    "            span: 0x4000\n"
    "            maps-to: 0x0000\n"
    "          - all-of: [ { chip-select: RAM1 }, { signal: PVOUT } ]\n"
    "            span: 0x4000\n"
    "            maps-to: 0x4000\n";

// superRAM -- the "maxed-out Slot 2" card: mechanically a CE-1601M with a
// 4-bit vertical-bank register (D0-D3, not D0-D2) and all 16 x 32 KB banks
// fitted = 512 KB. Same trigger-based OUT (28H) / data-domain latch, same
// PVOUT half-select nesting.
const char* kSuperRamYaml =
    "module-name: superRAM\n"
    "compatible-hosts: [PC-1600-Slot-2]\n"
    "definition-terminology: PC-1600-Slot-2\n"
    "regions:\n"
    "  - name: vram-512k\n"
    "    pc1600-module-class: plain-ram-no-header\n"
    "    content: { kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "    addressing: { chip-select: RAM1 }\n"
    "    banking:\n"
    "      latch:\n"
    "        type: trigger-based\n"
    "        trigger: { io-port: 0x28 }\n"
    "        sampled-lines: [D0, D1, D2, D3]\n"
    "        source-domain: data\n"
    "      bank-count: 16\n"
    "      bank-size: 0x8000\n"
    "      bank-window:\n"
    "        any-of:\n"
    "          - all-of: [ { chip-select: RAM1 }, { signal-negated: PVOUT } ]\n"
    "            span: 0x4000\n"
    "            maps-to: 0x0000\n"
    "          - all-of: [ { chip-select: RAM1 }, { signal: PVOUT } ]\n"
    "            span: 0x4000\n"
    "            maps-to: 0x4000\n";

// Minimal by-bank fixture exercising the flash engine: banks 0-1 Regular,
// banks 2-3 Flash, sized small (0x100/bank, 0x40 sectors) for cheap tests.
// Same trigger shape as CE-1638/CE-163F (pin 18, address domain), scaled
// down -- not modeling a real product.
const char* kFlashByBankYaml =
    "module-name: T-FLASH\n"
    "compatible-hosts: [PC-1500]\n"
    "definition-terminology: PC-1500\n"
    "regions:\n"
    "  - name: r\n"
    "    addressing: { chip-select: Y0 }\n"
    "    banking:\n"
    "      latch:\n"
    "        type: trigger-based\n"
    "        trigger: { pin: 18 }\n"
    "        sampled-lines: [A0, A1]\n"
    "        source-domain: address\n"
    "      bank-count: 4\n"
    "      bank-size: 0x100\n"
    "      bank-window: { chip-select: Y0, span: 0x100 }\n"
    "    content:\n"
    "      by-bank:\n"
    "        - { banks: \"0-1\", kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "        - banks: \"2-3\"\n"
    "          kind: flash\n"
    "          power-up-fill: 0xAA\n"
    "          protocol:\n"
    "            unlock-sequence:\n"
    "              - { address: 0x55, data: 0xAA }\n"
    "              - { address: 0x2A, data: 0x55 }\n"
    "            byte-program-command: 0xA0\n"
    "            erase-setup-command: 0x80\n"
    "            sector-erase-command: 0x30\n"
    "            chip-erase-command: 0x10\n"
    "            reset-command: 0xF0\n"
    "            sector-size: 0x40\n";

std::unique_ptr<SoftwareDefinedCard> buildCard(const char* yaml, CardHost host) {
    MemoryCardDefinition def;
    std::string err;
    if (!parseMemoryCardDefinition(yaml, &def, &err)) {
        std::fprintf(stderr, "  buildCard parse error: %s\n", err.c_str());
        return nullptr;
    }
    if (!def.compatibleWith(host)) return nullptr;
    return std::make_unique<SoftwareDefinedCard>(std::move(def));
}

// ── YAML reader ──────────────────────────────────────────────────────────

void test_yaml_block_map_and_scalars() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("a: 1\nb: hello\nc: true\n", &n, &err));
    CHECK(n.isMap() && n.map.size() == 3);
    long v = 0;
    CHECK(n.find("a") && n.find("a")->asInt(&v, &err) && v == 1);
    std::string s;
    CHECK(n.find("b") && n.find("b")->asString(&s, &err) && s == "hello");
    bool b = false;
    CHECK(n.find("c") && n.find("c")->asBool(&b, &err) && b);
}

void test_yaml_hex_ints() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("x: 0x2000\ny: 0xFF\nz: 255\n", &n, &err));
    long v = 0;
    CHECK(n.find("x")->asInt(&v, &err) && v == 0x2000);
    CHECK(n.find("y")->asInt(&v, &err) && v == 255);
    CHECK(n.find("z")->asInt(&v, &err) && v == 255);
}

void test_yaml_block_and_flow_sequences() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("flow: [a, b, c]\nblock:\n  - one\n  - two\n", &n, &err));
    const YamlNode* f = n.find("flow");
    CHECK(f && f->isSeq() && f->seq.size() == 3 && f->seq[2].scalar == "c");
    const YamlNode* bl = n.find("block");
    CHECK(bl && bl->isSeq() && bl->seq.size() == 2 && bl->seq[0].scalar == "one");
}

void test_yaml_flow_map_nested() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("bits: { A13: 1, A12: 0 }\n", &n, &err));
    const YamlNode* bits = n.find("bits");
    CHECK(bits && bits->isMap() && bits->map.size() == 2);
    long v = 0;
    CHECK(bits->find("A13")->asInt(&v, &err) && v == 1);
    CHECK(bits->find("A12")->asInt(&v, &err) && v == 0);
}

void test_yaml_seq_of_maps_with_continuation_keys() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("groups:\n"
                    "  - all-of:\n"
                    "      - chip-select: Y0\n"
                    "    span: 0x800\n"
                    "    maps-to: 0\n",
                    &n, &err));
    const YamlNode* g = n.find("groups");
    CHECK(g && g->isSeq() && g->seq.size() == 1);
    const YamlNode& item = g->seq[0];
    CHECK(item.isMap() && item.has("all-of") && item.has("span") && item.has("maps-to"));
    CHECK(item.find("all-of")->isSeq() && item.find("all-of")->seq[0].find("chip-select"));
}

void test_yaml_comments_and_blank_lines() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("# a comment\n\na: 1   # trailing\nb: \"x # y\"\n", &n, &err));
    CHECK(n.isMap() && n.map.size() == 2);
    std::string s;
    CHECK(n.find("b")->asString(&s, &err) && s == "x # y");  // '#' inside quotes kept
}

void test_yaml_block_scalar() {
    YamlNode n;
    std::string err;
    CHECK(parseYaml("bytes: |\n  43 16\n  00 00\nnext: 1\n", &n, &err));
    CHECK(n.find("bytes")->scalar == "43 16\n00 00");
    long v = 0;
    CHECK(n.find("next")->asInt(&v, &err) && v == 1);
}

void test_yaml_rejects_tabs_and_bad_indent() {
    YamlNode n;
    std::string err;
    CHECK(!parseYaml("a:\n\tb: 1\n", &n, &err) && !err.empty());
    err.clear();
    CHECK(!parseYaml("   a: 1\n", &n, &err) && !err.empty());
}

// ── definition parse / validation ───────────────────────────────────────

void test_inline_definitions_parse() {
    for (const char* y : {kCe155Yaml, kCe1600mYaml, kCe1601mYaml}) {
        MemoryCardDefinition def;
        std::string err;
        bool ok = parseMemoryCardDefinition(y, &def, &err);
        if (!ok) std::fprintf(stderr, "  (%s)\n", err.c_str());
        CHECK(ok);
    }
}

void test_ce155_definition_shape() {
    MemoryCardDefinition def;
    std::string err;
    CHECK(parseMemoryCardDefinition(kCe155Yaml, &def, &err));
    CHECK(def.terminology == CardHost::PC1500);
    CHECK(def.compatibleWith(CardHost::PC1500) && def.compatibleWith(CardHost::PC1600Slot1));
    CHECK(def.regions.size() == 1);
    const Region& r = def.regions[0];
    CHECK(!r.banked && r.capacity == 0x2000);
    CHECK(r.addressing.groups.size() == 4);
    const EnableGroup& y0 = r.addressing.groups[0];
    CHECK(y0.requireHigh.size() == 1 && y0.requireHigh[0] == 4);
    CHECK(y0.addrBits.size() == 3);
    CHECK(y0.span == 0x800 && y0.mapsTo == 0);
    CHECK(r.addressing.groups[1].requireHigh.size() == 1 &&
          r.addressing.groups[1].requireHigh[0] == 16);
    CHECK(r.addressing.groups[3].mapsTo == 0x1800);
}

// Same shape as kFlashByBankYaml (bank-size 0x100, banks 0-1 Regular /
// 2-3 Flash) plus `initial-content` on bank 0 (addressed-hex) and bank 2
// (plain hex, partial coverage -- the rest keeps the flash power-up-fill).
const char* kInitialContentYaml =
    "module-name: T-INIT\n"
    "compatible-hosts: [PC-1500]\n"
    "definition-terminology: PC-1500\n"
    "regions:\n"
    "  - name: r\n"
    "    addressing: { chip-select: Y0 }\n"
    "    banking:\n"
    "      latch:\n"
    "        type: trigger-based\n"
    "        trigger: { pin: 18 }\n"
    "        sampled-lines: [A0, A1]\n"
    "        source-domain: address\n"
    "      bank-count: 4\n"
    "      bank-size: 0x100\n"
    "      bank-window: { chip-select: Y0, span: 0x100 }\n"
    "    content:\n"
    "      by-bank:\n"
    "        - { banks: \"0-1\", kind: regular, writable: true, power-up-fill: 0xFF }\n"
    "        - banks: \"2-3\"\n"
    "          kind: flash\n"
    "          power-up-fill: 0xAA\n"
    "          protocol:\n"
    "            unlock-sequence:\n"
    "              - { address: 0x55, data: 0xAA }\n"
    "              - { address: 0x2A, data: 0x55 }\n"
    "            byte-program-command: 0xA0\n"
    "            erase-setup-command: 0x80\n"
    "            sector-erase-command: 0x30\n"
    "            chip-erase-command: 0x10\n"
    "            reset-command: 0xF0\n"
    "            sector-size: 0x40\n"
    "    initial-content:\n"
    "      blocks:\n"
    "        - bank: 0\n"
    "          offset: 0x0000\n"
    "          encoding: addressed-hex\n"
    "          bytes: |\n"
    "            $0000: 00...\n"
    "            $0010: 11 22 33 44 55 66 77 88  99 AA BB CC DD EE FF 00\n"
    "            $0020: FF...\n"
    "        - bank: 2\n"
    "          offset: 0x0010\n"
    "          encoding: hex\n"
    "          bytes: |\n"
    "            AA BB CC\n";

bool rejects(const std::string& text, std::string* err) {
    MemoryCardDefinition def;
    err->clear();
    return !parseMemoryCardDefinition(text, &def, err) && !err->empty();
}

const char* kMinPrefix =
    "module-name: T\n"
    "compatible-hosts: [PC-1500]\n"
    "definition-terminology: PC-1500\n"
    "regions:\n";

void test_reject_unknown_top_key() {
    std::string err;
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    capacity: 0x800\n    banking: none\n"
                      "    content: regular\n    addressing: { chip-select: Y0, span: 0x800 }\n"
                      "bogus-key: 1\n",
                  &err));
}

void test_reject_terminology_not_in_hosts() {
    std::string err;
    CHECK(rejects("module-name: T\n"
                  "compatible-hosts: [PC-1500]\n"
                  "definition-terminology: PC-1500A\n"
                  "regions:\n"
                  "  - name: r\n    capacity: 0x800\n    banking: none\n"
                  "    content: regular\n    addressing: { chip-select: Y0, span: 0x800 }\n",
                  &err));
}

void test_reject_tiling_gap_and_overlap() {
    std::string err;
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    capacity: 0x1000\n    banking: none\n    content: regular\n"
                      "    addressing:\n      any-of:\n"
                      "        - { chip-select: S1, span: 0x400, maps-to: 0x000 }\n"
                      "        - { chip-select: S2, span: 0x400, maps-to: 0x800 }\n",
                  &err));
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    capacity: 0x800\n    banking: none\n    content: regular\n"
                      "    addressing:\n      any-of:\n"
                      "        - { chip-select: S1, span: 0x800, maps-to: 0x000 }\n"
                      "        - { chip-select: S2, span: 0x800, maps-to: 0x400 }\n",
                  &err));
}

void test_reject_rom_flash_bybank_linebased() {
    std::string err;
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    capacity: 0x800\n    banking: none\n    content: rom\n"
                      "    addressing: { chip-select: Y0, span: 0x800 }\n",
                  &err));
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    capacity: 0x800\n    banking: none\n    content: flash\n"
                      "    addressing: { chip-select: Y0, span: 0x800 }\n",
                  &err));
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    banking:\n"
                      "        latch: { type: line-based, lines: [K0], source-domain: address-select }\n"
                      "        bank-count: 2\n        bank-size: 0x800\n"
                      "        bank-window: { chip-select: Y0, span: 0x800 }\n"
                      "    content: regular\n    addressing: { chip-select: Y0 }\n",
                  &err));
}

void test_reject_bybank_shapes() {
    std::string err;
    // by-bank on an unbanked region.
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    capacity: 0x800\n    banking: none\n"
                      "    content: { by-bank: [ { banks: \"0\", kind: regular } ] }\n"
                      "    addressing: { chip-select: Y0, span: 0x800 }\n",
                  &err));
    // Gap: banks 0-2 of a 4-bank region, nothing covers bank 3.
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0, A1], source-domain: address }\n"
                      "        bank-count: 4\n        bank-size: 0x100\n"
                      "        bank-window: { chip-select: Y0, span: 0x100 }\n"
                      "    content: { by-bank: [ { banks: \"0-2\", kind: regular } ] }\n",
                  &err));
    // Overlap: banks 0-2 and 2-3 both claim bank 2.
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0, A1], source-domain: address }\n"
                      "        bank-count: 4\n        bank-size: 0x100\n"
                      "        bank-window: { chip-select: Y0, span: 0x100 }\n"
                      "    content:\n"
                      "      by-bank:\n"
                      "        - { banks: \"0-2\", kind: regular }\n"
                      "        - { banks: \"2-3\", kind: regular }\n",
                  &err));
    // Out of range: bank-count is 4 (0-3), a range names bank 4.
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0, A1], source-domain: address }\n"
                      "        bank-count: 4\n        bank-size: 0x100\n"
                      "        bank-window: { chip-select: Y0, span: 0x100 }\n"
                      "    content: { by-bank: [ { banks: \"0-4\", kind: regular } ] }\n",
                  &err));
    // Flash content still needs a protocol, even inside a by-bank entry.
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0, A1], source-domain: address }\n"
                      "        bank-count: 4\n        bank-size: 0x100\n"
                      "        bank-window: { chip-select: Y0, span: 0x100 }\n"
                      "    content: { by-bank: [ { banks: \"0-3\", kind: flash } ] }\n",
                  &err));
}

void test_reject_unbanked_without_capacity() {
    std::string err;
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n    banking: none\n    content: regular\n"
                      "    addressing: { chip-select: Y0, span: 0x800 }\n",
                  &err));
}

// ── compatibility gate ─────────────────────────────────────────────────

void test_compat_gate_inline() {
    CHECK(buildCard(kCe1600mYaml, CardHost::PC1600Slot1) != nullptr);
    CHECK(buildCard(kCe1600mYaml, CardHost::PC1500) == nullptr);  // not in compatible-hosts
}

void test_compat_gate_via_file_if_present() {
    const char* path = "Qt6/resources/cards/ce155.card.yaml";
    std::ifstream probe(path);
    if (!probe) {
        std::fprintf(stderr, "SKIP test_compat_gate_via_file_if_present: %s not found\n", path);
        return;
    }
    std::string err;
    CHECK(makeSoftwareDefinedCard(path, CardHost::PC1500, &err) != nullptr);
    err.clear();
    CHECK(makeSoftwareDefinedCard(path, CardHost::PC1600Slot2, &err) == nullptr &&
          err.find("not compatible") != std::string::npos);
}

void test_ce1638_card_yaml_if_present() {
    const char* path = "Qt6/resources/cards/ce1638.card.yaml";
    std::ifstream probe(path);
    if (!probe) {
        std::fprintf(stderr, "SKIP test_ce1638_card_yaml_if_present: %s not found\n", path);
        return;
    }
    for (CardHost h : {CardHost::PC1500, CardHost::PC1500A, CardHost::PC1600Slot1,
                       CardHost::PC1600Slot2}) {
        std::string err;
        auto card = makeSoftwareDefinedCard(path, h, &err);
        CHECK(card != nullptr);
    }

    std::string err;
    auto card = makeSoftwareDefinedCard(path, CardHost::PC1500, &err);
    CHECK(card != nullptr);
    if (!card) return;
    CHECK(card->debugBankCount() == 8);

    auto* sd = static_cast<SoftwareDefinedCard*>(card.get());
    PinState sel;
    sel.forWrite = true;
    sel.pin[18] = true;
    sel.address = 5;  // A0-A2 -> bank 5
    CHECK(sd->respondsToWrite(sel, 0));
    CHECK(sd->currentBank() == 5);

    PinState w;
    w.pin[4] = true;
    w.forWrite = true;
    w.address = 0x1234;
    CHECK(sd->respondsToWrite(w, 0x42));
    uint8_t v = 0;
    CHECK(sd->respondsToRead(w, v) && v == 0x42);

    sel.address = 0;
    sd->respondsToWrite(sel, 0);
    CHECK(sd->respondsToRead(w, v) && v == 0xFF);  // bank 0 untouched
}

void test_ce163f_card_yaml_if_present() {
    const char* path = "Qt6/resources/cards/ce163f.card.yaml";
    std::ifstream probe(path);
    if (!probe) {
        std::fprintf(stderr, "SKIP test_ce163f_card_yaml_if_present: %s not found\n", path);
        return;
    }
    for (CardHost h : {CardHost::PC1500, CardHost::PC1500A, CardHost::PC1600Slot1,
                       CardHost::PC1600Slot2}) {
        std::string err;
        auto card = makeSoftwareDefinedCard(path, h, &err);
        CHECK(card != nullptr);
    }

    std::string err;
    auto card = makeSoftwareDefinedCard(path, CardHost::PC1500, &err);
    CHECK(card != nullptr);
    if (!card) return;
    CHECK(card->debugBankCount() == 16);
    auto* sd = static_cast<SoftwareDefinedCard*>(card.get());

    auto selectBank = [&](int bank) {
        PinState p;
        p.forWrite = true;
        p.pin[18] = true;
        p.address = uint16_t(bank);
        sd->respondsToWrite(p, 0);
    };
    auto write = [&](uint16_t off, uint8_t data) {
        PinState p;
        p.pin[4] = true;
        p.forWrite = true;
        p.address = off;
        return sd->respondsToWrite(p, data);
    };
    auto read = [&](uint16_t off) {
        uint8_t v = 0;
        PinState p;
        p.pin[4] = true;
        p.address = off;
        sd->respondsToRead(p, v);
        return v;
    };

    // Bank 0: ordinary RAM.
    selectBank(0);
    write(0x100, 0x99);
    CHECK(read(0x100) == 0x99);

    // Bank 9: FLASH, pre-loaded with real recovered firmware content
    // (`initial-content`, dumped via the "Dump Card YAML" debug feature) --
    // offset 0x0200 sits in that dump's untouched ($0120: FF...) tail, so it
    // still reads the erased-flash fill. Needs the real firmware's unlock
    // addresses (&1555/&2AAA), which the low-11-bit command-address-mask
    // reduces to the same &555/&2AA CE163FCard.hpp uses.
    selectBank(9);
    CHECK(read(0x0200) == 0xFF);
    write(0x1555, 0xAA);
    write(0x2AAA, 0x55);
    write(0x1555, 0xA0);
    write(0x0200, 0x0F);
    CHECK(read(0x0200) == (0xFF & 0x0F));
}

void test_superram_card_yaml_if_present() {
    const char* path = "Qt6/resources/cards/superram.card.yaml";
    std::ifstream probe(path);
    if (!probe) {
        std::fprintf(stderr, "SKIP test_superram_card_yaml_if_present: %s not found\n", path);
        return;
    }
    // Slot 2 only.
    for (CardHost h : {CardHost::PC1500, CardHost::PC1500A, CardHost::PC1600Slot1}) {
        std::string err;
        CHECK(makeSoftwareDefinedCard(path, h, &err) == nullptr);
    }

    std::string err;
    auto card = makeSoftwareDefinedCard(path, CardHost::PC1600Slot2, &err);
    CHECK(card != nullptr);
    if (!card) return;
    CHECK(card->debugBankCount() == 16);

    auto* sd = static_cast<SoftwareDefinedCard*>(card.get());
    PinState sel;
    sel.ioWrite = true;
    sel.forWrite = true;
    sel.address = 0x28;
    CHECK(sd->respondsToWrite(sel, 12));  // D3+D2 -> bank 12
    CHECK(sd->currentBank() == 12);

    PinState w;
    w.pin[4] = true;   // RAM1
    w.forWrite = true;
    w.address = 0x8000;
    CHECK(sd->respondsToWrite(w, 0x42));
    uint8_t v = 0;
    CHECK(sd->respondsToRead(w, v) && v == 0x42);

    sd->respondsToWrite(sel, 0);
    CHECK(sd->respondsToRead(w, v) && v == 0xFF);  // bank 0 untouched
}

// ── behaviour vs the hand-written prototypes ────────────────────────────

void test_ce155_matches_hardcoded_card() {
    auto sd = buildCard(kCe155Yaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;
    CE155Card ref;
    int mism = 0;
    for (uint32_t addr = 0x3000; addr <= 0x6000; ++addr) {
        for (int ps = 0; ps < 5; ++ps) {
            PinState p;
            p.address = static_cast<uint16_t>(addr);
            if (ps == 1) p.pin[4] = true;
            if (ps == 2) p.pin[16] = true;
            if (ps == 3) p.pin[17] = true;
            if (ps == 4) p.pin[18] = true;
            PinState pw = p;
            pw.forWrite = true;
            uint8_t val = static_cast<uint8_t>(addr ^ (ps * 37));
            if (sd->respondsToWrite(pw, val) != ref.respondsToWrite(pw, val)) mism++;
            uint8_t a = 0, b = 0;
            bool ra = sd->respondsToRead(p, a);
            bool rb = ref.respondsToRead(p, b);
            if (ra != rb || (ra && a != b)) mism++;
        }
    }
    CHECK(mism == 0);
}

void test_ce1600m_matches_plain_ram_card() {
    auto sd = buildCard(kCe1600mYaml, CardHost::PC1600Slot1);
    CHECK(sd != nullptr);
    if (!sd) return;
    PlainRamCard ref(0x8000);
    int mism = 0;
    for (uint32_t addr = 0x8000; addr <= 0xBFFF; addr += 3) {
        for (int pv = 0; pv < 2; ++pv) {
            PinState p;
            p.address = static_cast<uint16_t>(addr);
            p.pin[4] = true;
            p.pin[5] = (pv == 1);
            PinState pw = p;
            pw.forWrite = true;
            uint8_t val = static_cast<uint8_t>(addr * 5 + pv);
            if (sd->respondsToWrite(pw, val) != ref.respondsToWrite(pw, val)) mism++;
            uint8_t a = 0, b = 0;
            bool ra = sd->respondsToRead(p, a);
            bool rb = ref.respondsToRead(p, b);
            if (ra != rb || (ra && a != b)) mism++;
        }
    }
    CHECK(mism == 0);
}

// ── debugImageWrite(): the write side of debugImage() ──────────────────
//
// The PC-1600 fast BASIC loader scatters a tokenised program straight into
// a card's backing store, bypassing the bus. debugImageWrite() must be the
// exact inverse of debugImage() (same concatenated address space), land
// bytes that a normal bus read then sees, and reject an out-of-range range
// without a partial write.
void test_debug_image_write_roundtrip() {
    // SoftwareDefinedCard (CE-1600M: one 0x8000 unbanked region).
    auto sd = buildCard(kCe1600mYaml, CardHost::PC1600Slot1);
    CHECK(sd != nullptr);
    if (sd) {
        std::vector<uint8_t> patch(0x40);
        for (size_t i = 0; i < patch.size(); ++i) patch[i] = static_cast<uint8_t>(0xC5 + i);
        CHECK(sd->debugImageWrite(0x00C5, patch.data(), patch.size()));
        auto img = sd->debugImage();
        CHECK(img.size() == 0x8000);
        CHECK(std::equal(patch.begin(), patch.end(), img.begin() + 0x00C5));

        // Visible through an ordinary bus read (PVOUT low -> low half).
        PinState p;
        p.address = 0x80C5;
        p.pin[4] = true;
        uint8_t v = 0;
        CHECK(sd->respondsToRead(p, v) && v == 0xC5);

        // High half (card offset 0x4000+) -> PVOUT high.
        uint8_t hi = 0x99;
        CHECK(sd->debugImageWrite(0x4000, &hi, 1));
        PinState ph;
        ph.address = 0x8000;
        ph.pin[4] = true;
        ph.pin[5] = true;
        CHECK(sd->respondsToRead(ph, v) && v == 0x99);

        // Out of range: nothing written.
        auto before = sd->debugImage();
        uint8_t x = 0x11;
        CHECK(!sd->debugImageWrite(0x8000, &x, 1));
        CHECK(!sd->debugImageWrite(0x7FFF, patch.data(), 2));
        CHECK(sd->debugImage() == before);
    }

    // PlainRamCard.
    PlainRamCard ram(0x8000);
    uint8_t bytes[4] = {1, 2, 3, 4};
    CHECK(ram.debugImageWrite(0x10, bytes, 4));
    CHECK(ram.debugImage()[0x11] == 2);
    CHECK(!ram.debugImageWrite(0x7FFE, bytes, 4));
    CHECK(ram.debugImage()[0x11] == 2);  // unchanged by the failed write

    // Base ExpansionCard default: no writable backing.
    CHECK(!ram.ExpansionCard::debugImageWrite(0, bytes, 4));
}

// ── CE-1601M trigger-based banking ─────────────────────────────────────

void test_ce1601m_trigger_banking_direct() {
    auto sd = buildCard(kCe1601mYaml, CardHost::PC1600Slot2);
    CHECK(sd != nullptr);
    if (!sd) return;

    auto out28 = [&](uint8_t v) {
        PinState p;
        p.ioWrite = true;
        p.forWrite = true;
        p.address = 0x28;
        sd->respondsToWrite(p, v);
    };
    auto mem = [&](uint16_t addr, bool pvout) {
        PinState p;
        p.address = addr;
        p.pin[4] = true;
        p.pin[5] = pvout;
        return p;
    };

    out28(1);
    CHECK(sd->currentBank() == 1);
    PinState w = mem(0x8000, false);
    w.forWrite = true;
    CHECK(sd->respondsToWrite(w, 0x5A));
    uint8_t v = 0;
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0x5A);

    out28(0);
    CHECK(sd->currentBank() == 0);
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0xFF);

    PinState wh = mem(0x8000, true);
    wh.forWrite = true;
    CHECK(sd->respondsToWrite(wh, 0x24));
    CHECK(sd->respondsToRead(mem(0x8000, true), v) && v == 0x24);
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0xFF);  // low half untouched
}

// debugBankCount(): the GUI resource-inventory's "N-way Port 28H paging"
// label reads this alongside debugCurrentBank() -- confirms it reports the
// definition's bank-count (kCe1601mYaml: 2) and is stable across latch
// writes (unlike debugCurrentBank(), which tracks the live selection).
void test_debug_bank_count() {
    auto sd = buildCard(kCe1601mYaml, CardHost::PC1600Slot2);
    CHECK(sd != nullptr);
    if (!sd) return;
    CHECK(sd->debugBankCount() == 2);
    CHECK(sd->debugCurrentBank() == 0);

    PinState p;
    p.ioWrite = true;
    p.forWrite = true;
    p.address = 0x28;
    sd->respondsToWrite(p, 1);
    CHECK(sd->debugCurrentBank() == 1);
    CHECK(sd->debugBankCount() == 2);  // unchanged -- total count, not the live pick

    // An unbanked card (CE-1600M) has no bank concept: both report -1.
    auto plain = buildCard(kCe1600mYaml, CardHost::PC1600Slot1);
    CHECK(plain != nullptr);
    if (plain) {
        CHECK(plain->debugBankCount() == -1);
        CHECK(plain->debugCurrentBank() == -1);
    }
}

// ── superRAM: CE-1601M mechanism with a 4-bit (D0-D3) vertical-bank latch ─

void test_superram_16way_vertical_banking_direct() {
    // Slot 2 only -- same gate as the CE-1601M.
    CHECK(buildCard(kSuperRamYaml, CardHost::PC1600Slot1) == nullptr);
    CHECK(buildCard(kSuperRamYaml, CardHost::PC1500) == nullptr);

    auto sd = buildCard(kSuperRamYaml, CardHost::PC1600Slot2);
    CHECK(sd != nullptr);
    if (!sd) return;

    CHECK(sd->debugBankCount() == 16);  // 16 x 32 KB = 512 KB

    auto out28 = [&](uint8_t v) {
        PinState p;
        p.ioWrite = true;
        p.forWrite = true;
        p.address = 0x28;
        sd->respondsToWrite(p, v);
    };
    auto mem = [&](uint16_t addr, bool pvout) {
        PinState p;
        p.address = addr;
        p.pin[4] = true;   // RAM1 enable
        p.pin[5] = pvout;  // PVOUT half-select
        return p;
    };

    // D3 is honoured: bank 15 is reachable (a CE-1601M would mask to D0-D2
    // and land on bank 7 instead).
    out28(15);
    CHECK(sd->currentBank() == 15);
    PinState w15 = mem(0x8000, false);
    w15.forWrite = true;
    CHECK(sd->respondsToWrite(w15, 0x5A));
    uint8_t v = 0;
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0x5A);

    // A distinct high bank keeps its own bytes...
    out28(8);
    CHECK(sd->currentBank() == 8);
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0xFF);
    PinState w8 = mem(0x8000, false);
    w8.forWrite = true;
    CHECK(sd->respondsToWrite(w8, 0x24));

    // ...and PVOUT still splits each 32 KB bank into two 16 KB halves.
    PinState w8hi = mem(0x8000, true);
    w8hi.forWrite = true;
    CHECK(sd->respondsToWrite(w8hi, 0x99));
    CHECK(sd->respondsToRead(mem(0x8000, true), v) && v == 0x99);
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0x24);  // low half intact

    // Back to bank 15 -- its cell survived the excursion.
    out28(15);
    CHECK(sd->respondsToRead(mem(0x8000, false), v) && v == 0x5A);
}

// End to end through PC1600Memory: OUT (28H) forwards the whole byte
// (including D3) to the Slot 2 card's latch.
void test_superram_end_to_end_through_pc1600() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    mem.attachSlot2Card(buildCard(kSuperRamYaml, CardHost::PC1600Slot2));

    mem.writeIO(0x31, static_cast<uint8_t>(2 << 4));  // page-C bank 2 -> Slot 2, PVOUT low
    mem.writeIO(0x28, 0);
    mem.write(0x8000, 0xA1);
    CHECK(mem.read(0x8000) == 0xA1);

    mem.writeIO(0x28, 13);                            // vertical bank 13 -- needs D3+D2
    CHECK(bank.slot2VerticalBank() == 13);
    CHECK(mem.read(0x8000) == 0xFF);                  // fresh chip
    mem.write(0x8000, 0xB2);
    CHECK(mem.read(0x8000) == 0xB2);

    mem.writeIO(0x28, 0);
    CHECK(mem.read(0x8000) == 0xA1);                  // bank 0 cell untouched
}

// ── flash content / by-bank split ───────────────────────────────────────

void test_flash_bare_write_is_noop_until_unlocked() {
    auto sd = buildCard(kFlashByBankYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    auto selectBank = [&](int bank) {
        PinState p;
        p.forWrite = true;
        p.pin[18] = true;
        p.address = uint16_t(bank);
        CHECK(sd->respondsToWrite(p, 0));
    };
    auto mem = [&](uint16_t off) {
        PinState p;
        p.pin[4] = true;
        p.address = off;
        return p;
    };

    selectBank(2);  // first flash bank
    PinState w = mem(0x10);
    w.forWrite = true;
    CHECK(sd->respondsToWrite(w, 0x00));  // claimed, but not a program -- no-op
    uint8_t v = 0xFF;
    CHECK(sd->respondsToRead(mem(0x10), v) && v == 0xAA);  // still power-up fill

    // A write that doesn't match the unlock sequence's step 1 leaves the
    // array untouched too (it just arms/re-arms/idles the decoder).
    PinState w2 = mem(0x55);
    w2.forWrite = true;
    CHECK(sd->respondsToWrite(w2, 0x99));
    CHECK(sd->respondsToRead(mem(0x55), v) && v == 0xAA);
}

void test_flash_byte_program_nor_semantics_and_glitch_reset() {
    auto sd = buildCard(kFlashByBankYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    auto selectBank = [&](int bank) {
        PinState p;
        p.forWrite = true;
        p.pin[18] = true;
        p.address = uint16_t(bank);
        sd->respondsToWrite(p, 0);
    };
    auto write = [&](uint16_t off, uint8_t data) {
        PinState p;
        p.pin[4] = true;
        p.forWrite = true;
        p.address = off;
        return sd->respondsToWrite(p, data);
    };
    auto read = [&](uint16_t off) {
        uint8_t v = 0;
        PinState p;
        p.pin[4] = true;
        p.address = off;
        sd->respondsToRead(p, v);
        return v;
    };

    selectBank(3);
    CHECK(write(0x55, 0xAA));
    CHECK(write(0x2A, 0x55));
    CHECK(write(0x55, 0xA0));  // byte-program-command -> ProgramArmed
    CHECK(write(0x10, 0x0F));  // program 0x0F at offset 0x10
    CHECK(read(0x10) == (0xAA & 0x0F));  // NOR: powered-up 0xAA & 0x0F

    // A second program can only clear further bits, never set any back.
    CHECK(write(0x55, 0xAA));
    CHECK(write(0x2A, 0x55));
    CHECK(write(0x55, 0xA0));
    CHECK(write(0x10, 0xFF));         // would try to set bits -- NOR can't
    CHECK(read(0x10) == (0xAA & 0x0F));  // unchanged

    // Glitch recovery: an unexpected step-2 write resets to Idle, so a
    // plain write afterwards is a no-op, not a stray program.
    CHECK(write(0x55, 0xAA));
    CHECK(write(0x99, 0x99));  // wrong step 2 -- decoder falls back to Idle
    CHECK(write(0x20, 0x00));  // ordinary write, no unlock armed -- no-op
    CHECK(read(0x20) == 0xAA);
}

void test_flash_sector_and_chip_erase() {
    auto sd = buildCard(kFlashByBankYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    auto selectBank = [&](int bank) {
        PinState p;
        p.forWrite = true;
        p.pin[18] = true;
        p.address = uint16_t(bank);
        sd->respondsToWrite(p, 0);
    };
    auto write = [&](uint16_t off, uint8_t data) {
        PinState p;
        p.pin[4] = true;
        p.forWrite = true;
        p.address = off;
        return sd->respondsToWrite(p, data);
    };
    auto read = [&](uint16_t off) {
        uint8_t v = 0;
        PinState p;
        p.pin[4] = true;
        p.address = off;
        sd->respondsToRead(p, v);
        return v;
    };
    auto programByte = [&](uint16_t off, uint8_t data) {
        write(0x55, 0xAA);
        write(0x2A, 0x55);
        write(0x55, 0xA0);
        write(off, data);
    };
    auto directWrite = [&](uint16_t off, uint8_t data) {
        PinState p;
        p.pin[4] = true;
        p.forWrite = true;
        p.direct = true;
        p.address = off;
        return sd->respondsToWrite(p, data);
    };

    selectBank(2);
    directWrite(0x00, 0x11);  // poke bypasses the decoder entirely
    CHECK(read(0x00) == 0x11);

    programByte(0x00, 0x00);  // clear the whole first byte
    CHECK(read(0x00) == 0x00);

    // Sector erase (sector-size 0x40) clears only the current bank's sector.
    write(0x55, 0xAA);
    write(0x2A, 0x55);
    write(0x55, 0x80);  // erase-setup
    write(0x55, 0xAA);
    write(0x2A, 0x55);
    write(0x30, 0x30);  // sector-erase-command, sector containing 0x30
    CHECK(read(0x00) == 0xFF);

    // Chip erase clears every flash bank in the region, not just the
    // current one (spec §5: "clears the whole Flash content-range").
    selectBank(2);
    programByte(0x00, 0x00);
    selectBank(3);
    programByte(0x00, 0x00);
    CHECK(read(0x00) == 0x00);
    selectBank(2);
    write(0x55, 0xAA);
    write(0x2A, 0x55);
    write(0x55, 0x80);
    write(0x55, 0xAA);
    write(0x2A, 0x55);
    write(0x55, 0x10);  // chip-erase-command
    CHECK(read(0x00) == 0xFF);
    selectBank(3);
    CHECK(read(0x00) == 0xFF);  // bank 3 cleared too
}

void test_flash_reset_command_exception_during_program() {
    auto sd = buildCard(kFlashByBankYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    PinState sel;
    sel.forWrite = true;
    sel.pin[18] = true;
    sel.address = 3;
    sd->respondsToWrite(sel, 0);

    auto write = [&](uint16_t off, uint8_t data) {
        PinState p;
        p.pin[4] = true;
        p.forWrite = true;
        p.address = off;
        return sd->respondsToWrite(p, data);
    };
    auto read = [&](uint16_t off) {
        uint8_t v = 0;
        PinState p;
        p.pin[4] = true;
        p.address = off;
        sd->respondsToRead(p, v);
        return v;
    };

    write(0x55, 0xAA);
    write(0x2A, 0x55);
    write(0x55, 0xA0);      // ProgramArmed
    write(0x10, 0xF0);      // 0xF0 as *data* here must be programmed, not treated as a reset
    CHECK(read(0x10) == (0xAA & 0xF0));

    // Outside ProgramArmed, 0xF0 does reset the decoder -- a stray program
    // write afterwards is a no-op.
    write(0x55, 0xAA);
    write(0xF0, 0xF0);  // reset while awaiting a command
    write(0x20, 0x00);
    CHECK(read(0x20) == 0xAA);
}

void test_flash_bank_latch_does_not_perturb_decoder() {
    auto sd = buildCard(kFlashByBankYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    auto selectBank = [&](int bank) {
        PinState p;
        p.forWrite = true;
        p.pin[18] = true;
        p.address = uint16_t(bank);
        sd->respondsToWrite(p, 0);
    };
    auto write = [&](uint16_t off, uint8_t data) {
        PinState p;
        p.pin[4] = true;
        p.forWrite = true;
        p.address = off;
        return sd->respondsToWrite(p, data);
    };
    auto read = [&](uint16_t off) {
        uint8_t v = 0;
        PinState p;
        p.pin[4] = true;
        p.address = off;
        sd->respondsToRead(p, v);
        return v;
    };

    selectBank(2);
    write(0x55, 0xAA);
    selectBank(2);  // re-select the same bank mid-unlock -- must not reset the decoder
    write(0x2A, 0x55);
    write(0x55, 0xA0);
    write(0x10, 0x00);
    CHECK(read(0x10) == 0x00);  // the program went through -- decoder state survived the strobe
}

void test_flash_regular_banks_are_ordinary_ram() {
    auto sd = buildCard(kFlashByBankYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    PinState sel;
    sel.forWrite = true;
    sel.pin[18] = true;
    sel.address = 0;
    sd->respondsToWrite(sel, 0);

    PinState w;
    w.pin[4] = true;
    w.forWrite = true;
    w.address = 0x10;
    CHECK(sd->respondsToWrite(w, 0x5A));  // no unlock needed -- Regular content
    uint8_t v = 0;
    PinState r;
    r.pin[4] = true;
    r.address = 0x10;
    CHECK(sd->respondsToRead(r, v) && v == 0x5A);
}

// ── initial-content ──────────────────────────────────────────────────

void test_initial_content_addressed_hex_and_hex() {
    auto sd = buildCard(kInitialContentYaml, CardHost::PC1500);
    CHECK(sd != nullptr);
    if (!sd) return;

    auto selectBank = [&](int bank) {
        PinState p;
        p.forWrite = true;
        p.pin[18] = true;
        p.address = uint16_t(bank);
        sd->respondsToWrite(p, 0);
    };
    auto read = [&](uint16_t off) {
        uint8_t v = 0;
        PinState p;
        p.pin[4] = true;
        p.address = off;
        sd->respondsToRead(p, v);
        return v;
    };

    // Bank 0: addressed-hex -- $0000-$000F run of 00, $0010-$001F literal,
    // $0020-$00FF run of FF.
    selectBank(0);
    CHECK(read(0x0000) == 0x00);
    CHECK(read(0x000F) == 0x00);
    CHECK(read(0x0010) == 0x11);
    CHECK(read(0x001B) == 0xCC);
    CHECK(read(0x001F) == 0x00);
    CHECK(read(0x0020) == 0xFF);
    CHECK(read(0x00FF) == 0xFF);

    // Bank 2: FLASH, power-up-fill 0xAA, plain 'hex' block only covers
    // offset 0x10-0x12 -- everything else keeps the flash fill.
    selectBank(2);
    CHECK(read(0x0000) == 0xAA);
    CHECK(read(0x000F) == 0xAA);
    CHECK(read(0x0010) == 0xAA);  // 0xAA byte, indistinguishable from fill here
    CHECK(read(0x0011) == 0xBB);
    CHECK(read(0x0012) == 0xCC);
    CHECK(read(0x0013) == 0xAA);

    // Bank 3: FLASH, no initial-content block at all -- untouched fill.
    selectBank(3);
    CHECK(read(0x0000) == 0xAA);
}

void test_reject_initial_content_addressed_hex_gap_or_overlap() {
    std::string err;
    // Gap: $0000-$000F run, then $0020 directly (skips $0010-$001F).
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0], source-domain: address }\n"
                      "        bank-count: 2\n        bank-size: 0x20\n"
                      "        bank-window: { chip-select: Y0, span: 0x20 }\n"
                      "    content: { by-bank: [ { banks: \"0-1\", kind: regular } ] }\n"
                      "    initial-content:\n"
                      "      blocks:\n"
                      "        - bank: 0\n          offset: 0\n          encoding: addressed-hex\n"
                      "          bytes: |\n            $0000: 00...\n            $0010: 00...\n",
                  &err));

    // Overlap between two blocks on the same bank.
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0], source-domain: address }\n"
                      "        bank-count: 2\n        bank-size: 0x20\n"
                      "        bank-window: { chip-select: Y0, span: 0x20 }\n"
                      "    content: { by-bank: [ { banks: \"0-1\", kind: regular } ] }\n"
                      "    initial-content:\n"
                      "      blocks:\n"
                      "        - bank: 0\n          offset: 0\n          encoding: hex\n"
                      "          bytes: |\n            00 11 22\n"
                      "        - bank: 0\n          offset: 2\n          encoding: hex\n"
                      "          bytes: |\n            33 44\n",
                  &err));
}

void test_reject_initial_content_missing_bank_on_banked_region() {
    std::string err;
    CHECK(rejects(std::string(kMinPrefix) +
                      "  - name: r\n"
                      "    addressing: { chip-select: Y0 }\n"
                      "    banking:\n"
                      "        latch: { type: trigger-based, trigger: { pin: 18 },\n"
                      "                 sampled-lines: [A0], source-domain: address }\n"
                      "        bank-count: 2\n        bank-size: 0x20\n"
                      "        bank-window: { chip-select: Y0, span: 0x20 }\n"
                      "    content: { by-bank: [ { banks: \"0-1\", kind: regular } ] }\n"
                      "    initial-content:\n"
                      "      blocks:\n"
                      "        - offset: 0\n          encoding: hex\n          bytes: |\n            00\n",
                  &err));
}

// ── end to end ─────────────────────────────────────────────────────────

std::string makeScratchCardDir();  // defined below, in the catalogue section

void test_preset_parses_modulespec() {
    // `modulespecfile:` -- a path, resolved relative to the preset dir.
    PresetFile preset;
    std::string err;
    CHECK(parsePresetString("model: PC-1500\n"
                            "memory-expansion:\n"
                            "  - modulespecfile: /abs/path/foo.card.yaml\n",
                            "/tmp/memory_card_tests_scratch.pc1500", &preset, &err));
    CHECK(preset.memoryExpansionModule.empty());
    CHECK(preset.memoryExpansionModuleSpecFile == "/abs/path/foo.card.yaml");
    CHECK(preset.memoryExpansionModuleSpecName.empty());

    PresetFile p2;
    CHECK(parsePresetString("model: PC-1600\n"
                            "memory-expansion-2:\n"
                            "  - modulespecfile: cards/x.card.yaml\n",
                            "/tmp/memory_card_tests_scratch.pc1600", &p2, &err));
    CHECK(p2.slot2ModuleSpecFile == "/tmp/cards/x.card.yaml");  // relative to the scratch dir
    CHECK(p2.slot2Module.empty());

    // `modulespec:` -- a bundled module-name, stored verbatim (unresolved).
    PresetFile p3;
    CHECK(parsePresetString("model: PC-1600\n"
                            "memory-expansion-1:\n"
                            "  - modulespec: CE-155\n",
                            "/tmp/memory_card_tests_scratch3.pc1600", &p3, &err));
    CHECK(p3.slot1ModuleSpecName == "CE-155");
    CHECK(p3.slot1ModuleSpecFile.empty());
    CHECK(p3.slot1Module.empty());
}

void test_preset_rejects_module_and_modulespec_second_item() {
    PresetFile preset;
    std::string err;
    CHECK(!parsePresetString("model: PC-1600\n"
                             "memory-expansion-1:\n"
                             "  - module: ce155\n"
                             "  - modulespec: CE-155\n",
                             "/tmp/memory_card_tests_scratch2.pc1600", &preset, &err));
}

void test_ce1601m_end_to_end_through_pc1600() {
    // Drive a CE-1601M through a full preset load two ways -- a
    // `modulespecfile:` path and a `modulespec:` module-name resolved from
    // a scratch module directory -- both exercising the OUT (28H) -> Slot 2
    // route. `run` asserts the vertical-bank behaviour for a loaded preset.
    auto run = [&](const PresetFile& preset, const std::string& moduleDir) {
        PC1600Machine m;
        PC1600PresetLoadResult r = applyPC1600Preset(m, preset, {}, ".", moduleDir);
        CHECK(r.ok);
        CHECK(m.slot2Attached());
        // The loader reports the attached module's module-name for the GUI
        // button, whichever spec form named it; slot 1 stays empty here.
        CHECK(r.slot1ModuleLabel.empty());
        CHECK(r.slot2ModuleLabel == "CE-1601M");
        m.memory().writeIO(0x31, static_cast<uint8_t>(2 << 4));  // page-C bank 2 -> Slot 2
        m.memory().writeIO(0x28, 0);
        m.memory().write(0x8000, 0xA1);
        CHECK(m.memory().read(0x8000) == 0xA1);
        m.memory().writeIO(0x28, 1);  // vertical bank 1 -- a different 32 KB chip
        CHECK(m.memory().read(0x8000) == 0xFF);
        m.memory().write(0x8000, 0xB2);
        CHECK(m.memory().read(0x8000) == 0xB2);
        m.memory().writeIO(0x28, 0);
        CHECK(m.memory().read(0x8000) == 0xA1);
    };

    const std::string cardPath = "/tmp/memory_card_tests_ce1601m.card.yaml";
    { std::ofstream f(cardPath); f << kCe1601mYaml; }
    PresetFile byFile;
    std::string err;
    CHECK(parsePresetString(std::string("model: PC-1600\n"
                                        "memory-expansion-2:\n"
                                        "  - modulespecfile: ") +
                                cardPath + "\n",
                            "/tmp/memory_card_tests_e2e.pc1600", &byFile, &err));
    CHECK(byFile.slot2ModuleSpecFile == cardPath);
    run(byFile, ".");

    const std::string moduleDir = makeScratchCardDir();  // writes ce1601m.card.yaml (module-name CE-1601M)
    PresetFile byName;
    CHECK(parsePresetString("model: PC-1600\n"
                            "memory-expansion-2:\n"
                            "  - modulespec: CE-1601M\n",
                            "/tmp/memory_card_tests_e2e_name.pc1600", &byName, &err));
    CHECK(byName.slot2ModuleSpecName == "CE-1601M");
    run(byName, moduleDir);
}

// The SLOT2MAP gate-array remap (Port 3CH b5:b4): the firmware can make the
// Slot 2 RAM chip-select assert for a bank-1 access outside the ordinary
// page-C window -- the "(S2:) at Bank 1" path the RAM-disk file system uses.
// SLOT2MAP ROM routine: PC1600-P0-B0.bin 0A6DH.
void test_ce1601m_slot2map_remap() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    mem.attachSlot2Card(buildCard(kCe1601mYaml, CardHost::PC1600Slot2));

    // Reference bytes, written through the ordinary (mode 0) route:
    // page-C bank 2 = Slot 2 PVOUT-low, bank 3 = PVOUT-high, vertical bank 0.
    mem.writeIO(0x28, 0);
    mem.writeIO(0x3C, 0x00);              // SLOT2MAP A=0 (default)
    mem.writeIO(0x31, 2 << 4);            // page-C bank 2
    mem.write(0x8000, 0xA1);
    mem.write(0xBFFF, 0xA2);              // top of the low 16K half
    mem.writeIO(0x31, 3 << 4);            // page-C bank 3
    mem.write(0x8000, 0xB1);             // low edge of the high 16K half

    // Mode 1 (A=1): Slot 2's low 16K half also answers at page-C bank 1,
    // which the ordinary decode would give to Slot 1.
    mem.writeIO(0x3C, 0x20);
    CHECK(bank.slot2MapMode() == 1);
    mem.writeIO(0x31, 1 << 4);            // page-C bank 1
    CHECK(mem.read(0x8000) == 0xA1);      // same module cell as mode-0 page-C bank 2
    CHECK(mem.read(0xBFFF) == 0xA2);
    mem.write(0x9000, 0x77);              // a fresh write through the remap
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x31, 2 << 4);
    CHECK(mem.read(0x9000) == 0x77);      // landed in the module, visible via the normal route

    // Mode 2 (A=2): Slot 2's low 16K half at page-B bank 1 (4000-7FFF),
    // its high 16K half at page-A bank 1 (0000-3FFF).
    mem.writeIO(0x3C, 0x10);
    CHECK(bank.slot2MapMode() == 2);
    mem.writeIO(0x31, 1 << 1);            // page-B bank 1 (Port 31H b3:b1 = 001)
    CHECK(mem.read(0x4000) == 0xA1);      // low half, offset 0 -- the same cell again
    CHECK(mem.read(0x7FFF) == 0xA2);
    mem.writeIO(0x31, 0x01);              // page-A bank 1 (Port 31H b0 = 1)
    CHECK(mem.read(0x0000) == 0xB1);      // the HIGH half this time
    mem.write(0x0001, 0xC3);
    mem.writeIO(0x3C, 0x00);              // read the high-half write back the ordinary way
    mem.writeIO(0x31, 3 << 4);
    CHECK(mem.read(0x8001) == 0xC3);

    // A CE-1601M-class card's OUT (28H) vertical-bank latch still applies
    // under a remap -- it's the card's own state, not derived from the
    // remapped address.
    mem.writeIO(0x3C, 0x20);             // mode 1
    mem.writeIO(0x31, 1 << 4);
    mem.writeIO(0x28, 1);                 // vertical bank 1 -- the other 32K chip
    CHECK(mem.read(0x8000) == 0xFF);      // blank
    mem.write(0x8000, 0xEE);
    mem.writeIO(0x28, 0);
    CHECK(mem.read(0x8000) == 0xA1);      // vbank 0 cell intact
    mem.writeIO(0x28, 1);
    CHECK(mem.read(0x8000) == 0xEE);

    // Mode 0: the remap is inert -- page-C bank 1 is Slot 1 (empty here),
    // and page-A/B bank 1 don't reach Slot 2 at all.
    mem.writeIO(0x28, 0);
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x31, 1 << 4);            // page-C bank 1 -> Slot 1
    CHECK(mem.read(0x8000) == 0xFF);
    mem.writeIO(0x31, 1 << 1);            // page-B bank 1
    CHECK(mem.read(0x4000) == 0xFF);
    mem.writeIO(0x31, 0x01);              // page-A bank 1
    CHECK(mem.read(0x0000) == 0xFF);      // no ROM loaded -> open bus, not Slot 2
}

// SLOT2MAP with an empty Slot 2: the redirect must be inert, never divert a
// page-A/B ROM fetch into nothing.
void test_slot2map_remap_no_card_is_inert() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> lo(0x4000, 0x11), hi(0x4000, 0x22);
    CHECK(mem.loadBank0(lo.data(), lo.size(), hi.data(), hi.size()));

    mem.writeIO(0x3C, 0x10);              // SLOT2MAP A=2, but no Slot 2 card
    mem.writeIO(0x31, 0x01);              // page-A bank 1
    CHECK(mem.read(0x0000) == 0x11);      // still the page-A ROM
    mem.write(0x0000, 0x99);
    CHECK(mem.read(0x0000) == 0x11);      // ROM ignores the write
    CHECK(!mem.isWritable(0x0000));
}

// The SLOT1MAP gate-array remap (Port 3CH b2): when armed, Slot 1's high
// 16K half (beta, normally page-C bank 1) also answers at page-B bank 1
// (4000-7FFF) -- a mirror, not a move (TRM 0196H entry + diagram).
void test_slot1map_remap() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(mem.attachSlot1(0x8000)); // 32K unbanked: bank 0 = alpha, bank 1 = beta

    // Reference bytes via the ordinary (SLOT1MAP=0) route.
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x31, 0 << 4);            // page-C bank 0 (alpha)
    mem.write(0x8000, 0xA1);
    mem.writeIO(0x31, 1 << 4);            // page-C bank 1 (beta)
    mem.write(0x8000, 0xB1);
    mem.write(0xBFFF, 0xB2);

    // SLOT1MAP=1: beta also answers at page-B bank 1.
    mem.writeIO(0x3C, 0x04);
    CHECK(bank.slot1MapActive());
    mem.writeIO(0x31, 1 << 1);            // page-B bank 1 (Port 31H b3:b1 = 001)
    CHECK(mem.read(0x4000) == 0xB1);      // same module cell as beta's page-C bank 1
    CHECK(mem.read(0x7FFF) == 0xB2);
    mem.write(0x4001, 0xC3);              // a fresh write through the mirror
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x31, 1 << 4);            // read it back through the ordinary route
    CHECK(mem.read(0x8001) == 0xC3);

    // alpha (bank 0) never mirrors, and the mirror is gated on page-B
    // actually selecting bank 1 -- neither reaches Slot 1 here.
    mem.writeIO(0x3C, 0x04);
    mem.writeIO(0x31, 0x00);              // page-B bank 0, not bank 1
    CHECK(mem.read(0x4000) == 0xFF);      // open bus: SLOT1MAP armed but ungated
}

// SLOT1MAP with an empty Slot 1: the redirect must be inert.
void test_slot1map_remap_no_card_is_inert() {
    PC1600Bank bank;
    PC1600Memory mem(bank);

    mem.writeIO(0x3C, 0x04);              // SLOT1MAP=1, but no Slot 1 card
    mem.writeIO(0x31, 1 << 1);            // page-B bank 1
    CHECK(mem.read(0x4000) == 0xFF);      // open bus, not diverted into nothing
    mem.write(0x4000, 0x99);
    CHECK(!mem.isWritable(0x4000));
}

// SLOT1MAP and SLOT2MAP mode 2 both mirror onto page-B bank 1 -- the one
// window they can collide on (two different physical cards). Arbitrated
// "last call wins": whichever field's value most recently changed owns the
// window; an exact tie (both changed in the same Port 3CH write) favors
// SLOT2MAP. See PC1600Bank::slot1MapWinsTie().
void test_slot1map_slot2map_collision_last_call_wins() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(mem.attachSlot1(0x8000));
    CHECK(mem.attachSlot2(0x8000));

    // Reference bytes: Slot 1's beta half, Slot 2's low half.
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x31, 1 << 4);            // page-C bank 1 -> Slot 1 beta
    mem.write(0x8000, 0xB1);
    mem.writeIO(0x31, 2 << 4);            // page-C bank 2 -> Slot 2 low half
    mem.write(0x8000, 0xA1);

    mem.writeIO(0x31, 1 << 1);            // page-B bank 1, for the rest of this test

    // Only SLOT2MAP armed: Slot 2 answers.
    mem.writeIO(0x3C, 0x10);              // SLOT2MAP A=2
    CHECK(mem.read(0x4000) == 0xA1);

    // Exact tie -- both fields go from inactive to active in the very same
    // Port 3CH write (neither was already at this value beforehand):
    // SLOT2MAP wins (the documented fallback).
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x3C, 0x14);              // SLOT1MAP=1 + SLOT2MAP A=2, one write
    CHECK(mem.read(0x4000) == 0xA1);

    // SLOT2MAP armed first, SLOT1MAP added in a later write (SLOT2MAP's own
    // field unchanged by it): SLOT1MAP is the more recent change and wins.
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x3C, 0x10);              // SLOT2MAP A=2 alone
    mem.writeIO(0x3C, 0x14);              // SLOT1MAP=1 added
    CHECK(mem.read(0x4000) == 0xB1);

    // SLOT1MAP armed first, SLOT2MAP added later (SLOT1MAP's own bit
    // unchanged by it): SLOT2MAP is now the more recent change and wins.
    mem.writeIO(0x3C, 0x00);
    mem.writeIO(0x3C, 0x04);              // SLOT1MAP=1 alone
    mem.writeIO(0x3C, 0x14);              // SLOT2MAP A=2 added
    CHECK(mem.read(0x4000) == 0xA1);
}

// ── catalogue: scan a directory + resolve modulespec by module-name ────

// A throwaway directory of .card.yaml files for the catalogue tests.
// Plain POSIX mkdir + `/tmp` literals, matching the other file-based tests
// in this suite (which hardcode `/tmp/...` paths) rather than pulling
// <filesystem> into the test.
std::string makeScratchCardDir() {
    const std::string dir = "/tmp/memory_card_catalog_tests";
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    auto put = [&](const char* file, const char* body) {
        std::ofstream(dir + "/" + file) << body;
    };
    put("ce155.card.yaml", kCe155Yaml);
    put("ce1600m.card.yaml", kCe1600mYaml);
    put("ce1601m.card.yaml", kCe1601mYaml);
    // Not a card file -- must be ignored by the ".card.yaml" tail match.
    put("notes.yaml", "hello: world\n");
    // Malformed card -- must be skipped, not abort the scan.
    put("broken.card.yaml", "module-name: BROKEN\nbogus-key: 1\n");
    return dir;
}

void test_catalog_scan_lists_and_sorts() {
    const std::string dir = makeScratchCardDir();
    std::string err;
    auto entries = scanMemoryCardDirectory(dir, &err);
    CHECK(entries.size() == 3);  // ce155 / ce1600m / ce1601m; notes.yaml + broken skipped
    if (entries.size() == 3) {
        CHECK(entries[0].moduleName == "CE-155");     // sorted by module-name
        CHECK(entries[1].moduleName == "CE-1600M");
        CHECK(entries[2].moduleName == "CE-1601M");
        CHECK(entries[0].compatibleWith(CardHost::PC1500));
        CHECK(!entries[0].compatibleWith(CardHost::PC1600Slot2));
        CHECK(entries[2].compatibleWith(CardHost::PC1600Slot2));
    }
    CHECK(err.find("broken.card.yaml") != std::string::npos);  // reported, not fatal
}

void test_catalog_scan_missing_dir() {
    std::string err;
    auto entries = scanMemoryCardDirectory("/no/such/module/dir", &err);
    CHECK(entries.empty());
    CHECK(!err.empty());
}

void test_resolve_modulespec_by_name() {
    const std::string dir = makeScratchCardDir();
    std::string path, err;
    CHECK(resolveModuleSpecByName(dir, "CE-1601M", &path, &err));
    CHECK(path.find("ce1601m.card.yaml") != std::string::npos);

    // Round-trips through makeSoftwareDefinedCard for the right host.
    err.clear();
    CHECK(makeSoftwareDefinedCard(path, CardHost::PC1600Slot2, &err) != nullptr);

    // Miss.
    path.clear();
    err.clear();
    CHECK(!resolveModuleSpecByName(dir, "CE-9999", &path, &err));
    CHECK(err.find("no module named") != std::string::npos);
}

void test_resolve_modulespec_ambiguous() {
    const std::string dir = makeScratchCardDir();
    // Second file, same module-name as ce155.card.yaml.
    { std::ofstream(dir + "/ce155-dup.card.yaml") << kCe155Yaml; }
    std::string path, err;
    CHECK(!resolveModuleSpecByName(dir, "CE-155", &path, &err));
    CHECK(err.find("more than one") != std::string::npos);
}

// The ordered multi-directory overload: bundled catalogue first, then a
// fallback directory (the GUI's iCloud `BatteryCards/` folder) that holds a
// user's saved battery-card instance named `CE-1601M - Programs.card.yaml`
// (module-name with spaces, the exact string `- modulespec:` would carry).
void test_resolve_modulespec_multi_dir() {
    const std::string bundled = makeScratchCardDir();  // ce155 / ce1600m / ce1601m
    const std::string instances = "/tmp/memory_card_catalog_tests_instances";
    std::system(("rm -rf " + instances + " && mkdir -p " + instances).c_str());
    // A named instance whose filename has blanks -- resolution is by the
    // `module-name:` field, so the filename shape is irrelevant.
    {
        const std::string body = kCe1601mYaml;
        std::ofstream(instances + "/CE-1601M - Programs.card.yaml")
            << "module-name: \"CE-1601M - Programs\"\n"
            << body.substr(body.find('\n') + 1);
    }

    std::string path, err;
    // Found only in the fallback directory.
    CHECK(resolveModuleSpecByName(std::vector<std::string>{bundled, instances},
                                  "CE-1601M - Programs", &path, &err));
    CHECK(path.find("CE-1601M - Programs.card.yaml") != std::string::npos);

    // A bundled name still resolves with the fallback appended.
    path.clear(); err.clear();
    CHECK(resolveModuleSpecByName(std::vector<std::string>{bundled, instances},
                                  "CE-1601M", &path, &err));
    CHECK(path.find("ce1601m.card.yaml") != std::string::npos);

    // A non-existent fallback directory is skipped, not an error.
    path.clear(); err.clear();
    CHECK(resolveModuleSpecByName(std::vector<std::string>{bundled, "/no/such/dir"},
                                  "CE-1601M", &path, &err));

    // Missing everywhere -> one aggregated "no module named" naming the
    // directories actually searched.
    path.clear(); err.clear();
    CHECK(!resolveModuleSpecByName(std::vector<std::string>{bundled, instances},
                                   "CE-9999", &path, &err));
    CHECK(err.find("no module named") != std::string::npos);
    CHECK(err.find(instances) != std::string::npos);

    // Bundled wins when both directories declare the same name (first hit).
    {
        std::ofstream(instances + "/ce155-shadow.card.yaml") << kCe155Yaml;
    }
    path.clear(); err.clear();
    CHECK(resolveModuleSpecByName(std::vector<std::string>{bundled, instances},
                                  "CE-155", &path, &err));
    CHECK(path.find(bundled) != std::string::npos);
}

}  // namespace

int run_memory_card_tests() {
    test_yaml_block_map_and_scalars();
    test_yaml_hex_ints();
    test_yaml_block_and_flow_sequences();
    test_yaml_flow_map_nested();
    test_yaml_seq_of_maps_with_continuation_keys();
    test_yaml_comments_and_blank_lines();
    test_yaml_block_scalar();
    test_yaml_rejects_tabs_and_bad_indent();

    test_inline_definitions_parse();
    test_ce155_definition_shape();
    test_reject_unknown_top_key();
    test_reject_terminology_not_in_hosts();
    test_reject_tiling_gap_and_overlap();
    test_reject_rom_flash_bybank_linebased();
    test_reject_bybank_shapes();
    test_reject_unbanked_without_capacity();

    test_compat_gate_inline();
    test_compat_gate_via_file_if_present();
    test_ce155_matches_hardcoded_card();
    test_ce1600m_matches_plain_ram_card();
    test_debug_image_write_roundtrip();
    test_ce1601m_trigger_banking_direct();
    test_debug_bank_count();
    test_superram_16way_vertical_banking_direct();
    test_superram_end_to_end_through_pc1600();

    test_flash_bare_write_is_noop_until_unlocked();
    test_flash_byte_program_nor_semantics_and_glitch_reset();
    test_flash_sector_and_chip_erase();
    test_flash_reset_command_exception_during_program();
    test_flash_bank_latch_does_not_perturb_decoder();
    test_flash_regular_banks_are_ordinary_ram();
    test_ce1638_card_yaml_if_present();
    test_ce163f_card_yaml_if_present();
    test_superram_card_yaml_if_present();

    test_initial_content_addressed_hex_and_hex();
    test_reject_initial_content_addressed_hex_gap_or_overlap();
    test_reject_initial_content_missing_bank_on_banked_region();

    test_preset_parses_modulespec();
    test_preset_rejects_module_and_modulespec_second_item();
    test_ce1601m_end_to_end_through_pc1600();
    test_ce1601m_slot2map_remap();
    test_slot2map_remap_no_card_is_inert();
    test_slot1map_remap();
    test_slot1map_remap_no_card_is_inert();
    test_slot1map_slot2map_collision_last_call_wins();

    test_catalog_scan_lists_and_sorts();
    test_catalog_scan_missing_dir();
    test_resolve_modulespec_by_name();
    test_resolve_modulespec_ambiguous();
    test_resolve_modulespec_multi_dir();

    std::printf("memory_card_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
