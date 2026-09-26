/**
 * @file lefdefToKTAdaptor.h
 * @brief LEF/DEF format adapter using the Adapter pattern
 * 
 * Parses industry-standard LEF (physical library) and DEF (design) files
 * and converts them into the internal PlacementDB.  Supports the ISPD /
 * ICCAD placement-contest style inputs (floorplan.def + cells.lef +
 * tech.lef + design.v), where every standard cell is "UNPLACED", macros
 * and I/O pads are placed/fixed, and net connectivity comes from the
 * DEF NETS section.
 */

#ifndef LEFDEF_TO_KT_ADAPTOR_H
#define LEFDEF_TO_KT_ADAPTOR_H

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include "datamodel/kt_dm.h"

namespace ktplace {
namespace io {

/**
 * @brief Adapter for LEF/DEF format input files
 * 
 *  - cells.lef            -> cell sizes, pin locations/directions (micron)
 *  - tech.lef             -> ignored (no MACROs)
 *  - floorplan.def        -> die area, rows, placed/fixed macros, I/O pads,
 *                            and the flat component netlist
 * 
 * DEF coordinate units (UNITS DISTANCE MICRONS) are respected: LEF sizes,
 * which are in microns, are scaled into the DEF coordinate frame.
 */
class LefDefInputAdapter {
public:
    /// Constructor
    explicit LefDefInputAdapter(std::unique_ptr<core::PlacementDB> db = nullptr);

    /// Destructor
    ~LefDefInputAdapter();

    // Copy semantics (deleted)
    LefDefInputAdapter(const LefDefInputAdapter &) = delete;
    LefDefInputAdapter &operator=(const LefDefInputAdapter &) = delete;

    // Move semantics
    LefDefInputAdapter(LefDefInputAdapter &&) noexcept;
    LefDefInputAdapter &operator=(LefDefInputAdapter &&) noexcept;

    /**
     * @brief Auto-detect and read the LEF/DEF files from a directory
     * @param dirPath Directory containing floorplan.def/cells.lef/etc.
     * @return true if successful, false otherwise
     */
    [[nodiscard]] bool readFromDirectory(const std::string &dirPath);

    /**
     * @brief Read LEF/DEF format from explicit files
     * @param defFile Path to the .def file (can be gzipped)
     * @param lefFiles Paths to .lef library files (can be gzipped)
     * @return true if successful, false otherwise
     */
    [[nodiscard]] bool readFromFiles(const std::string &defFile,
                                     const std::vector<std::string> &lefFiles);

    /**
     * @brief Get the PlacementDB object
     */
    [[nodiscard]] core::PlacementDB &getPlacementDB() {
        return *db;
    }
    [[nodiscard]] const core::PlacementDB &getPlacementDB() const {
        return *db;
    }

    /**
     * @brief Release ownership of the PlacementDB
     */
    [[nodiscard]] std::unique_ptr<core::PlacementDB> releasePlacementDB() {
        return std::move(db);
    }

private:
    // Per-macro record gathered from the LEF MACRO blocks.
    struct MacroRec {
        double widthMicrons = 1.0;
        double heightMicrons = 1.0;
        bool isBlock = false;
        std::unordered_map<std::string, bool> pinIsInput;  // pin name -> isInput
        std::unordered_map<std::string, std::pair<double, double>>
            pinOffset;  // pin -> (x,y) microns
        std::unordered_map<std::string, bool> pinExists;
    };

    // A single pin reference inside a DEF net: either an instance pin
    // (inst != "") or an I/O pad (ioPin != "").
    struct NetPinRef {
        std::string inst;   // instance name (sanitised)
        std::string pin;    // cell pin name
        std::string ioPin;  // I/O pad name when this is a pin-level ref
    };

    bool parseLefFile(const std::string &filePath);
    bool parseDefFile(const std::string &filePath);
    void peekDefUnits(const std::string &filePath);

    std::vector<std::string> static tokenize(const std::string &line);
    std::string static sanitizeName(const std::string &name);

    // Internal state
    std::unique_ptr<core::PlacementDB> db;
    std::unordered_map<std::string, MacroRec> macros;        // LEF macro -> record
    std::unordered_map<std::string, std::string> instMacro;  // DEF inst -> macro
    double unitsPerMicron = 1.0;                             // DEF UNITS DISTANCE MICRONS
    double siteWidthMicrons = 0.0;                           // LEF SITE core size (micron)
    double siteHeightMicrons = 0.0;
};

}  // namespace io
}  // namespace ktplace

#endif  // LEFDEF_TO_KT_ADAPTOR_H