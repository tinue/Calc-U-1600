#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Yaml.hpp"

// ── Parsed + validated memory-card definition ─────────────────────────
//
// The runtime model of a docs/Memory-Card-Definition-Format.md file, after
// YAML parsing, schema validation, and terminology→pin resolution. Built
// by parseMemoryCardDefinition(); consumed by SoftwareDefinedCard.
//
// Scope: Regular, Flash and ROM content, single-kind or `by-bank`-split;
// Unbanked or trigger-based Banked. `line-based` latches are parsed far
// enough to report a clear "not supported in v1" error.

enum class CardHost { PC1500, PC1500A, PC1600Slot1, PC1600Slot2 };

bool cardHostFromToken(const std::string& tok, CardHost* out);
const char* cardHostToken(CardHost h);

// Signal name → physical 40-pin edge-connector contact (1..40) for a
// terminology's vocabulary, mirroring Core/Connector/ExpansionCard.hpp's
// pin dictionary and Format §1. -1 if `name` is not a signal in that
// vocabulary. `A0..A15` / `D0..D7` are handled by the caller, not here.
int resolveSignalPin(CardHost terminology, const std::string& name);

struct EnableGroup {
    std::vector<int> requireHigh;  // physical pins that must be asserted
    std::vector<int> requireLow;   // physical pins that must be de-asserted
    struct AddrBit {
        int bit;
        int level;
    };
    std::vector<AddrBit> addrBits;
    bool hasMemoryRange = false;
    uint32_t rangeFrom = 0;
    uint32_t rangeTo = 0;  // inclusive
    uint32_t span = 0;     // power of two; 0 only for a pure gate (banked region addressing)
    uint32_t mapsTo = 0;   // byte offset into the backing store / current bank slice
};

struct Addressing {
    std::vector<EnableGroup> groups;  // OR'd; first match wins
};

// Rom: read-only forever (a mask ROM -- not even a host poke writes it);
// every byte comes from `initial-content`, which must cover the whole range.
enum class ContentKind { Regular, Flash, Rom };

// A JEDEC-style unlock/program/erase protocol for a Flash content range --
// "chip/firmware properties, not something the loader infers" (spec §5), so
// every field is required at parse time, no defaults. Modeled on the
// CE-163F's flash chip (Qt6/resources/cards/ce163f.card.yaml and
// SoftwareDefinedCard::flashWrite() document every field).
struct FlashProtocol {
    struct AddrData {
        uint32_t address = 0;
        uint8_t data = 0;
    };
    std::vector<AddrData> unlockSequence;  // exactly 2 steps
    uint8_t byteProgramCommand = 0;
    uint8_t eraseSetupCommand = 0;
    uint8_t sectorEraseCommand = 0;
    uint8_t chipEraseCommand = 0;
    uint8_t resetCommand = 0;
    uint32_t sectorSize = 0;

    // How many low address bits the command decoder actually looks at
    // before comparing against an unlock-sequence/command address --
    // CE-163F's real flash chip decodes only its low 11 bits (recovered
    // from firmware disassembly: the firmware's unlock addresses are
    // &1555/&2AAA, not the datasheet's &5555/&2AAA, because &1555 & 0x7FF
    // == &555). Default 0xFFFFFFFF: compare the full address, no masking.
    uint32_t commandAddressMask = 0xFFFFFFFFu;
};

struct RegionContent {
    ContentKind kind = ContentKind::Regular;
    bool writable = true;
    // Regular RAM powers up 0x00 (CMOS RAM after a power loss); the flash
    // parser sets 0xFF (erased) unless the file says otherwise.
    uint8_t powerUpFill = 0x00;
    bool hasWriteProtect = false;
    bool writeProtectDefaultProtected = false;
    bool writeProtectPersisted = false;
    FlashProtocol flash;  // meaningful only when kind == ContentKind::Flash
};

enum class TriggerKind { Pin, IoPort };

struct Banking {
    TriggerKind triggerKind = TriggerKind::Pin;
    int triggerPin = 0;         // TriggerKind::Pin
    uint8_t triggerPort = 0;    // TriggerKind::IoPort
    bool sampleData = false;    // source-domain: data (else: address)
    std::vector<int> sampledBits;  // bit indices; sampledBits[0] is the LSB of the bank number
    uint32_t bankCount = 0;
    uint32_t bankSize = 0;
    Addressing bankWindow;
};

// One `by-bank:` entry (spec §5's split-content note): the content for
// banks [loBank, hiBank] (inclusive) of a banked region.
struct BankContentRange {
    uint32_t loBank = 0;
    uint32_t hiBank = 0;
    RegionContent content;
};

struct Region {
    std::string name;
    Addressing addressing;  // region gate; also the slice map when unbanked
    RegionContent content;  // used directly unless contentByBank is non-empty
    std::vector<BankContentRange> contentByBank;  // by-bank split (banked only)
    bool banked = false;
    Banking banking;
    uint32_t capacity = 0;  // unbanked only (banked = bankCount * bankSize)

    // `initial-content` (spec §5a / Format.md §6), already resolved to a
    // full bank-size (or `capacity`, unbanked) byte buffer per referenced
    // bank -- gaps a block didn't cover are pre-filled with that bank's
    // `contentForBank(bank).powerUpFill`. Unbanked regions use key 0.
    // Empty when the region has no `initial-content` at all.
    std::unordered_map<uint32_t, std::vector<uint8_t>> initialContentByBank;

    // The content that applies to `bank` (0 for an unbanked region's one
    // and only "bank") -- `content` itself unless a `by-bank:` split names
    // a range covering it.
    const RegionContent& contentForBank(uint32_t bank) const {
        for (const auto& r : contentByBank)
            if (bank >= r.loBank && bank <= r.hiBank) return r.content;
        return content;
    }
};

struct MemoryCardDefinition {
    std::string moduleName;
    std::vector<CardHost> compatibleHosts;
    CardHost terminology = CardHost::PC1500;
    std::vector<Region> regions;

    // Marks the card as battery-backed real hardware (CE-163 family,
    // CE-1600M, CE-1601M, CE-1638) -- eligible for the app's name-and-save
    // persistence flow. Has no effect on region/content parsing.
    bool battery = false;

    // A ROM module: every byte of every region is `rom` content (e.g. a
    // CE-502B program module). The app lists these in their own section.
    bool isRom() const {
        if (regions.empty()) return false;
        for (const Region& r : regions) {
            const uint32_t banks = r.banked ? r.banking.bankCount : 1;
            for (uint32_t b = 0; b < banks; ++b)
                if (r.contentForBank(b).kind != ContentKind::Rom) return false;
        }
        return true;
    }

