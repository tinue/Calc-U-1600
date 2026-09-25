#pragma once
#include <string>
#include <vector>

#include "../Resources/BundledRomCatalog.hpp"
#include "PresetRunner.hpp"

/// A preset's `interface:` key: attaches the CE-158 (both models share
/// the same `attachCE158(bytes, size)` shape) before the boot, like the
/// plotter. Returns false with `result->error` set; nothing to do (true)
/// for no interface.
template <class Machine>
bool attachPresetInterface(Machine& machine, const std::string& interfaceName,
                           const std::vector<std::string>& romDirs, const PresetLogFn& log,
                           PresetLoadResult* result) {
    if (interfaceName != "ce158") return true;
    if (!BundledRoms::attachCE158(machine, romDirs, &result->error)) {
        if (log) log(result->error);
        return false;
    }
    result->ce158Attached = true;
    if (log) log("interface: CE-158 attached");
    return true;
}
