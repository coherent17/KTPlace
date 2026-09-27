/**
 * @file bookshelfToKTAdaptor.h
 * @brief Bookshelf format adapter using Adapter pattern
 */

#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include "datamodel/kt_dm.h"

namespace ktplace {

// Forward declaration of base adapter interface
class InputAdapter;

/**
 * @brief Adapter for Bookshelf format input files
 * 
 * Parses Bookshelf format files (.nodes, .nets, .pl, .scl, .wts) and
 * converts them into the internal PlacementDB format.
 * 
 * Uses the Adapter pattern to convert from Bookshelf format to unified data model.
 */
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

    /**
     * @brief Read Bookshelf format from directory
     * @param baseName Base name of the files (e.g., "adaptec2" for adaptec2.nodes, adaptec2.nets, etc.)
     * @param dirPath Directory path containing the files
     * @return true if successful, false otherwise
     */
    [[nodiscard]] bool readFromDirectory(const std::string &baseName, const std::string &dirPath);

    /**
     * @brief Read Bookshelf format from individual files
     * @param nodesFile Path to .nodes file (can be gzipped)
     * @param netsFile Path to .nets file (can be gzipped)
     * @param plFile Path to .pl file (optional, can be gzipped)
     * @param sclFile Path to .scl file (optional, can be gzipped)
     * @param wtsFile Path to .wts file (optional, can be gzipped)
     * @return true if successful, false otherwise
     */
    [[nodiscard]] bool readFromFiles(const std::string &nodesFile, const std::string &netsFile,
                                     const std::string &plFile = "",
                                     const std::string &sclFile = "",
                                     const std::string &wtsFile = "");

    /**
     * @brief Get the PlacementDB object
     * @return Reference to the internal PlacementDB
     */
    [[nodiscard]] PlacementDB &getPlacementDB() {
        return *db;
    }
    [[nodiscard]] const PlacementDB &getPlacementDB() const {
        return *db;
    }

    /**
     * @brief Release ownership of the PlacementDB
     * @return Unique pointer to the PlacementDB
     */
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