    bool compatibleWith(CardHost h) const {
        for (CardHost c : compatibleHosts)
            if (c == h) return true;
        return false;
    }
};

bool parseMemoryCardDefinition(const std::string& yamlText, MemoryCardDefinition* out,
                               std::string* error);

// ── implementation ───────────────────────────────────────────────────────

inline bool cardHostFromToken(const std::string& tok, CardHost* out) {
    if (tok == "PC-1500") { *out = CardHost::PC1500; return true; }
    if (tok == "PC-1500A") { *out = CardHost::PC1500A; return true; }
    if (tok == "PC-1600-Slot-1") { *out = CardHost::PC1600Slot1; return true; }
    if (tok == "PC-1600-Slot-2") { *out = CardHost::PC1600Slot2; return true; }
    return false;
}

inline const char* cardHostToken(CardHost h) {
    switch (h) {
        case CardHost::PC1500: return "PC-1500";
        case CardHost::PC1500A: return "PC-1500A";
        case CardHost::PC1600Slot1: return "PC-1600-Slot-1";
        case CardHost::PC1600Slot2: return "PC-1600-Slot-2";
    }
    return "?";
}

inline int resolveSignalPin(CardHost t, const std::string& n) {
    switch (t) {
        case CardHost::PC1500:
            if (n == "Y0") return 4;
            if (n == "Y2") return 19;
            if (n == "S1") return 16;
            if (n == "S2") return 17;
            if (n == "S3") return 18;
            if (n == "S4") return 5;
            if (n == "PU") return 3;
            if (n == "PV") return 2;
            return -1;
        case CardHost::PC1500A:
            if (n == "Y0") return 4;
            if (n == "Y2") return 19;
            if (n == "S3") return 16;
            if (n == "S4") return 17;
            if (n == "S5") return 18;
            if (n == "PU") return 3;
            if (n == "PV") return 2;
            return -1;
        case CardHost::PC1600Slot1:
            if (n == "RAM2") return 4;
            if (n == "PVOUT") return 5;
            if (n == "S1") return 16;
            if (n == "S2") return 17;
            if (n == "S3") return 18;
            if (n == "PU") return 3;
            if (n == "PT") return 19;
            return -1;
        case CardHost::PC1600Slot2:
            if (n == "RAM1") return 4;
            if (n == "PVOUT") return 5;
            if (n == "K0") return 16;
            if (n == "K1") return 17;
            if (n == "K2") return 18;
            if (n == "PU") return 3;
            if (n == "PT") return 19;
            return -1;
    }
    return -1;
}

