#pragma once

#include <string>
#include <vector>

namespace ktplace {

class FlowMgr;

struct CommandHelp {
    std::string name;
    std::string usage;
    std::string summary;
};

void bindTclCommands(FlowMgr &flow);
void unbindTclCommands(const FlowMgr &flow);

[[nodiscard]] const std::vector<CommandHelp> &commandHelp();
[[nodiscard]] std::string helpText(const std::string &command = {});

}  // namespace ktplace
