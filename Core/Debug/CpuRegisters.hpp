#pragma once

#include <string>
#include <vector>

#include "DebugTarget.hpp"

class LH5801;
class SC7852;
struct LH5801HistoryFrame;
struct Z80HistoryFrame;

// Register tables of the two CPU families, shared by the machine debug
// targets: the display list, name lookup (incl. 8-bit halves and flags)
// and history-frame conversion. Names are lower case.
namespace debug {

std::vector<Register> lhRegisters(const LH5801& cpu);
bool lhReadRegister(const LH5801& cpu, const std::string& name, uint32_t* value);
bool lhWriteRegister(LH5801& cpu, const std::string& name, uint32_t value);
HistoryEntry lhHistoryEntry(const LH5801HistoryFrame& f);

std::vector<Register> z80Registers(const SC7852& cpu);
bool z80ReadRegister(const SC7852& cpu, const std::string& name, uint32_t* value);
bool z80WriteRegister(SC7852& cpu, const std::string& name, uint32_t value);
HistoryEntry z80HistoryEntry(const Z80HistoryFrame& f);

} // namespace debug
