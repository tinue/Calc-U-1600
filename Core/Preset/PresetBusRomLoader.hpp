#pragma once
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Connector/BusRomCard.hpp"
#include "../FileIO.hpp"
#include "PresetFile.hpp"

// Builds the cards for a preset's `bus-rom:` items (PresetFile::busRoms).
// The file is read here, at load time, so a clean start picks up a fresh
// build. Returns null with `*error` set on a missing or oversized file.
namespace preset_bus_rom {

inline bool readRom(const PresetBusRom& rom, size_t maxSize, std::vector<uint8_t>* out, std::string* error) {
    if (!readWholeFile(rom.path, out)) {
        *error = "could not read " + rom.path;
        return false;
    }
    if (out->empty() || out->size() > maxSize) {
        *error = rom.path + ": " + std::to_string(out->size()) + " bytes, expected 1-" + std::to_string(maxSize);
        return false;
    }
    return true;
}

/// The card's name in the inspector: the ROM's file name.
inline std::string fileName(const PresetBusRom& rom) {
    return std::filesystem::path(rom.path).filename().string();
}

/// An `address` ROM: the PC-1500 connector, or the PC-1600's LH5803 side.
inline std::unique_ptr<BusRomCard> makeCard(const PresetBusRom& rom, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readRom(rom, 0x10000u - rom.address, &bytes, error)) return nullptr;
    return std::make_unique<BusRomCard>(std::move(bytes), rom.address, rom.me1, rom.pv, rom.pu, fileName(rom));
}

/// A `bank` ROM on the PC-1600 system bus.
inline std::unique_ptr<PC1600BusRomCard> makeSystemBusCard(const PresetBusRom& rom, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readRom(rom, PC1600BusRomCard::kBankSize, &bytes, error)) return nullptr;
    return std::make_unique<PC1600BusRomCard>(std::move(bytes), uint8_t(rom.bank), fileName(rom));
}

/// Attaches `card` (null = its make*Card() failed) to `machine`.
template <class Machine, class Card>
bool attach(Machine& machine, std::unique_ptr<Card> card) {
    if (!card) return false;
    machine.attachBusRom(std::move(card));
    return true;
}

inline std::string describe(const PresetBusRom& rom);

/// Attaches every `bus-rom:` item through `attachOne(rom, &err)` (a make*Card()
/// plus attach()), logging each; the first failure stops with `*error` set.
/// Called last, but it puts each card in front of its chain, so a bus ROM
/// shadows the ROM of a peripheral attached earlier at the same place (e.g.
/// a rebuilt host-drive ROM in bank 7).
template <class AttachOne>
bool attachAll(const std::vector<PresetBusRom>& roms, AttachOne attachOne,
               const std::function<void(const std::string&)>& log, std::string* error) {
    for (const PresetBusRom& rom : roms) {
        std::string err;
        if (!attachOne(rom, &err)) {
            *error = "bus-rom: " + err;
            if (log) log(*error);
            return false;
        }
        if (log) log(describe(rom));
    }
    return true;
}

/// A line for the preset log.
inline std::string describe(const PresetBusRom& rom) {
    char where[48];
    if (rom.bank >= 0) std::snprintf(where, sizeof where, "bank %d", rom.bank);
    else std::snprintf(where, sizeof where, "%s &%04X%s%s", rom.me1 ? "ME1" : "ME0", rom.address,
                       rom.pv < 0 ? "" : rom.pv ? " PV=1" : " PV=0", rom.pu < 0 ? "" : rom.pu ? " PU=1" : " PU=0");
    return "bus-rom: " + rom.path + " at " + where;
}

} // namespace preset_bus_rom
