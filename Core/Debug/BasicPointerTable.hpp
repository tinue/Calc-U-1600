#pragma once
#include <cstdint>
#include <functional>

// ── Shared debug-pointer tables ──────────────────────────────────────────
//
// Plain data consumed by the GUI's debug panel (Qt6's DebugPanel): the
// address/description tables for the BASIC/system-variable pointers each
// model exposes, kept in one place rather than duplicated per platform.
namespace CoreDebug {

struct BasicPointerEntry {
    const char* name;
    uint16_t address;
    enum class Width : uint8_t { Byte, Word } width;
    const char* description;
};

extern const BasicPointerEntry kPC1500BasicPointers[];
extern const int kPC1500BasicPointerCount;
// Indices into kPC1500BasicPointers for the two entries debugDumpPointers()'s
// MEM computation needs by name lookup elsewhere -- kept as plain constants
// here (not a runtime search) since the table is fixed at compile time.
extern const int kPC1500BasPrgEndIndex;
extern const int kPC1500RamEndIndex;
// The LOCK register: the dump appends "(unlocked)" to it when it reads $FF.
extern const int kPC1500LockIndex;
extern const int kPC1500BasicPointerMaxNameLength;

struct PC1600PointerEntry {
    const char* name;
    uint16_t address;
    // AddrBE: a BASIC pointer as the ROM stores it -- high byte first, bit 15
    // inverted (the PC-1500 / LH5803 convention); pc1600PointerToZ80() gives
    // the Z-80 address. WordBE: a plain big-endian number (line numbers).
    // WordLE: a Z-80-native little-endian word.
    enum class Value : uint8_t { Byte, AddrBE, WordBE, WordLE } value;
    const char* note;
};

extern const PC1600PointerEntry kPC1600Pointers[];
extern const int kPC1600PointerCount;
extern const int kPC1600PointerMaxNameLength;

// The Z-80 address of a stored BASIC pointer (F865, F867, F899, ...): bit 15
// is stored inverted, e.g. $40C5 = Z-80 C0C5, $00C5 = 80C5 in a module bank.
constexpr uint16_t pc1600PointerToZ80(uint16_t stored) { return stored ^ 0x8000; }

// ── PC-1600 program areas (S0, and S1/S2 program modules) ────────────────
//
// What the ROM itself reports, computed the ROM's way so the Debug panel
// agrees with MEM / STATUS 259 / STATUS 260. Sources: SharpPC1500Reference
// PC-1600-Work-Area-Map.md §3.5 and §4.5 (LH5803 ROM $CC30 / $CE41).
struct PC1600SlotProgramArea {
    bool programModule = false;  // SxMTb b7 clear
    uint8_t mtb = 0;             // SxMTb: first ADTBL index (FEH folded into S0, FFH none)
    uint8_t basePage = 0;        // Z-80 page of the module window base
    uint8_t limitPage = 0;       // area limit (Z-80 page; C0H = top of the window)
    uint8_t limitIndex = 0;      // SxMBb: ADTBL index of the limit
    uint16_t start = 0;          // program start, Z-80 page-2 address
    uint8_t startIndex = 0;
    uint16_t end = 0;            // program end (the FFH mark), Z-80 page-2 address
    uint8_t endIndex = 0;
    int freeBytes = 0;           // STATUS 259 / 260
};

struct PC1600ProgramAreas {
    uint8_t title = 0;           // F1D5H: 0 = S0, 1 = S1, 2 = S2
    uint8_t startIndex = 0;      // F02BH: ADTBL index of the S0 program start
    uint8_t endIndex = 0;        // F02CH: ADTBL index of the S0 program end
    uint8_t ramEndPage = 0;      // F864H
    int memS0 = 0;               // MEM / STATUS 0 (always S0, whatever TITLE says)
    PC1600SlotProgramArea slot[2];  // S1, S2
};

PC1600ProgramAreas readPC1600ProgramAreas(const std::function<uint8_t(uint16_t)>& peek);

}  // namespace CoreDebug
