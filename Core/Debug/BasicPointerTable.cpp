#include "BasicPointerTable.hpp"

namespace CoreDebug {

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