namespace mcd_detail {

inline bool isPow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Uppercase hex, no "0x" prefix -- for building "near offset 0x..." errors.
inline std::string hexStr(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%X", v);
    return std::string(b);
}

// "A12" → 12 (wantData=false); "D3" → 3 (wantData=true). -1 otherwise.
inline int busBit(const std::string& name, bool wantData) {
    if (name.size() < 2) return -1;
    if (name[0] != (wantData ? 'D' : 'A')) return -1;
    int v = 0;
    for (size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        v = v * 10 + (name[i] - '0');
    }
    int max = wantData ? 7 : 15;
    return (v <= max) ? v : -1;
}

inline bool getInt(const YamlNode& n, const char* what, long* out, std::string* error) {
    if (!n.asInt(out, error)) {
        *error = *error + " (" + what + ")";
        return false;
    }
    return true;
}

// Collect signal names from a scalar or a sequence-of-scalars term.
inline bool collectSignals(const YamlNode& n, std::vector<std::string>* out, std::string* error) {
    if (n.isScalar()) {
        std::string s;
        if (!n.asString(&s, error)) return false;
        out->push_back(s);
        return true;
    }
    if (n.isSeq()) {
        for (const auto& e : n.seq) {
            std::string s;
            if (!e.asString(&s, error)) return false;
            out->push_back(s);
        }
        return true;
    }
    *error = "line " + std::to_string(n.line) + ": expected a signal name or list of names";
    return false;
}

inline bool resolveInto(CardHost term, const std::vector<std::string>& names,
                        std::vector<int>* pins, std::string* error) {
    for (const auto& name : names) {
        int pin = resolveSignalPin(term, name);
        if (pin < 0) {
            *error = "signal '" + name + "' is not valid for terminology " +
                     std::string(cardHostToken(term));
            return false;
        }
        pins->push_back(pin);
    }
    return true;
}

// Parse one enable group. `gateOnly` forbids span/maps-to (a banked
// region's own addressing is a pure gate).
inline bool parseGroup(const YamlNode& g, CardHost term, bool gateOnly, EnableGroup* out,
                       std::string* error) {
    if (!g.isMap()) {
        *error = "line " + std::to_string(g.line) + ": addressing group must be a mapping";
        return false;
    }
    if (!g.requireOnlyKeys({"all-of", "chip-select", "signal", "signal-negated", "address-bits",
                            "memory-range", "span", "maps-to"},
                           error))
        return false;

    // Terms live either under `all-of:` or directly on the group map.
    std::vector<const YamlNode*> termMaps;
    if (const YamlNode* allOf = g.find("all-of")) {
        if (!allOf->isSeq()) {
            *error = "line " + std::to_string(allOf->line) + ": 'all-of' must be a list";
            return false;
        }
        for (const auto& t : allOf->seq) termMaps.push_back(&t);
    }
    termMaps.push_back(&g);  // also read term keys sitting directly on the group

    for (const YamlNode* tm : termMaps) {
        if (const YamlNode* cs = tm->find("chip-select")) {
            std::vector<std::string> names;
            if (!collectSignals(*cs, &names, error) ||
                !resolveInto(term, names, &out->requireHigh, error))
                return false;
        }
        if (const YamlNode* sg = tm->find("signal")) {
            std::vector<std::string> names;
            if (!collectSignals(*sg, &names, error) ||
                !resolveInto(term, names, &out->requireHigh, error))
                return false;
        }
        if (const YamlNode* sn = tm->find("signal-negated")) {
            std::vector<std::string> names;
            if (!collectSignals(*sn, &names, error) ||
                !resolveInto(term, names, &out->requireLow, error))
                return false;
        }
        if (const YamlNode* ab = tm->find("address-bits")) {
            if (!ab->isMap()) {
                *error = "line " + std::to_string(ab->line) + ": 'address-bits' must be a mapping";
                return false;
            }
            for (const auto& kv : ab->map) {
                int bit = busBit(kv.first, /*wantData=*/false);
                if (bit < 0) {
                    *error = "line " + std::to_string(ab->line) + ": '" + kv.first +
                             "' is not an address line (A0..A15)";
                    return false;
                }
                long level = 0;
                if (!kv.second.asInt(&level, error)) return false;
                if (level != 0 && level != 1) {
                    *error = "line " + std::to_string(ab->line) + ": address-bits level must be 0 or 1";
                    return false;
                }
                out->addrBits.push_back({bit, static_cast<int>(level)});
            }
        }
        if (const YamlNode* mr = tm->find("memory-range")) {
            const YamlNode* from = mr->find("from");
            const YamlNode* to = mr->find("to");
            if (!mr->isMap() || !from || !to) {
                *error = "line " + std::to_string(mr->line) +
                         ": 'memory-range' needs { from, to }";
                return false;
            }
            long f = 0, t = 0;
            if (!getInt(*from, "memory-range.from", &f, error) ||
                !getInt(*to, "memory-range.to", &t, error))
                return false;
            if (t < f) {
                *error = "line " + std::to_string(mr->line) + ": memory-range to < from";
                return false;
            }
            out->hasMemoryRange = true;
            out->rangeFrom = static_cast<uint32_t>(f);
            out->rangeTo = static_cast<uint32_t>(t);
            out->span = static_cast<uint32_t>(t - f + 1);
        }
    }

    bool hasSpan = g.has("span");
    bool hasMapsTo = g.has("maps-to");
    if (gateOnly && (hasSpan || hasMapsTo)) {
        *error = "line " + std::to_string(g.line) +
                 ": a banked region's addressing is a pure gate -- no span/maps-to here";
        return false;
    }
    if (hasSpan) {
        long s = 0;
        if (!getInt(*g.find("span"), "span", &s, error)) return false;
        if (s <= 0 || !isPow2(static_cast<uint32_t>(s))) {
            *error = "line " + std::to_string(g.line) + ": span must be a positive power of two";
            return false;
        }
        out->span = static_cast<uint32_t>(s);
    }
    if (hasMapsTo) {
        long m = 0;
        if (!getInt(*g.find("maps-to"), "maps-to", &m, error)) return false;
        out->mapsTo = static_cast<uint32_t>(m);
    }
    return true;
}

// `total` is the byte size the groups must tile (capacity or bank-size).
// `gateOnly` builds a pure gate (no tiling check, span/maps-to forbidden).
inline bool parseAddressing(const YamlNode& node, CardHost term, bool gateOnly, uint32_t total,
                            Addressing* out, std::string* error) {
    std::vector<const YamlNode*> groupNodes;
    if (node.isMap() && node.has("any-of")) {
        const YamlNode* anyOf = node.find("any-of");
        if (!node.requireOnlyKeys({"any-of"}, error)) return false;
        if (!anyOf->isSeq() || anyOf->seq.empty()) {
            *error = "line " + std::to_string(anyOf->line) + ": 'any-of' must be a non-empty list";
            return false;
        }
        for (const auto& g : anyOf->seq) groupNodes.push_back(&g);
    } else {
        groupNodes.push_back(&node);  // single-group shorthand
    }

    uint32_t nextOffset = 0;
    for (const YamlNode* gn : groupNodes) {
        EnableGroup grp;
        bool hadMapsTo = gn->isMap() && gn->has("maps-to");
        if (!parseGroup(*gn, term, gateOnly, &grp, error)) return false;
        if (!gateOnly) {
            if (grp.span == 0) {
                *error = "line " + std::to_string(gn->line) +
                         ": addressing group needs a 'span' (or a 'memory-range')";
                return false;
            }
            if (!hadMapsTo) grp.mapsTo = nextOffset;
            nextOffset = grp.mapsTo + grp.span;
        }
        out->groups.push_back(std::move(grp));
    }

    if (!gateOnly) {
        // Tile check: the groups' [mapsTo, mapsTo+span) slices must cover
        // [0, total) exactly, with no gap and no overlap.
        std::vector<std::pair<uint32_t, uint32_t>> spans;  // (start, end)
        for (const auto& g : out->groups) spans.emplace_back(g.mapsTo, g.mapsTo + g.span);
        std::sort(spans.begin(), spans.end());
        uint32_t cursor = 0;
        for (const auto& s : spans) {
            if (s.first != cursor) {
                *error = "addressing groups do not tile the region: gap or overlap near offset 0x" +
                         hexStr(cursor);
                return false;
            }
            cursor = s.second;
        }
        if (cursor != total) {
            *error = "addressing groups cover 0x" + hexStr(cursor) +
                     " bytes but the region is 0x" + hexStr(total);
            return false;
        }
    }
    return true;
}

// Parses `protocol:` for `content: { kind: flash, ... }` -- every field a
// "chip/firmware property" per spec §5, so none of it is inferred/defaulted.
inline bool parseFlashProtocol(const YamlNode& node, FlashProtocol* out, std::string* error) {
    if (!node.isMap() ||
        !node.requireOnlyKeys({"unlock-sequence", "byte-program-command", "erase-setup-command",
                                "sector-erase-command", "chip-erase-command", "sector-size",
                                "reset-command", "command-address-mask"},
                               error))
        return false;

    const YamlNode* us = node.find("unlock-sequence");
    if (!us || !us->isSeq() || us->seq.size() != 2) {
        *error = "line " + std::to_string(node.line) +
                 ": protocol needs 'unlock-sequence' with exactly 2 steps";
        return false;
    }
    for (const auto& step : us->seq) {
        if (!step.isMap() || !step.requireOnlyKeys({"address", "data"}, error)) {
            if (error->empty())
                *error = "line " + std::to_string(step.line) +
                         ": unlock-sequence step must be a mapping with address/data";
            return false;
        }
        const YamlNode* a = step.find("address");
        const YamlNode* d = step.find("data");
        if (!a || !d) {
            *error = "line " + std::to_string(step.line) + ": unlock-sequence step needs address and data";
            return false;
        }
        long av = 0, dv = 0;
        if (!getInt(*a, "unlock-sequence.address", &av, error)) return false;
        if (!getInt(*d, "unlock-sequence.data", &dv, error)) return false;
        out->unlockSequence.push_back({static_cast<uint32_t>(av), static_cast<uint8_t>(dv)});
    }

    auto reqByte = [&](const char* key, uint8_t* dst) {
        const YamlNode* n = node.find(key);
        if (!n) {
            *error = "line " + std::to_string(node.line) + ": protocol needs '" + key + "'";
            return false;
        }
        long v = 0;
        if (!getInt(*n, key, &v, error)) return false;
        *dst = static_cast<uint8_t>(v);
        return true;
    };
    if (!reqByte("byte-program-command", &out->byteProgramCommand)) return false;
    if (!reqByte("erase-setup-command", &out->eraseSetupCommand)) return false;
    if (!reqByte("sector-erase-command", &out->sectorEraseCommand)) return false;
    if (!reqByte("chip-erase-command", &out->chipEraseCommand)) return false;
    if (!reqByte("reset-command", &out->resetCommand)) return false;

    const YamlNode* ss = node.find("sector-size");
    if (!ss) {
        *error = "line " + std::to_string(node.line) + ": protocol needs 'sector-size'";
        return false;
    }
    long ssv = 0;
    if (!getInt(*ss, "sector-size", &ssv, error)) return false;
    if (ssv <= 0 || !isPow2(static_cast<uint32_t>(ssv))) {
        *error = "line " + std::to_string(ss->line) + ": sector-size must be a positive power of two";
        return false;
    }
    out->sectorSize = static_cast<uint32_t>(ssv);

    if (const YamlNode* mask = node.find("command-address-mask")) {
        long mv = 0;
        if (!getInt(*mask, "command-address-mask", &mv, error)) return false;
        out->commandAddressMask = static_cast<uint32_t>(mv);
    }
    return true;
}

// Parses one content entry -- either a whole region's `content:` or one
// `by-bank:` list entry (which additionally carries `banks:`, ignored
// here -- the caller consumes it before/after calling this).
inline bool parseContent(const YamlNode& node, RegionContent* out, std::string* error) {
    std::string kind;
    const YamlNode* body = nullptr;
    if (node.isScalar()) {
        if (!node.asString(&kind, error)) return false;
    } else if (node.isMap()) {
        const YamlNode* k = node.find("kind");
        if (!k) {
            *error = "line " + std::to_string(node.line) + ": 'content' mapping needs 'kind'";
            return false;
        }
        if (!k->asString(&kind, error)) return false;
        body = &node;
    } else {
        *error = "line " + std::to_string(node.line) + ": 'content' must be a string or mapping";
        return false;
    }

    if (kind == "rom") {
        // No options: read-only is the whole point, and the bytes come from
        // `initial-content` (coverage checked in parseRegion()).
        if (body && !body->requireOnlyKeys({"kind", "banks"}, error)) return false;
        out->kind = ContentKind::Rom;
        out->writable = false;
        return true;
    }
    if (kind == "flash") {
        if (!body || !body->has("protocol")) {
            *error = "line " + std::to_string(node.line) +
                     ": 'flash' content needs a mapping with 'protocol' (chip/firmware "
                     "properties are never inferred, spec §5)";
            return false;
        }
        if (!body->requireOnlyKeys({"kind", "power-up-fill", "protocol", "banks"}, error))
            return false;
        out->kind = ContentKind::Flash;
        out->writable = false;  // meaningless here -- gated by the command decoder, not this flag
        out->powerUpFill = 0xFF;  // erased, unless `power-up-fill` below says otherwise
        if (const YamlNode* pf = body->find("power-up-fill")) {
            long v = 0;
            if (!getInt(*pf, "power-up-fill", &v, error)) return false;
            out->powerUpFill = static_cast<uint8_t>(v);
        }
        return parseFlashProtocol(*body->find("protocol"), &out->flash, error);
    }
    if (kind != "regular") {
        *error = "line " + std::to_string(node.line) + ": unknown content kind '" + kind + "'";
        return false;
    }

    if (body) {
        if (!body->requireOnlyKeys({"kind", "writable", "power-up-fill", "write-protect", "banks"},
                                   error))
            return false;
        if (const YamlNode* w = body->find("writable")) {
            if (!w->asBool(&out->writable, error)) return false;
        }
        if (const YamlNode* pf = body->find("power-up-fill")) {
            long v = 0;
            if (!getInt(*pf, "power-up-fill", &v, error)) return false;
            out->powerUpFill = static_cast<uint8_t>(v);
        }
        if (const YamlNode* wp = body->find("write-protect")) {
            if (!wp->isMap() || !wp->requireOnlyKeys({"default", "persisted"}, error)) {
                if (error->empty())
                    *error = "line " + std::to_string(wp->line) + ": 'write-protect' must be a mapping";
                return false;
            }
            out->hasWriteProtect = true;
            if (const YamlNode* d = wp->find("default")) {
                std::string s;
                if (!d->asString(&s, error)) return false;
                if (s == "protected") out->writeProtectDefaultProtected = true;
                else if (s == "unprotected") out->writeProtectDefaultProtected = false;
                else {
                    *error = "line " + std::to_string(d->line) +
                             ": write-protect default must be protected|unprotected";
                    return false;
                }
            }
            if (const YamlNode* p = wp->find("persisted")) {
                if (!p->asBool(&out->writeProtectPersisted, error)) return false;
            }
        }
    }
    return true;
}

inline bool parseBanking(const YamlNode& node, CardHost term, Banking* out, std::string* error) {
    if (!node.isMap() ||
        !node.requireOnlyKeys({"latch", "bank-count", "bank-size", "bank-window"}, error))
        return false;
    const YamlNode* latch = node.find("latch");
    const YamlNode* bc = node.find("bank-count");
    const YamlNode* bs = node.find("bank-size");
    const YamlNode* bw = node.find("bank-window");
    if (!latch || !bc || !bs || !bw) {
        *error = "line " + std::to_string(node.line) +
                 ": banking needs latch, bank-count, bank-size, bank-window";
        return false;
    }
    if (!latch->isMap() ||
        !latch->requireOnlyKeys({"type", "trigger", "sampled-lines", "source-domain", "lines"},
                                error))
        return false;

    std::string type;
    const YamlNode* typeN = latch->find("type");
    if (!typeN || !typeN->asString(&type, error)) {
        if (error->empty()) *error = "line " + std::to_string(latch->line) + ": latch needs 'type'";
        return false;
    }
    if (type == "line-based") {
        *error = "line " + std::to_string(latch->line) +
                 ": line-based banking is not supported in v1 (trigger-based only)";
        return false;
    }
    if (type != "trigger-based") {
        *error = "line " + std::to_string(latch->line) + ": unknown latch type '" + type + "'";
        return false;
    }

    const YamlNode* trig = latch->find("trigger");
    if (!trig || !trig->isMap() || !trig->requireOnlyKeys({"pin", "io-port"}, error)) {
        if (error->empty())
            *error = "line " + std::to_string(latch->line) +
                     ": trigger-based latch needs trigger: { pin: N } or { io-port: 0xNN }";
        return false;
    }
    const YamlNode* pinN = trig->find("pin");
    const YamlNode* portN = trig->find("io-port");
    if ((pinN && portN) || (!pinN && !portN)) {
        *error = "line " + std::to_string(trig->line) +
                 ": trigger needs exactly one of 'pin' or 'io-port'";
        return false;
    }
    if (pinN) {
        long v = 0;
        if (!getInt(*pinN, "trigger.pin", &v, error)) return false;
        if (v < 1 || v > 40) {
            *error = "line " + std::to_string(trig->line) + ": trigger pin must be 1..40";
            return false;
        }
        out->triggerKind = TriggerKind::Pin;
        out->triggerPin = static_cast<int>(v);
    } else {
        long v = 0;
        if (!getInt(*portN, "trigger.io-port", &v, error)) return false;
        if (v < 0 || v > 0xFF) {
            *error = "line " + std::to_string(trig->line) + ": trigger io-port must be 0..0xFF";
            return false;
        }
        out->triggerKind = TriggerKind::IoPort;
        out->triggerPort = static_cast<uint8_t>(v);
    }

    std::string domain;
    const YamlNode* dom = latch->find("source-domain");
    if (!dom || !dom->asString(&domain, error)) {
        if (error->empty())
            *error = "line " + std::to_string(latch->line) + ": latch needs 'source-domain'";
        return false;
    }
    if (domain == "data") out->sampleData = true;
    else if (domain == "address" || domain == "address-select") out->sampleData = false;
    else {
        *error = "line " + std::to_string(dom->line) + ": source-domain must be address or data";
        return false;
    }

    const YamlNode* sl = latch->find("sampled-lines");
    if (!sl || !sl->isSeq() || sl->seq.empty()) {
        *error = "line " + std::to_string(latch->line) +
                 ": trigger-based latch needs a non-empty 'sampled-lines' list";
        return false;
    }
    for (const auto& e : sl->seq) {
        std::string name;
        if (!e.asString(&name, error)) return false;
        int bit = busBit(name, out->sampleData);
        if (bit < 0) {
            *error = "line " + std::to_string(e.line) + ": '" + name +
                     "' is not a " + (out->sampleData ? "data (D0..D7)" : "address (A0..A15)") +
                     " line for this source-domain";
            return false;
        }
        out->sampledBits.push_back(bit);
    }

    long bcv = 0, bsv = 0;
    if (!getInt(*bc, "bank-count", &bcv, error)) return false;
    if (!getInt(*bs, "bank-size", &bsv, error)) return false;
    if (bcv <= 0) {
        *error = "line " + std::to_string(bc->line) + ": bank-count must be positive";
        return false;
    }
    if (bsv <= 0 || !isPow2(static_cast<uint32_t>(bsv))) {
        *error = "line " + std::to_string(bs->line) + ": bank-size must be a positive power of two";
        return false;
    }
    out->bankCount = static_cast<uint32_t>(bcv);
    out->bankSize = static_cast<uint32_t>(bsv);

    return parseAddressing(*bw, term, /*gateOnly=*/false, out->bankSize, &out->bankWindow, error);
}

// "8" or "0-7" -> [lo, hi] inclusive. Reuses asInt-style decimal/0x parsing
// on each side (bank numbers are always small, plain decimal in practice).
inline bool parseBankRangeToken(const std::string& s, uint32_t* lo, uint32_t* hi,
                                std::string* error) {
    auto parseOne = [&](const std::string& tok, long* out) -> bool {
        if (tok.empty()) return false;
        char* endp = nullptr;
        int base = 10;
        size_t start = 0;
        if (tok.size() >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
            base = 16;
            start = 2;
        }
        long v = std::strtol(tok.c_str() + start, &endp, base);
        if (!endp || *endp != '\0' || (start >= tok.size())) return false;
        *out = v;
        return true;
    };
    size_t dash = s.find('-');
    long loV = 0, hiV = 0;
    if (dash == std::string::npos) {
        if (!parseOne(s, &loV)) {
            *error = "'" + s + "' is not a bank number or range";
            return false;
        }
        hiV = loV;
    } else {
        if (!parseOne(s.substr(0, dash), &loV) || !parseOne(s.substr(dash + 1), &hiV)) {
            *error = "'" + s + "' is not a valid 'lo-hi' bank range";
            return false;
        }
    }
    if (loV < 0 || hiV < loV) {
        *error = "'" + s + "' is not a valid bank range (lo <= hi, both >= 0)";
        return false;
    }
    *lo = static_cast<uint32_t>(loV);
    *hi = static_cast<uint32_t>(hiV);
    return true;
}

// Dispatches a region's `content:` node: either the single-kind form
// (handled by parseContent) or a `by-bank:` list (spec §5's split-content
// note) -- `bankCount` is 0 for an unbanked region, where by-bank is a
// hard error (there is only ever one "bank").
inline bool parseContentSpec(const YamlNode& node, uint32_t bankCount, RegionContent* singleOut,
                             std::vector<BankContentRange>* byBankOut, std::string* error) {
    if (!(node.isMap() && node.has("by-bank"))) return parseContent(node, singleOut, error);

    if (!node.requireOnlyKeys({"by-bank"}, error)) return false;
    if (bankCount == 0) {
        *error = "line " + std::to_string(node.line) +
                 ": 'by-bank' content needs a banked region";
        return false;
    }
    const YamlNode* list = node.find("by-bank");
    if (!list->isSeq() || list->seq.empty()) {
        *error = "line " + std::to_string(list->line) + ": 'by-bank' must be a non-empty list";
        return false;
    }
    for (const auto& entry : list->seq) {
        if (!entry.isMap()) {
            *error = "line " + std::to_string(entry.line) + ": by-bank entry must be a mapping";
            return false;
        }
        const YamlNode* banksN = entry.find("banks");
        if (!banksN) {
            *error = "line " + std::to_string(entry.line) + ": by-bank entry needs 'banks'";
            return false;
        }
        std::string banksStr;
        if (!banksN->asString(&banksStr, error)) return false;
        BankContentRange bcr;
        if (!parseBankRangeToken(banksStr, &bcr.loBank, &bcr.hiBank, error)) {
            *error = "line " + std::to_string(banksN->line) + ": " + *error;
            return false;
        }
        if (bcr.hiBank >= bankCount) {
            *error = "line " + std::to_string(banksN->line) + ": 'banks: " + banksStr +
                     "' is out of range 0.." + std::to_string(bankCount - 1);
            return false;
        }
        if (!parseContent(entry, &bcr.content, error)) return false;
        byBankOut->push_back(std::move(bcr));
    }

    // Tile check: the ranges must partition 0..bankCount-1 exactly.
    std::vector<std::pair<uint32_t, uint32_t>> spans;  // (lo, hi+1)
    for (const auto& r : *byBankOut) spans.emplace_back(r.loBank, r.hiBank + 1);
    std::sort(spans.begin(), spans.end());
    uint32_t cursor = 0;
    for (const auto& s : spans) {
        if (s.first != cursor) {
            *error = "'by-bank' ranges do not partition the region: gap or overlap near bank " +
                     std::to_string(cursor);
            return false;
        }
        cursor = s.second;
    }
    if (cursor != bankCount) {
        *error = "'by-bank' ranges cover banks 0.." + std::to_string(cursor - 1) + " but the region has " +
                 std::to_string(bankCount) + " banks";
        return false;
    }
    return true;
}

// One hex byte pair ("4B") -> its value. -1 if not two valid hex digits.
inline int hexBytePair(const std::string& tok) {
    if (tok.size() != 2 || !std::isxdigit(static_cast<unsigned char>(tok[0])) ||
        !std::isxdigit(static_cast<unsigned char>(tok[1])))
        return -1;
    return static_cast<int>(std::strtol(tok.c_str(), nullptr, 16));
}

// `encoding: hex` -- whitespace/newlines ignored, a flat stream of hex byte
// pairs (Format.md §6). Used as-is, at the block's `offset`; bytes it
// doesn't cover keep the bank's power-up-fill.
inline bool parsePlainHex(const std::string& text, std::vector<uint8_t>* out, std::string* error) {
    std::string tok;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (tok.empty()) continue;
            int v = hexBytePair(tok);
            if (v < 0) { *error = "'" + tok + "' is not a hex byte pair"; return false; }
            out->push_back(static_cast<uint8_t>(v));
            tok.clear();
        } else {
            tok.push_back(c);
        }
    }
    if (!tok.empty()) {
        int v = hexBytePair(tok);
        if (v < 0) { *error = "'" + tok + "' is not a hex byte pair"; return false; }
        out->push_back(static_cast<uint8_t>(v));
    }
    if (out->empty()) { *error = "'hex' block has no bytes"; return false; }
    return true;
}

