// @file bookshelfToKTAdaptor.h// Bookshelf format adapter using Adapter pattern


#pragma once

#include "datamodel/kt_dm.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ktplace {

// Forward declaration of base adapter interface

// Adapter for Bookshelf format input files// Parses Bookshelf format files (.nodes, .nets, .pl, .scl, .wts) and// converts them into the internal PlacementDB format.// Uses the Adapter pattern to convert from Bookshelf format to unified data model.

class BookshelfInputAdapter {
public:
    /// Constructor
    explicit BookshelfInputAdapter(std::unique_ptr<PlacementDB> db = nullptr);

    /// Destructor
    ~BookshelfInputAdapter();

    // Copy semantics (deleted)
    BookshelfInputAdapter(const BookshelfInputAdapter &) = delete;
    BookshelfInputAdapter &operator=(const BookshelfInputAdapter &) = delete;

    // Move semantics
    BookshelfInputAdapter(BookshelfInputAdapter &&) noexcept;
    BookshelfInputAdapter &operator=(BookshelfInputAdapter &&) noexcept;

    // Read Bookshelf format from directory

    [[nodiscard]] bool readFromDirectory(const std::string &dirPath);

    // Read Bookshelf format from individual files

    [[nodiscard]] bool readFromFiles(const std::string &nodesFile, const std::string &netsFile,
                                     const std::string &plFile = "",
                                     const std::string &sclFile = "",
                                     const std::string &wtsFile = "");

    // Get the PlacementDB object

    [[nodiscard]] PlacementDB &getPlacementDB() {
        return *db;
    }
    [[nodiscard]] const PlacementDB &getPlacementDB() const {
        return *db;
    }

    // Release ownership of the PlacementDB

    [[nodiscard]] std::unique_ptr<PlacementDB> releasePlacementDB() {
        return std::move(db);
    }

private:
    // Helper methods for parsing individual files
    bool parseNodesFile(const std::string &filePath);
    bool parseNetsFile(const std::string &filePath);
    bool parsePlacementFile(const std::string &filePath);
    bool parseSclFile(const std::string &filePath);
    bool parseWtsFile(const std::string &filePath);

    // Helper methods for parsing lines
    bool parsePlacementLine(const std::string &line);
    bool parseSclRow(const std::vector<std::string> &tokens);

    // Utility methods
    std::string stripComments(const std::string &line);
    std::vector<std::string> tokenize(const std::string &line);

    // Internal state
    std::unique_ptr<PlacementDB> db;
    std::unordered_map<std::string, double> netWeights;  // From .wts file
};

}  // namespace ktplace
