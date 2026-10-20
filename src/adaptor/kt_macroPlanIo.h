// @file kt_macroPlanIo.h
// The ICCAD 2022 Problem D macro files: the .mlist and .txt it reads, the .dmp it writes.

#pragma once

#include "datamodel/kt_macroPlan.h"

#include <string>

namespace ktplace {

class ktDM;

[[nodiscard]] MacroPlan readMacroPlan(const ktDM &db, const std::string &mlistPath,
                                      const std::string &rulesPath);

void writeMacroPlan(const ktDM &db, const std::string &path);

}  // namespace ktplace