// `encoding: base64` (Format.md §6) -- standard alphabet, '=' padding
// optional/ignored on decode.
inline bool parseBase64(const std::string& text, std::vector<uint8_t>* out, std::string* error) {
    auto decodeChar = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    uint32_t acc = 0;
    int bits = 0;
    for (char raw : text) {
        unsigned char c = static_cast<unsigned char>(raw);
        if (std::isspace(c) || c == '=') continue;
        int v = decodeChar(c);
        if (v < 0) { *error = "invalid base64 character"; return false; }
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    if (out->empty()) { *error = "'base64' block has no bytes"; return false; }
    return true;
}

// This project's own dialect for a labeled, unambiguously-compactable hex
// dump: lines of "$XXXX: <up to 16 space-separated hex byte pairs>" or
// "$XXXX: XX..." where the trailing "..." means "byte XX repeats from this
// address up to (but not including) the next line's address, or the end of
// `blockLength` for the last line." Lines must partition [0, blockLength)
// exactly -- no gap, no overlap -- which is what makes "..." exact rather
// than a guess (unlike the elided `debugDumpMemRegion` console dump this
// mirrors). Designed to be the direct output of a "dump whole card, ready
// to paste" debug feature, so a real memory dump needs no reformatting to
// become a `bytes: |` block.
inline bool parseAddressedHex(const std::string& text, uint32_t blockLength,
                              std::vector<uint8_t>* out, std::string* error) {
    struct Line {
        uint32_t addr;
        bool isRun;
        uint8_t runByte;
        std::vector<uint8_t> bytes;
    };
    std::vector<Line> lines;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        size_t b = raw.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;  // blank line
        size_t e = raw.find_last_not_of(" \t\r");
        std::string s = raw.substr(b, e - b + 1);

        if (s[0] != '$') { *error = "addressed-hex line must start with '$': '" + s + "'"; return false; }
        size_t colon = s.find(':');
        if (colon == std::string::npos) {
            *error = "addressed-hex line missing ':': '" + s + "'";
            return false;
        }
        char* endp = nullptr;
        std::string addrStr = s.substr(1, colon - 1);
        long addrVal = std::strtol(addrStr.c_str(), &endp, 16);
        if (addrStr.empty() || !endp || *endp != '\0' || addrVal < 0) {
            *error = "bad address in addressed-hex line: '" + s + "'";
            return false;
        }

        std::vector<std::string> toks;
        {
            std::string tok;
            for (size_t i = colon + 1; i <= s.size(); ++i) {
                char c = (i < s.size()) ? s[i] : ' ';
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!tok.empty()) { toks.push_back(tok); tok.clear(); }
                } else {
                    tok.push_back(c);
                }
            }
        }
        if (toks.empty()) { *error = "empty addressed-hex line: '" + s + "'"; return false; }

        Line line;
        line.addr = static_cast<uint32_t>(addrVal);
        if (toks.size() == 1 && toks[0].size() == 5 && toks[0].substr(2) == "...") {
            int v = hexBytePair(toks[0].substr(0, 2));
            if (v < 0) { *error = "bad run byte in addressed-hex line: '" + s + "'"; return false; }
            line.isRun = true;
            line.runByte = static_cast<uint8_t>(v);
        } else {
            line.isRun = false;
            for (const auto& tok : toks) {
                int v = hexBytePair(tok);
                if (v < 0) {
                    *error = "'" + tok + "' is not a hex byte pair in addressed-hex line: '" + s + "'";
                    return false;
                }
                line.bytes.push_back(static_cast<uint8_t>(v));
            }
        }
        lines.push_back(std::move(line));
    }
    if (lines.empty()) { *error = "addressed-hex block has no lines"; return false; }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.addr < b.addr; });

    out->assign(blockLength, 0);
    uint32_t cursor = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& line = lines[i];
        if (line.addr != cursor) {
            *error = "addressed-hex lines have a gap or overlap at/near $" + hexStr(cursor);
            return false;
        }
        if (line.isRun) {
            uint32_t end = (i + 1 < lines.size()) ? lines[i + 1].addr : blockLength;
            if (end <= cursor || end > blockLength) {
                *error = "addressed-hex run at $" + hexStr(cursor) + " has an invalid length";
                return false;
            }
            std::fill(out->begin() + cursor, out->begin() + end, line.runByte);
            cursor = end;
        } else {
            uint32_t end = cursor + static_cast<uint32_t>(line.bytes.size());
            if (end > blockLength) {
                *error = "addressed-hex line at $" + hexStr(cursor) + " overruns the block";
                return false;
            }
            std::copy(line.bytes.begin(), line.bytes.end(), out->begin() + cursor);
            cursor = end;
        }
    }
    if (cursor != blockLength) {
        *error = "addressed-hex block covers 0x" + hexStr(cursor) + " bytes but needs 0x" +
                 hexStr(blockLength);
        return false;
    }
    return true;
}

