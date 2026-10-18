#pragma once

namespace ktplace {

class FlowMgr;

void bindTclCommands(FlowMgr &flow);
void unbindTclCommands(const FlowMgr &flow);

}  // namespace ktplace
