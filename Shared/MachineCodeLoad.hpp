#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "MachineCodeFile.hpp"
#include "Shared/AppSession.hpp"

namespace shared {

// A cassette WAV's files to pick from, or the one image to load.
struct ProgramChoice {
    std::vector<std::vector<std::uint8_t>> images;  // one per file, tape order
    std::vector<std::string> names;                 // "1. HELLO", "2. (no name)"; empty for a single image
    bool needsChoice() const { return images.size() > 1; }
};

// Load BASIC Program… / Load Machine Code… on `bytes`: a WAV holding
// several files lists them ("Load which one?"), one holding a single file
// gives its image; anything else (or a WAV that doesn't decode -- the
// loader then says why) is the image itself.
ProgramChoice programChoice(const std::vector<std::uint8_t>& bytes);

// ── MachineCodeLoad: one Load Machine Code… from file to advice ─────────
//
// 1. plan() on the file's image (after programChoice()) against the live
//    machine: refused (error()), ready, or asking for a start address.
// 2. needsAddress(): the frontend asks, checking each typed text with
//    setAddress() (hint, valid) and enabling Load on `valid`.
// 3. AppSession::loadMachineCode(request()).
// 4. loaded(): what to show (where it went, NEW / CALL); the frontend then
//    types the CALL (MachineSession::pasteText) for the user to ENTER.
class MachineCodeLoad {
public:
    static MachineCodeLoad plan(const MachineSession& session, const std::vector<std::uint8_t>& image);

    const std::string& error() const { return m_plan.error; }  // non-empty: refuse
    bool needsAddress() const { return m_plan.needsAddress; }
    std::size_t length() const { return m_request.payload.size(); }
    // The address field's text to start with ("&C0C5"; empty for none), its
    // placeholder, and (PC-1600) which CPU's address space it is in.
    std::string defaultAddressText() const;
    std::string addressPlaceholder() const;
    std::string cpuNote() const;  // empty on a PC-1500

    struct AddressCheck {
        bool valid = false;
        std::string hint;  // a refusal, a warning, or where it goes; empty: nothing to say
    };
    // A typed start address (hex, `&C000` / `$C000` / `0xC000`); a valid
    // one becomes the load target.
    AddressCheck setAddress(const std::string& text);

    // For AppSession::loadMachineCode(); valid once there is an address.
    const MachineCodeLoadRequest& request() const { return m_request; }

    struct Loaded {
        std::string summary;  // "Loaded 42 bytes at &C0C5–&C0EE (S0)"
        machinecode::Advice advice;
    };
    // After a successful load: reads the live machine's RAM bounds.
    Loaded loaded(const MachineSession& session) const;

private:
    machinecode::Target m_target = machinecode::Target::PC1500;
    machinecode::File m_code;  // its payload moved into m_request
    machinecode::PC1600State m_state;
    machinecode::Plan m_plan;
    std::uint32_t m_addr = 0;  // in m_plan.cpu's address space
    MachineCodeLoadRequest m_request;  // the slot is a machinecode::Slot

    machinecode::Slot slot() const { return static_cast<machinecode::Slot>(m_request.slot); }
};

} // namespace shared