// `initial-content` (spec §5a, Format.md §6): resolves each referenced
// bank (unbanked regions use key 0) into a full bank-size/`capacity`-length
// byte buffer, starting from that bank's power-up-fill and overlaying each
// block's bytes at its offset. `encoding: file` is not implemented (it
// would need the definition file's own directory threaded through, which
// nothing needs yet) -- unlike `rom`/`by-bank` elsewhere in this parser,
// unsupported here means "not yet", not "never".
inline bool parseInitialContent(const YamlNode& node, const Region& regionSoFar,
                                std::unordered_map<uint32_t, std::vector<uint8_t>>* out,
                                std::unordered_map<uint32_t, std::vector<bool>>* coveredOut,
                                std::string* error) {
    if (!node.isMap() || !node.requireOnlyKeys({"fill", "blocks"}, error)) return false;

    bool hasFillOverride = false;
    uint8_t fillOverride = 0;
    if (const YamlNode* f = node.find("fill")) {
        long v = 0;
        if (!getInt(*f, "fill", &v, error)) return false;
        fillOverride = static_cast<uint8_t>(v);
        hasFillOverride = true;
    }

    const YamlNode* blocksN = node.find("blocks");
    if (!blocksN || !blocksN->isSeq() || blocksN->seq.empty()) {
        *error = "line " + std::to_string(node.line) + ": 'initial-content' needs a non-empty 'blocks' list";
        return false;
    }

    uint32_t bankCount = regionSoFar.banked ? regionSoFar.banking.bankCount : 1;
    uint32_t bankSize = regionSoFar.banked ? regionSoFar.banking.bankSize : regionSoFar.capacity;
    auto& covered = *coveredOut;  // per-bank coverage, for overlap checks and the ROM rule

    for (const auto& entry : blocksN->seq) {
        if (!entry.isMap() ||
            !entry.requireOnlyKeys({"bank", "offset", "encoding", "bytes", "path"}, error))
            return false;

        uint32_t bank = 0;
        const YamlNode* bankN = entry.find("bank");
        if (bankN) {
            if (!regionSoFar.banked) {
                *error = "line " + std::to_string(bankN->line) +
                         ": 'bank' is only valid for a banked region";
                return false;
            }
            long v = 0;
            if (!getInt(*bankN, "bank", &v, error)) return false;
            if (v < 0 || static_cast<uint32_t>(v) >= bankCount) {
                *error = "line " + std::to_string(bankN->line) + ": bank " + std::to_string(v) +
                         " is out of range 0.." + std::to_string(bankCount - 1);
                return false;
            }
            bank = static_cast<uint32_t>(v);
        } else if (regionSoFar.banked) {
            *error = "line " + std::to_string(entry.line) +
                     ": a banked region's initial-content block needs 'bank'";
            return false;
        }

        long offV = 0;
        if (const YamlNode* offN = entry.find("offset")) {
            if (!getInt(*offN, "offset", &offV, error)) return false;
        }
        if (offV < 0 || static_cast<uint32_t>(offV) > bankSize) {
            *error = "line " + std::to_string(entry.line) + ": offset is outside the bank";
            return false;
        }
        uint32_t offset = static_cast<uint32_t>(offV);

        std::string encoding;
        const YamlNode* encN = entry.find("encoding");
        if (!encN || !encN->asString(&encoding, error)) {
            if (error->empty())
                *error = "line " + std::to_string(entry.line) + ": block needs 'encoding'";
            return false;
        }

        std::vector<uint8_t> bytes;
        if (encoding == "addressed-hex") {
            const YamlNode* bN = entry.find("bytes");
            if (!bN) {
                *error = "line " + std::to_string(entry.line) + ": 'addressed-hex' needs 'bytes'";
                return false;
            }
            std::string text;
            if (!bN->asString(&text, error)) return false;
            if (!parseAddressedHex(text, bankSize - offset, &bytes, error)) {
                *error = "line " + std::to_string(bN->line) + ": " + *error;
                return false;
            }
        } else if (encoding == "hex") {
            const YamlNode* bN = entry.find("bytes");
            if (!bN) {
                *error = "line " + std::to_string(entry.line) + ": 'hex' needs 'bytes'";
                return false;
            }
            std::string text;
            if (!bN->asString(&text, error)) return false;
            if (!parsePlainHex(text, &bytes, error)) {
                *error = "line " + std::to_string(bN->line) + ": " + *error;
                return false;
            }
        } else if (encoding == "base64") {
            const YamlNode* bN = entry.find("bytes");
            if (!bN) {
                *error = "line " + std::to_string(entry.line) + ": 'base64' needs 'bytes'";
                return false;
            }
            std::string text;
            if (!bN->asString(&text, error)) return false;
            if (!parseBase64(text, &bytes, error)) {
                *error = "line " + std::to_string(bN->line) + ": " + *error;
                return false;
            }
        } else if (encoding == "file") {
            *error = "line " + std::to_string(entry.line) + ": encoding 'file' is not supported yet";
            return false;
        } else {
            *error = "line " + std::to_string(encN->line) + ": unknown encoding '" + encoding + "'";
            return false;
        }

        if (offset + bytes.size() > bankSize) {
            *error = "line " + std::to_string(entry.line) + ": block runs past the end of the bank";
            return false;
        }

        auto& buf = (*out)[bank];
        if (buf.empty())
            buf.assign(bankSize, regionSoFar.contentForBank(bank).powerUpFill);
        auto& cov = covered[bank];
        if (cov.empty()) cov.assign(bankSize, false);
        for (size_t i = 0; i < bytes.size(); ++i) {
            uint32_t a = offset + static_cast<uint32_t>(i);
            if (cov[a]) {
                *error = "line " + std::to_string(entry.line) + ": initial-content blocks overlap at bank " +
                         std::to_string(bank) + " offset 0x" + hexStr(a);
                return false;
            }
            cov[a] = true;
            buf[a] = bytes[i];
        }
        if (hasFillOverride) {
            for (uint32_t a = 0; a < bankSize; ++a)
                if (!cov[a]) buf[a] = fillOverride;
        }
    }
    return true;
}

