#include "BasicPointerTable.hpp"

namespace CoreDebug {

using W = BasicPointerEntry::Width;

// PC-1500 BASIC/system-variable pointer table, used by the debug panel's
// "Dump Pointers" view. Addresses per the ROM disassembly's PC-1500.lib.
const BasicPointerEntry kPC1500BasicPointers[] = {
    {"BASPRG_ST",   0x7865, W::Word, "BASIC program start"},
    {"BASPRG_END",  0x7867, W::Word, "BASIC program end (used by MEM)"},
    {"BASPRG_EDT",  0x7869, W::Word, "Editor line-modification pointer"},
    {"CURVARADD",   0x7883, W::Word, "Currently accessed variable"},
    {"CURVARTYPE",  0x7885, W::Byte, "Active variable's type"},
    {"VAR_START",   0x7899, W::Word, "Dimensioned-variable area boundary (grows downward)"},
    {"DATA_PTR",    0x78BE, W::Word, "DATA statement cursor"},
    {"CURR_LINE",   0x789C, W::Word, "Currently executing BASIC line number"},
    {"PREV_LINE",   0x78A2, W::Word, "Previous line number (error reporting / CONT)"},
    {"TRACE_ON",    0x788D, W::Byte, "Non-zero if TRON is active"},
    {"TRACE_PARAM", 0x788E, W::Word, "Trace output vector"},
    {"RAM_ST",      0x7863, W::Byte, "High byte of user RAM start"},
    {"RAM_END",     0x7864, W::Byte, "High byte of first invalid page (one past top of RAM)"},
    {"WARM_START",  0x7A20, W::Byte, "Must be $01 to skip NEW0? cold start"},
    {"STK_SAVE",    0x7A21, W::Word, "System stack pointer, saved for warm start"},
    {"DISP_CTRL",   0x7880, W::Byte, "LCD refresh / auto-off flags"},
    {"BREAK_STAT",  0x7881, W::Byte, "BREAK-key/execution-pause status"},
    {"IN_BUF_PTR",  0x788B, W::Byte, "Low byte of the cursor into the $7Bxx input buffer"},
    {"ON_ERR_VEC",  0x78B8, W::Word, "ON ERROR GOTO handler address"},
    {"SRCH_PTR",    0x78A6, W::Word, "Program/variable scan workspace"},
    {"STK_FOR_GSB", 0x7882, W::Byte, "FOR/NEXT + GOSUB stack depth"},
    {"LOCK",        0x79FF, W::Byte, "Lock register; unlocked with NEW0 or UNLOCK ROM routine"},
};
const int kPC1500BasicPointerCount = sizeof(kPC1500BasicPointers) / sizeof(kPC1500BasicPointers[0]);
const int kPC1500BasPrgEndIndex = 1;  // "BASPRG_END"
const int kPC1500RamEndIndex = 12;    // "RAM_END"
const int kPC1500LockIndex = 21;      // "LOCK"
const int kPC1500BasicPointerMaxNameLength = 11;  // "TRACE_PARAM" / "STK_FOR_GSB"

using V = PC1600PointerEntry::Value;

// PC-1600 BASIC/system-variable pointer table, used by the debug panel's
// "Dump Pointers" view. The BASIC pointers are the PC-1500's at F800H+ and
// describe the S0 area only (the S1/S2 program modules keep theirs in the
// slot descriptors, see readPC1600ProgramAreas()).
const PC1600PointerEntry kPC1600Pointers[] = {
    {"BASPRG_ST",    0xF865, V::AddrBE, "S0 program start (bank F02BH)"},
    {"BASPRG_END",   0xF867, V::AddrBE, "S0 program end, the FFH mark (bank F02CH)"},
    {"BASPRG_EDT",   0xF869, V::AddrBE, "Edit / merge head"},
    {"RAM_END",      0xF864, V::Byte,   "Page of the S0 user-area top (MEM, VARIABLE_PTR reset)"},
    {"VARIABLE_PTR", 0xF899, V::AddrBE, "Start of the variables (they grow down)"},
    {"CURRENT_TOP",  0xF89E, V::AddrBE, "Start of the current line's block (bank F1C1H)"},
    {"CURRENT_LINE", 0xF89C, V::WordBE, "Current program line number"},
    {"SEARCH_LINE",  0xF8A8, V::WordBE, "Line after SEARCH hit / last-entered line"},
    {"ERL",          0xF89B, V::Byte,   "Last error number"},
    {"BREAK_ADDR",   0xF8AC, V::AddrBE, "Address of last BREAK"},
    {"ERROR_ADDR",   0xF8B2, V::AddrBE, "Address of last error"},
    {"ON_ERR_ADDR",  0xF8B8, V::AddrBE, "ON ERROR GOTO target (stored bit 15 set: off)"},
    {"FBNO",         0xF02D, V::Byte,   "MAXFILES value"},
    {"FBBP",         0xF04C, V::WordLE, "Communication-buffer start"},
    {"FCBPTR",       0xF04E, V::WordLE, "FCB / file-buffer start"},
    {"DSPLPTR",      0xF05C, V::Byte,   "LCD display start line"},
    {"LCDWK1",       0xF05D, V::Byte,   "LCD work 1 (charset / cursor flags)"},
    {"CRSRX",        0xF060, V::Byte,   "cursor column"},
    {"CRSRY",        0xF05F, V::Byte,   "cursor row"},
    {"KEYWK1",       0xF079, V::Byte,   "key work 1 (click / repeat flags)"},
};
const int kPC1600PointerCount = sizeof(kPC1600Pointers) / sizeof(kPC1600Pointers[0]);
const int kPC1600PointerMaxNameLength = 12;  // "CURRENT_LINE" / "VARIABLE_PTR"

PC1600ProgramAreas readPC1600ProgramAreas(const std::function<uint8_t(uint16_t)>& peek) {
    PC1600ProgramAreas a;
    a.title = peek(0xF1D5);
    a.startIndex = peek(0xF02B);
    a.endIndex = peek(0xF02C);
    a.ramEndPage = peek(0xF864);
    // MEM (LH5803 $CC30): RAM_END:00 - (BASPRG_END + 1) + (5 - F02CH) * 4000H,
    // all in the stored (LH5803-view) representation.
    const int s0End = (peek(0xF867) << 8) | peek(0xF868);
    a.memS0 = (a.ramEndPage << 8) - (s0End + 1) + (5 - a.endIndex) * 0x4000;
    for (int i = 0; i < 2; ++i) {
        PC1600SlotProgramArea& s = a.slot[i];
        static_cast<pc1600::SlotDescriptor&>(s) = pc1600::readSlotDescriptor(peek, i + 1);
        // STATUS 259 / 260 (LH5803 $CE41): limit:00 - end + (SxMBb - end bank) * 4000H.
        if (s.programModule())
            s.freeBytes = (s.limitPage << 8) - s.end + (s.limitIndex - s.endIndex) * 0x4000;
    }
    return a;
}

}  // namespace CoreDebug
