#pragma once
#include <initializer_list>
#include <memory>
#include <string>

#include "CE155Card.hpp"
#include "CE1638PlusCard.hpp"
#include "CE163FCard.hpp"
#include "ExpansionCard.hpp"
#include "PlainRamCard.hpp"

// The single source of truth for which modules a PC-1600 memory slot
// accepts, and what card each name builds. Shared by the `.pc1600` preset
// parser (which validates names against `kSlotModuleNames`), the preset
// loader, and the Bridge -- so adding a module type is one edit here.
//
// The same card objects the PC-1500 `memory-expansion:` path builds: a card
// decodes purely from its physical edge-connector pins and never learns
// which host or slot it sits in (see ExpansionCard.hpp), so it plugs into a
// PC-1600 MemorySlotConnector pin-for-pin, exactly as CE155Card documents.
// Whatever address window it ends up answering at is a consequence of what
// that connector drives onto the pins -- the CE-1638+/CE-163F banked-window
// index is masked to the bank size for precisely this reason (their Y0
// window is &0000-&3FFF on a PC-1500 but &8000-&BFFF on a PC-1600 slot).
inline constexpr std::initializer_list<const char*> kSlotModuleNames = {
    "ce155",      // the 8KB CE-155
    "ram16",      // generic unbanked 16KB RAM chip
    "ram32",      // generic unbanked 32KB RAM chip
    "ce1638plus", // throwaway 128KB / 8-bank RAM proof-of-concept
    "ce163f",     // throwaway 256KB / 16-bank (8 RAM + 8 FLASH) proof-of-concept
};

/// Builds the card for `name`, or nullptr if it isn't one of
/// `kSlotModuleNames`.
inline std::unique_ptr<ExpansionCard> makeSlotModuleCard(const std::string& name) {
    if (name == "ce155") return std::make_unique<CE155Card>();
    if (name == "ram16") return std::make_unique<PlainRamCard>(0x4000);
    if (name == "ram32") return std::make_unique<PlainRamCard>(0x8000);
    if (name == "ce1638plus") return std::make_unique<CE1638PlusCard>();
    if (name == "ce163f") return std::make_unique<CE163FCard>();
    return nullptr;
}
