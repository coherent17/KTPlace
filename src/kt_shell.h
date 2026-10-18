#pragma once

#include <istream>

namespace ktplace {

class kt_option;

int runShell(const kt_option &options, std::istream &in);

}  // namespace ktplace