inline bool parseRegion(const YamlNode& node, CardHost term, Region* out, std::string* error) {
    if (!node.isMap() ||
        !node.requireOnlyKeys({"name", "addressing", "content", "banking", "capacity",
                               "initial-content", "pc1600-module-class"},
                              error))
        return false;

    const YamlNode* initialContentN = node.find("initial-content");
    if (const YamlNode* cls = node.find("pc1600-module-class")) {
        std::string s;
        if (!cls->asString(&s, error)) return false;
        if (s != "plain-ram-no-header" && !initialContentN) {
            *error = "line " + std::to_string(cls->line) + ": pc1600-module-class '" + s +
                     "' needs a header image (initial-content)";
            return false;
        }
    }

    const YamlNode* nameN = node.find("name");
    const YamlNode* addrN = node.find("addressing");
    const YamlNode* contN = node.find("content");
    const YamlNode* bankN = node.find("banking");
    if (!nameN || !addrN || !contN || !bankN) {
        *error = "line " + std::to_string(node.line) +
                 ": region needs name, addressing, content, banking";
        return false;
    }
    if (!nameN->asString(&out->name, error)) return false;

    bool banked = false;
    if (bankN->isScalar()) {
        std::string s;
        if (!bankN->asString(&s, error)) return false;
        if (s != "none") {
            *error = "line " + std::to_string(bankN->line) + ": banking must be 'none' or a mapping";
            return false;
        }
    } else {
        banked = true;
    }
    out->banked = banked;

    if (banked) {
        if (node.has("capacity")) {
            *error = "line " + std::to_string(node.find("capacity")->line) +
                     ": a banked region omits 'capacity' (it is bank-count * bank-size)";
            return false;
        }
        if (!parseBanking(*bankN, term, &out->banking, error)) return false;
        // region-level addressing is a pure gate
        if (!parseAddressing(*addrN, term, /*gateOnly=*/true, 0, &out->addressing, error))
            return false;
    } else {
        const YamlNode* capN = node.find("capacity");
        if (!capN) {
            *error = "line " + std::to_string(node.line) +
                     ": an unbanked region needs 'capacity'";
            return false;
        }
        long cap = 0;
        if (!getInt(*capN, "capacity", &cap, error)) return false;
        if (cap <= 0) {
            *error = "line " + std::to_string(capN->line) + ": capacity must be positive";
            return false;
        }
        out->capacity = static_cast<uint32_t>(cap);
        if (!parseAddressing(*addrN, term, /*gateOnly=*/false, out->capacity, &out->addressing,
                             error))
            return false;
    }

    // Content is parsed after banking so `by-bank:` can validate its
    // ranges against bank-count.
    if (!parseContentSpec(*contN, banked ? out->banking.bankCount : 0, &out->content,
                          &out->contentByBank, error))
        return false;

    // A flash range's sector-erase must stay inside one bank.
    if (banked) {
        auto checkSectorSize = [&](const RegionContent& c) -> bool {
            if (c.kind != ContentKind::Flash) return true;
            if (c.flash.sectorSize > out->banking.bankSize ||
                out->banking.bankSize % c.flash.sectorSize != 0) {
                *error = "line " + std::to_string(contN->line) +
                         ": flash 'sector-size' must evenly divide 'bank-size'";
                return false;
            }
            return true;
        };
        if (out->contentByBank.empty()) {
            if (!checkSectorSize(out->content)) return false;
        } else {
            for (const auto& r : out->contentByBank)
                if (!checkSectorSize(r.content)) return false;
        }
    }

    // initial-content is resolved last: it needs `out->banked`/`banking`/
    // `content`/`contentByBank` already in place for contentForBank().
    std::unordered_map<uint32_t, std::vector<bool>> covered;
    if (initialContentN) {
        if (!parseInitialContent(*initialContentN, *out, &out->initialContentByBank, &covered, error))
            return false;
    }

    // A ROM range has no power-up state of its own: `initial-content` blocks
    // must cover every byte of it (spec §5a). `fill:` doesn't count.
    const uint32_t bankCount = out->banked ? out->banking.bankCount : 1;
    const uint32_t bankSize = out->banked ? out->banking.bankSize : out->capacity;
    for (uint32_t b = 0; b < bankCount; ++b) {
        if (out->contentForBank(b).kind != ContentKind::Rom) continue;
        auto it = covered.find(b);
        uint32_t gap = 0;
        if (it != covered.end())
            while (gap < bankSize && it->second[gap]) ++gap;
        if (gap < bankSize) {
            *error = "line " + std::to_string(node.line) + ": 'rom' content needs initial-content covering " +
                     "every byte -- " + (out->banked ? "bank " + std::to_string(b) + " " : std::string()) +
                     "offset 0x" + hexStr(gap) + " is not covered";
            return false;
        }
    }
    return true;
}

}  // namespace mcd_detail

