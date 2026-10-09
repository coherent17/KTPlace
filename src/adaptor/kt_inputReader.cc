// @file kt_inputReader.cc
// Choosing an input adapter for a directory

#include "adaptor/kt_inputReader.h"

#include "adaptor/bookshelfToKTAdaptor.h"
#include "adaptor/lefdefToKTAdaptor.h"

#include <vector>

namespace ktplace {

std::unique_ptr<InputReader> makeInputReader(const std::string &dirPath) {
    // Order does not matter: the formats are recognised by disjoint extensions.
    // Adding one is a single entry here.
    std::vector<std::unique_ptr<InputReader>> candidates;
    candidates.push_back(std::make_unique<BookshelfInputAdapter>());
    candidates.push_back(std::make_unique<LefDefInputAdapter>());
    for (std::unique_ptr<InputReader> &candidate : candidates) {
        if (candidate->recognises(dirPath)) {
            return std::move(candidate);
        }
    }
    return nullptr;
}

}  // namespace ktplace
