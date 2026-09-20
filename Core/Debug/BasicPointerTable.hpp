#pragma once
#include <cstdint>

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
    enum class Value : uint8_t { Byte, WordBE, WordLE } value;
    const char* note;
};

extern const PC1600PointerEntry kPC1600Pointers[];
extern const int kPC1600PointerCount;
extern const int kPC1600BasPrgEndIndex;
extern const int kPC1600PointerMaxNameLength;
constexpr uint16_t kPC1600WorkAreaBase = 0xF000;

}  // namespace CoreDebug