inline bool parseMemoryCardDefinition(const std::string& yamlText, MemoryCardDefinition* out,
                                      std::string* error) {
    YamlNode root;
    if (!parseYaml(yamlText, &root, error)) return false;
    if (!root.isMap()) {
        *error = "card definition must be a mapping";
        return false;
    }
    if (!root.requireOnlyKeys(
            {"module-name", "compatible-hosts", "definition-terminology", "regions", "notes",
             "battery"},
            error))
        return false;

    const YamlNode* nameN = root.find("module-name");
    const YamlNode* hostsN = root.find("compatible-hosts");
    const YamlNode* termN = root.find("definition-terminology");
    const YamlNode* regionsN = root.find("regions");
    if (!nameN || !hostsN || !termN || !regionsN) {
        *error = "card definition needs module-name, compatible-hosts, definition-terminology, regions";
        return false;
    }
    if (!nameN->asString(&out->moduleName, error)) return false;

    if (const YamlNode* batteryN = root.find("battery")) {
        if (!batteryN->asBool(&out->battery, error)) return false;
    }

    if (!hostsN->isSeq() || hostsN->seq.empty()) {
        *error = "line " + std::to_string(hostsN->line) + ": compatible-hosts must be a non-empty list";
        return false;
    }
    for (const auto& h : hostsN->seq) {
        std::string tok;
        if (!h.asString(&tok, error)) return false;
        CardHost ch;
        if (!cardHostFromToken(tok, &ch)) {
            *error = "line " + std::to_string(h.line) + ": unknown host '" + tok + "'";
            return false;
        }
        out->compatibleHosts.push_back(ch);
    }

    std::string termTok;
    if (!termN->asString(&termTok, error)) return false;
    if (!cardHostFromToken(termTok, &out->terminology)) {
        *error = "line " + std::to_string(termN->line) + ": unknown definition-terminology '" +
                 termTok + "'";
        return false;
    }
    if (!out->compatibleWith(out->terminology)) {
        *error = "definition-terminology " + termTok + " is not in compatible-hosts";
        return false;
    }

    if (!regionsN->isSeq() || regionsN->seq.empty()) {
        *error = "line " + std::to_string(regionsN->line) + ": regions must be a non-empty list";
        return false;
    }
    for (const auto& rn : regionsN->seq) {
        Region region;
        if (!mcd_detail::parseRegion(rn, out->terminology, &region, error)) return false;
        for (const auto& ex : out->regions) {
            if (ex.name == region.name) {
                *error = "duplicate region name '" + region.name + "'";
                return false;
            }
        }
        out->regions.push_back(std::move(region));
    }
    return true;
}
