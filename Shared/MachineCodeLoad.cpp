#include "Shared/MachineCodeLoad.hpp"

#include <cstdio>

#include "PC1500/PC1500MachineCodeLoader.hpp"
#include "PC1600/PC1600MachineCodeLoader.hpp"
#include "PC1600/PC1600ProgramPlacement.hpp"
#include "ProgramFile.hpp"

namespace shared {

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "&%X", static_cast<unsigned>(v));
    return buf;
}

} // namespace

ProgramChoice programChoice(const std::vector<std::uint8_t>& bytes) {
    ProgramChoice choice;
    // A tape that can't be read stays a WAV: the loader says why.
    std::vector<programfile::TapeFile> files = programfile::tapeFiles(bytes, nullptr);
    if (files.empty()) {
        choice.images.push_back(bytes);
        return choice;
    }
    if (files.size() > 1)
        for (std::size_t i = 0; i < files.size(); ++i)
            choice.names.push_back(std::to_string(i + 1) + ". " +
                                   (files[i].name.empty() ? std::string("(no name)") : files[i].name));
    for (auto& file : files) choice.images.push_back(std::move(file.image));
    return choice;
}

MachineCodeLoad MachineCodeLoad::plan(const MachineSession& session, const std::vector<std::uint8_t>& image) {
    MachineCodeLoad load;
    load.m_code = machinecode::readFile(image);
    const bool isPC1600 = session.currentModel() == Model::PC1600;
    load.m_target = isPC1600 ? machinecode::Target::PC1600 : machinecode::Target::PC1500;
    // On the PC-1600 the machine's MODE and TITLE decide the address space
    // and the target (docs/background/plans/Loader-Mode-Plan.md); the loader
    // never changes them.
    if (isPC1600 && session.pc1600()) load.m_state = pc1600LoadState(*session.pc1600());
    const machinecode::Plan plan = machinecode::plan(load.m_target, load.m_code, load.m_state);
    load.m_error = plan.error;
    if (!plan.error.empty()) return load;

    load.m_cpu = plan.cpu;
    load.m_needsAddress = plan.needsAddress;
    load.m_defaultAddr = plan.defaultAddr;
    load.m_request.payload = load.m_code.payload;
    // In the code's CPU's address space (a header's bank is plan.bank).
    load.m_addr = load.m_code.loadAddr & 0xFFFF;
    load.m_slot = plan.slot;
    load.m_request.addr = plan.busAddr;
    load.m_request.slot = static_cast<int>(plan.slot);
    load.m_request.bank = plan.bank;  // the header's bank, else the slot's
    return load;
}

std::string MachineCodeLoad::defaultAddressText() const { return m_defaultAddr != 0 ? hex(m_defaultAddr) : ""; }

std::string MachineCodeLoad::addressPlaceholder() const {
    // An example from the target's own map: the start of BASIC's program
    // area on the PC-1600 (in the MODE's address space), the classic
    // machine-code spot on the PC-1500.
    if (m_target != machinecode::Target::PC1600) return "hex, e.g. &7C01";
    return m_cpu == machinecode::Cpu::LH5803 ? "hex, e.g. &40C5" : "hex, e.g. &C0C5";
}

std::string MachineCodeLoad::cpuNote() const {
    if (m_target != machinecode::Target::PC1600) return {};
    return m_cpu == machinecode::Cpu::LH5803
               ? "MODE 1: LH5801 code is assumed. The address is in the PC-1500 (LH5803) address space, "
                 "where &4000-&7FFF is the internal RAM."
               : "MODE 0: Z-80 code is assumed. The address is a Z-80 address.";
}

MachineCodeLoad::AddressCheck MachineCodeLoad::setAddress(const std::string& text) {
    AddressCheck check;
    std::uint32_t addr = 0;
    const std::size_t len = length();
    if (!machinecode::parseHexAddress(text, &addr)) {
        if (text.find_first_not_of(" \t") != std::string::npos) check.hint = "Not a hex address (&0000-&FFFF).";
        return check;
    }
    if (m_target == machinecode::Target::PC1600) {
        const bool lh5803 = m_cpu == machinecode::Cpu::LH5803;
        if (lh5803 && addr + len > 0x8000) {
            check.hint = "The LH5803's RAM is &0000-&7FFF.";
            return check;
        }
        const std::uint32_t busAddr = lh5803 ? pc1600::lh5803ToZ80(static_cast<std::uint16_t>(addr)) : addr;
        std::string why;
        machinecode::Slot slot = machinecode::Slot::S0;
        if (!machinecode::pc1600TargetFor(busAddr, len, m_state, &slot, &why, m_cpu)) {
            check.hint = why;
            return check;
        }
        const std::string work = machinecode::pc1600WorkAreaWarning(busAddr, len, m_cpu);
        if (!work.empty()) {
            check.hint = work;  // a warning; loading stays possible
        } else if (slot != machinecode::Slot::S0) {
            const std::string n = slot == machinecode::Slot::S1 ? "1" : "2";
            check.hint = m_state.title != 0 ? "Goes into the slot " + n + " program module, the selected program area."
                                            : "Goes into the slot " + n + " module, where BASIC's program area starts.";
        }
        m_slot = slot;
        m_request.addr = busAddr;
        m_request.slot = static_cast<int>(slot);
        m_request.bank = machinecode::pc1600DefaultBank(slot);
    } else {
        if (static_cast<std::uint64_t>(addr) + len > 0x10000) {
            check.hint = "The code runs past &FFFF.";
            return check;
        }
        m_request.addr = addr;
    }
    m_addr = addr;
    check.valid = true;
    return check;
}

MachineCodeLoad::Loaded MachineCodeLoad::loaded(const MachineSession& session) const {
    Loaded out;
    const bool isPC1600 = m_target == machinecode::Target::PC1600;
    // The advice: NEW that keeps BASIC off the code, CALL that starts it.
    std::uint32_t ramStart = 0, ramEnd = 0;
    if (!isPC1600 && session.pc1500()) pc1500UserRam(*session.pc1500(), &ramStart, &ramEnd);
    const std::size_t len = length();
    out.advice = machinecode::advice(m_target, m_slot, m_request.bank, m_addr, len, m_code.autorunAddr, ramStart,
                                     ramEnd, m_state, m_cpu);

    const std::string slot = machinecode::slotName(m_slot);
    out.summary = "Loaded " + std::to_string(len) + " bytes at " + hex(m_addr) + "–" +
                  hex(m_addr + static_cast<std::uint32_t>(len) - 1);
    if (isPC1600 && m_cpu == machinecode::Cpu::LH5803)
        out.summary += " (LH5803 addresses; Z-80 " + hex(m_request.addr) + ", " + slot + ")";
    else if (isPC1600 && m_request.bank != 0)
        out.summary += " (" + slot + ", bank " + std::to_string(m_request.bank) + ")";
    else if (isPC1600)
        out.summary += " (" + slot + ")";
    return out;
}

} // namespace shared
