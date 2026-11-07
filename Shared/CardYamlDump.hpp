#pragma once
#include <string>
#include <vector>

namespace shared {

class AppSession;

// The INSPECTOR's Card YAML: the RAM of the module in each slot as an
// `initial-content:` block for a card file, each under a header line naming
// the module, its slot and its banks. One chunk per line group; a single
// message when no slot holds a card with dumpable RAM.
std::vector<std::string> cardYamlDump(AppSession& app);

} // namespace shared
