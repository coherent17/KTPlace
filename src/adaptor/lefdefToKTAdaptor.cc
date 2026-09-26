/**
 * @file lefdefToKTAdaptor.cc
 * @brief Implementation of the LEF/DEF format adapter
 *
 * Parses the contest-style LEF/DEF benchmark format:
 *   - LEF MACRO blocks provide cell dimensions (microns) and pin
 *     locations/directions.
 *   - The DEF file provides the die area, row sites, the instance list
 *     (components) with their placement status, the I/O pads (pins), and
 *     the flat signal netlist (nets).
 *
 * DEF coordinates already use the DEF unit scale (UNITS DISTANCE MICRONS);
 * LEF dimensions, which are in microns, are scaled by that factor so both
 * live in the same coordinate frame.
 */

#include "adaptor/lefdefToKTAdaptor.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <filesystem>

// Boost.Iostreams - transparent gzip input
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/device/file.hpp>

namespace ktplace {
namespace io {

namespace {

// Gzip-aware text file. If the path ends in ".gz" the stream is decompressed
// on the fly with Boost.Iostreams; otherwise it is read as plain text.
class InputTextFile {
public:
    explicit InputTextFile(const std::string &path) {
        const bool gz = path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
        if (gz) {
            in.push(boost::iostreams::gzip_decompressor());
            in.push(boost::iostreams::file_source(path, std::ios::binary));
        } else {
            in.push(boost::iostreams::file_source(path));
        }
    }

    std::istream &stream() {
        return in;
    }
    explicit operator bool() const {
        return static_cast<bool>(in);
    }

private:
    boost::iostreams::filtering_istream in;
};

// Try to parse a double, returning false on failure (avoids stod exceptions
// hitting the caller).
bool tryDouble(const std::string &s, double &out) {
    try {
        std::size_t used = 0;
        out = std::stod(s, &used);
        return used > 0;
    } catch (const std::exception &) {
        return false;
    }
}

}  // namespace

std::vector<std::string> LefDefInputAdapter::tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

std::string LefDefInputAdapter::sanitizeName(const std::string &name) {
    std::string out = name;
    if (!out.empty() && out[0] == '\\') {
        out.erase(0, 1);  // DEF escape prefix
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c == '/' || c == '.' || c == '[' || c == ']' || c == '\\' || c == '(' || c == ')' ||
            c == '{' || c == '}') {
            out[i] = '_';
        }
    }
    return out;
}

LefDefInputAdapter::LefDefInputAdapter(std::unique_ptr<core::PlacementDB> database)
    : db(database ? std::move(database) : std::make_unique<core::PlacementDB>()) {}

LefDefInputAdapter::~LefDefInputAdapter() = default;

LefDefInputAdapter::LefDefInputAdapter(LefDefInputAdapter &&) noexcept = default;
LefDefInputAdapter &LefDefInputAdapter::operator=(LefDefInputAdapter &&) noexcept = default;

bool LefDefInputAdapter::readFromDirectory(const std::string &dirPath) {
    namespace fs = std::filesystem;

    std::string defFile;
    std::vector<std::string> lefFiles;
    std::error_code ec;
    fs::directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string lower = [&]() {
            std::string l = name;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return l;
        }();
        if ((lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".def") == 0) ||
            (lower.size() >= 7 && lower.compare(lower.size() - 7, 7, ".def.gz") == 0)) {
            if (defFile.empty() || lower.find("floorplan") != std::string::npos) {
                defFile = it->path().string();
            }
        }
        if ((lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lef") == 0) ||
            (lower.size() >= 7 && lower.compare(lower.size() - 7, 7, ".lef.gz") == 0)) {
            lefFiles.push_back(it->path().string());
        }
    }
    if (ec) {
        std::cerr << "Error: cannot scan LEF/DEF directory " << dirPath << std::endl;
        return false;
    }
    if (defFile.empty()) {
        std::cerr << "Error: no .def file found in " << dirPath << std::endl;
        return false;
    }
    return readFromFiles(defFile, lefFiles);
}

