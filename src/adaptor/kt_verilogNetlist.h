// @file kt_verilogNetlist.h
// The nets of a flat structural Verilog netlist, for a DEF that leaves out its NETS.

#pragma once

#include <string>
#include <vector>

namespace ktplace {

struct VerilogPin {
    std::string inst;
    std::string pin;
};

struct VerilogNet {
    std::string name;
    bool port = false;
    std::vector<VerilogPin> pins;
};

[[nodiscard]] std::vector<VerilogNet> readVerilogNetlist(const std::string &path);

}  // namespace ktplace