bool LefDefInputAdapter::readFromFiles(const std::string &defFile,
                                       const std::vector<std::string> &lefFiles) {
    // DEF coordinate scale must be known before LEF micron sizes are used.
    peekDefUnits(defFile);

    for (const std::string &lef : lefFiles) {
        if (!parseLefFile(lef)) {
            return false;
        }
    }
    return parseDefFile(defFile);
}

void LefDefInputAdapter::peekDefUnits(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        return;
    }
    unitsPerMicron = 1.0;
    std::string line;
    while (std::getline(file.stream(), line)) {
        if (line.find("UNITS") == std::string::npos && line.find("MICRONS") == std::string::npos) {
            continue;
        }
        const auto tokens = tokenize(line);
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i] == "MICRONS") {
                double v = 1.0;
                if (tryDouble(tokens[i + 1], v) && v > 0.0) {
                    unitsPerMicron = v;
                }
                return;
            }
        }
        return;
    }
}

bool LefDefInputAdapter::parseLefFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        std::cerr << "Error: cannot open LEF file: " << filePath << std::endl;
        return false;
    }

    std::string line;
    bool inMacro = false;
    std::string macro;
    MacroRec rec;
    bool inPin = false;
    std::string pin;
    bool inSite = false;
    std::string site;
    std::size_t lineNum = 0;

    while (std::getline(file.stream(), line)) {
        ++lineNum;
        const auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        if (!inMacro && !inSite) {
            if (tokens[0] == "MACRO" && tokens.size() >= 2) {
                inMacro = true;
                inPin = false;
                macro = sanitizeName(tokens[1]);
                rec = MacroRec();
            } else if (tokens[0] == "SITE" && tokens.size() >= 2) {
                inSite = true;
                site = tokens[1];
            } else if (tokens[0] == "UNITS") {
                // LEF may carry its own UNITS block; harmless to skip.
            }
            continue;
        }

        if (inSite) {
            if (tokens[0] == "SIZE" && tokens.size() >= 4) {
                double w = 0.0, h = 0.0;
                if (tryDouble(tokens[1], w)) {
                    siteWidthMicrons = w;
                }
                if (tryDouble(tokens[3], h)) {
                    siteHeightMicrons = h;
                }
            } else if (tokens[0] == "END") {
                inSite = false;
                site.clear();
            }
            continue;
        }

        if (tokens[0] == "SIZE" && tokens.size() >= 4) {
            double w = 1.0, h = 1.0;
            if (tryDouble(tokens[1], w)) {
                rec.widthMicrons = w;
            }
            if (tryDouble(tokens[3], h)) {
                rec.heightMicrons = h;
            }
        } else if (tokens[0] == "CLASS" && tokens.size() >= 2) {
            rec.isBlock = (tokens[1] == "BLOCK");
        } else if (tokens[0] == "PIN" && tokens.size() >= 2) {
            inPin = true;
            pin = sanitizeName(tokens[1]);
            rec.pinExists[pin] = true;
        } else if (tokens[0] == "DIRECTION" && inPin && tokens.size() >= 2) {
            const std::string &d = tokens[1];
            rec.pinIsInput[pin] = (d != "OUTPUT");
        } else if (tokens[0] == "RECT" && inPin && tokens.size() >= 5) {
            double x1 = 0.0, y1 = 0.0;
            tryDouble(tokens[1], x1);
            tryDouble(tokens[2], y1);
            rec.pinOffset[pin] = {x1, y1};
        } else if (tokens[0] == "END") {
            if (tokens.size() >= 2) {
                if (inPin && sanitizeName(tokens[1]) == pin) {
                    inPin = false;
                    pin.clear();
                } else if (sanitizeName(tokens[1]) == macro || tokens[1] == macro) {
                    macros[macro] = rec;
                    inMacro = false;
                }
            }
        }
    }
    return true;
}

bool LefDefInputAdapter::parseDefFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        std::cerr << "Error: cannot open DEF file: " << filePath << std::endl;
        return false;
    }

    std::string line;
    std::size_t lineNum = 0;
    int section = 0;  // 0=none, 1=COMPONENTS, 2=PINS, 3=NETS

    // Component entry under construction ("- inst macro" + continuation lines).
    std::string compName;
    std::string macroName;
    bool compSeen = false;  // inside a component entry
    bool compPlaced = false;
    bool compFixed = false;
    double compX = 0.0, compY = 0.0;

    // Pin entry under construction ("- pinName" + continuation lines).
    std::string pinName;
    bool pinSeen = false;
    bool pinPlaced = false;
    double pinX = 0.0, pinY = 0.0;

    // Nets: assembled token stream for the section.
    std::vector<std::string> netTokens;

    const auto flushComponent = [&]() {
        if (!compSeen) {
            return;
        }
        compSeen = false;
        const std::string cname = sanitizeName(compName);
        auto mIt = macros.find(sanitizeName(macroName));
        double w = unitsPerMicron, h = unitsPerMicron;
        if (mIt != macros.end()) {
            w = mIt->second.widthMicrons * unitsPerMicron;
            h = mIt->second.heightMicrons * unitsPerMicron;
        }
        db->addCell(cname, w, h, false);
        if (compPlaced) {
            db->setCellPosition(cname, compX, compY);
        }
        if (compFixed) {
            db->setCellFixed(cname, true);
        }
        instMacro[cname] = sanitizeName(macroName);
    };

    const auto flushPin = [&]() {
        if (!pinSeen) {
            return;
        }
        pinSeen = false;
        const std::string pname = sanitizeName(pinName);
        auto mIt = macros.find(pname);
        double w = unitsPerMicron, h = unitsPerMicron;
        if (mIt != macros.end()) {
            w = mIt->second.widthMicrons * unitsPerMicron;
            h = mIt->second.heightMicrons * unitsPerMicron;
        } else {
            w = std::max(unitsPerMicron, 1.0);
            h = std::max(unitsPerMicron, 1.0);
        }
        db->addCell(pname, w, h, true);  // I/O pads are terminals
        if (pinPlaced) {
            db->setCellPosition(pname, pinX, pinY);
        }
    };

    while (std::getline(file.stream(), line)) {
        ++lineNum;
        const auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        // ---- Row / die / units lines (only valid outside sections) ----
        if (tokens[0] == "ROW" && tokens.size() >= 13) {
            double x = 0.0, y = 0.0, numX = 0.0, dx = 200.0, dy = 0.0;
            tryDouble(tokens[3], x);
            tryDouble(tokens[4], y);
            // ROW name site X Y orient DO numX BY numY STEP dx dy
            if (tokens.size() > 7) {
                tryDouble(tokens[7], numX);
            }
            for (std::size_t i = 8; i + 1 < tokens.size(); ++i) {
                if (tokens[i] == "STEP") {
                    tryDouble(tokens[i + 1], dx);
                    if (i + 2 < tokens.size()) {
                        tryDouble(tokens[i + 2], dy);
                    }
                }
            }
            // Many contest DEFs describe a single strip per ROW line with
            // "STEP <siteWidth> 0"; fall back to the LEF site height for the
            // row height in that case.
            double rowH = dy > 0.0 ? dy : siteHeightMicrons * unitsPerMicron;
            double siteW = dx > 0.0 ? dx : siteWidthMicrons * unitsPerMicron;
            if (siteW <= 0.0) {
                siteW = 1.0;
            }
            db->addRow(y, rowH, siteW, siteW, numX);
            continue;
        }
        if (tokens[0] == "DIEAREA" && tokens.size() >= 9) {
            double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;
            // DIEAREA ( x0 y0 ) ( x1 y1 ) ;
            if (tokens.size() >= 5) {
                tryDouble(tokens[1], x0);
                tryDouble(tokens[2], y0);
            }
            if (tokens.size() >= 7) {
                tryDouble(tokens[4], x1);
                tryDouble(tokens[5], y1);
            }
            if (x1 <= x0 || y1 <= y0) {
                x1 = std::max(x1, x0 + 1.0);
                y1 = std::max(y1, y0 + 1.0);
            }
            db->setDieArea(x0, y0, x1, y1);
            continue;
        }

        // ---- Section transitions ----
        if (tokens[0] == "COMPONENTS") {
            section = 1;
            continue;
        }
        if (section == 1 && tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "COMPONENTS") {
            flushComponent();
            flushPin();
            section = 0;
            continue;
        }
        if (tokens[0] == "PINS") {
            flushComponent();
            flushPin();
            section = 2;
            continue;
        }
        if (section == 2 && tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "PINS") {
            flushPin();
            section = 0;
            continue;
        }
        if (tokens[0] == "NETS") {
            flushPin();
            netTokens.clear();
            section = 3;
            continue;
        }
        if (section == 3 && tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "NETS") {
            section = 0;
            continue;
        }
        if (tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "DESIGN") {
            flushComponent();
            flushPin();
            section = 0;
            continue;
        }

        // ---- COMPONENTS section ----
        if (section == 1) {
            if (tokens[0] == "-" && tokens.size() >= 3) {
                flushComponent();
                compName = tokens[1];
                macroName = tokens[2];
                compSeen = true;
                compPlaced = false;
                compFixed = false;
                compX = 0.0;
                compY = 0.0;
                // Same-line "+ ..." properties (e.g. "+ FIXED ( x y ) N ;")
                for (std::size_t i = 3; i < tokens.size(); ++i) {
                    if (tokens[i] == "PLACED" || tokens[i] == "FIXED") {
                        std::size_t c = i + 1;
                        if (c < tokens.size() && tokens[c] == "(") {
                            ++c;
                        }
                        double tx = 0.0, ty = 0.0;
                        if (c + 1 < tokens.size() && tryDouble(tokens[c], tx) &&
                            tryDouble(tokens[c + 1], ty)) {
                            compPlaced = true;
                            compFixed = (tokens[i] == "FIXED");
                            compX = tx;
                            compY = ty;
                        }
                    } else if (tokens[i] == "UNPLACED") {
                        compPlaced = false;
                    }
                }
            } else if (tokens[0] == "+") {
                if (tokens.size() >= 2 && tokens[1] == "PLACED" && tokens.size() >= 6) {
                    compPlaced = true;
                    tryDouble(tokens[3], compX);
                    tryDouble(tokens[4], compY);
                } else if (tokens.size() >= 2 && tokens[1] == "FIXED" && tokens.size() >= 6) {
                    compPlaced = true;
                    compFixed = true;
                    tryDouble(tokens[3], compX);
                    tryDouble(tokens[4], compY);
                } else if (tokens.size() >= 2 && tokens[1] == "UNPLACED") {
                    compPlaced = false;
                }
            }
            continue;
        }

        // ---- PINS section ----
        if (section == 2) {
            if (tokens[0] == "-" && tokens.size() >= 2) {
                flushPin();
                pinName = tokens[1];
                pinSeen = true;
                pinPlaced = false;
                pinX = 0.0;
                pinY = 0.0;
                for (std::size_t i = 2; i < tokens.size(); ++i) {
                    if (tokens[i] == "PLACED" || tokens[i] == "FIXED") {
                        std::size_t c = i + 1;
                        if (c < tokens.size() && tokens[c] == "(") {
                            ++c;
                        }
                        double tx = 0.0, ty = 0.0;
                        if (c + 1 < tokens.size() && tryDouble(tokens[c], tx) &&
                            tryDouble(tokens[c + 1], ty)) {
                            pinPlaced = true;
                            pinX = tx;
                            pinY = ty;
                        }
                    }
                }
            } else if (tokens[0] == "+" && pinSeen) {
                if (tokens.size() >= 2 && (tokens[1] == "PLACED" || tokens[1] == "FIXED") &&
                    tokens.size() >= 6) {
                    pinPlaced = true;
                    tryDouble(tokens[3], pinX);
                    tryDouble(tokens[4], pinY);
                }
            }
            continue;
        }

        // ---- NETS section ----
        if (section == 3) {
            // A DEF net can span multiple lines; accumulate raw tokens and
            // split them into nets at '-' markers and ';' terminators.
            netTokens.insert(netTokens.end(), tokens.begin(), tokens.end());
            continue;
        }
    }

    flushComponent();
    flushPin();

    // ---- Process the accumulated NETS token stream ----
    std::string netName;
    std::vector<NetPinRef> pins;
    bool haveNet = false;
    bool inGroup = false;
    std::vector<std::string> group;

    const auto resolveGroup = [&]() {
        if (group.empty()) {
            return;
        }
        NetPinRef ref;
        if (!group.empty() && group[0] == "PIN") {
            ref.ioPin = sanitizeName(group[1]);
        } else if (group.size() >= 2) {
            ref.inst = sanitizeName(group[0]);
            ref.pin = sanitizeName(group[1]);
        }
        group.clear();
        pins.push_back(std::move(ref));
    };

    const auto flushNet = [&]() {
        if (!haveNet) {
            return;
        }
        haveNet = false;
        std::string name = netName;
        netName.clear();
        if (name.empty()) {
            return;
        }
        // The graph keys every vertex (cells AND nets) by name globally, but
        // in DEF every I/O pad's net is named after the pad itself.  Disambiguate
        // the net so the pad cell keeps its own name.
        while (db->hasCell(name)) {
            name += "__NET";
        }
        if (!db->hasNet(name)) {
            db->addNet(name, 1.0);
        }
        for (const NetPinRef &ref : pins) {
            std::string cell;
            double offsetX = 0.0, offsetY = 0.0;
            bool isInput = false;
            if (!ref.inst.empty()) {
                cell = ref.inst;
                auto it = instMacro.find(cell);
                if (it != instMacro.end()) {
                    auto mIt = macros.find(it->second);
                    if (mIt != macros.end()) {
                        const MacroRec &mr = mIt->second;
                        auto pOff = mr.pinOffset.find(ref.pin);
                        if (pOff != mr.pinOffset.end()) {
                            offsetX = pOff->second.first * unitsPerMicron;
                            offsetY = pOff->second.second * unitsPerMicron;
                        }
                        auto pIn = mr.pinIsInput.find(ref.pin);
                        if (pIn != mr.pinIsInput.end()) {
                            isInput = pIn->second;
                        }
                    }
                }
            } else {
                cell = ref.ioPin;
            }
            if (cell.empty() || !db->hasCell(cell)) {
                continue;
            }
            try {
                db->addPin(cell, name, offsetX, offsetY, isInput);
            } catch (const std::exception &e) {
                std::cerr << "Warning: DEF net '" << name << "' pin '" << cell
                          << "' skipped: " << e.what() << std::endl;
            }
        }
        pins.clear();
    };

    for (const std::string &tok : netTokens) {
        if (tok == "-") {
            // New net: finalise the previous one.
            flushNet();
            haveNet = true;
            inGroup = false;
        } else if (tok == ";") {
            flushNet();
            inGroup = false;
        } else if (tok == "(") {
            inGroup = true;
            group.clear();
        } else if (tok == ")") {
            inGroup = false;
            resolveGroup();
        } else if (tok == "+") {
            // Ignore any + continuations reaching the stream (none expected).
        } else if (inGroup) {
            group.push_back(tok);
        } else if (haveNet) {
            // Net name tokens up to the first '(' group.
            if (!netName.empty()) {
                netName += "_";
            }
            netName += tok;
        }
    }
    flushNet();

    return true;
}

}  // namespace io
}  // namespace ktplace