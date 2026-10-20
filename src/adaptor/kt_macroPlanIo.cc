#include "adaptor/kt_macroPlanIo.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_macroPlan.h"
#include "util/kt_gzipFile.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

class Tokens {
public:
    Tokens(std::istream &in, std::string source) : file(std::move(source)) {
        std::string word;
        while (in >> word) {
            words.push_back(std::move(word));
        }
    }

    [[nodiscard]] bool done() const {
        return at >= words.size();
    }

    const std::string &next() {
        if (done()) {
            throw std::invalid_argument(std::format("{}: unexpected end of file", file));
        }
        return words[at++];
    }

    void expect(const std::string &word) {
        const std::string &got = next();
        if (got != word) {
            throw std::invalid_argument(
                std::format("{}: expected '{}' but read '{}'", file, word, got));
        }
    }

    double number() {
        const std::string &word = next();
        try {
            return std::stod(word);
        } catch (const std::exception &) {
            throw std::invalid_argument(
                std::format("{}: expected a number, read '{}'", file, word));
        }
    }

    [[nodiscard]] const std::string &source() const {
        return file;
    }

private:
    std::string file;
    std::vector<std::string> words;
    std::size_t at = 0;
};

Tokens readTokens(const std::string &path) {
    GzipInputFile in(path);
    if (!in) {
        throw std::invalid_argument(std::format("cannot read {}", path));
    }
    return {in, path};
}

void readComponents(Tokens &tokens, const ktDM &db, MacroPlan &plan) {
    const auto count = static_cast<std::size_t>(tokens.number());
    tokens.expect(";");
    while (true) {
        const std::string &word = tokens.next();
        if (word == "END") {
            tokens.expect("COMPONENTS");
            break;
        }
        if (word != "-") {
            throw std::invalid_argument(std::format("{}: expected '-' or END COMPONENTS, read '{}'",
                                                    tokens.source(), word));
        }
        PlannedMacro macro;
        const std::string name = tokens.next();
        if (!db.hasCell(name)) {
            throw std::invalid_argument(
                std::format("{}: macro {} is not in the design", tokens.source(), name));
        }
        macro.cellId = db.getCellId(name);
        macro.master = tokens.next();
        tokens.expect("+");
        const std::string status = tokens.next();
        if (status != "PLACED" && status != "FIXED") {
            throw std::invalid_argument(
                std::format("{}: {} is {}, not PLACED or FIXED", tokens.source(), name, status));
        }
        macro.movable = status == "PLACED";
        tokens.expect("(");
        macro.originX = tokens.number();
        macro.originY = tokens.number();
        tokens.expect(")");
        const std::string orient = tokens.next();
        const std::optional<Orient> parsed = orientFromName(orient);
        if (!parsed) {
            throw std::invalid_argument(std::format("{}: {} has orientation {}, not N, S, FN or FS",
                                                    tokens.source(), name, orient));
        }
        macro.originOrient = *parsed;
        tokens.expect(";");
        plan.macros.push_back(std::move(macro));
    }
    if (plan.macros.size() != count) {
        throw std::invalid_argument(std::format("{}: COMPONENTS says {} but lists {}",
                                                tokens.source(), count, plan.macros.size()));
    }
}

void readDieArea(Tokens &tokens, MacroPlan &plan) {
    while (true) {
        const std::string &word = tokens.next();
        if (word == ";") {
            break;
        }
        if (word != "(") {
            throw std::invalid_argument(
                std::format("{}: expected '(' in DIEAREA, read '{}'", tokens.source(), word));
        }
        const double x = tokens.number();
        const double y = tokens.number();
        tokens.expect(")");
        plan.diePoints.emplace_back(x, y);
    }
    if (plan.diePoints.size() < 2) {
        throw std::invalid_argument(std::format("{}: DIEAREA needs two points", tokens.source()));
    }
    plan.die = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    for (const auto &[x, y] : plan.diePoints) {
        plan.die[0] = std::min(plan.die[0], x);
        plan.die[1] = std::min(plan.die[1], y);
        plan.die[2] = std::max(plan.die[2], x);
        plan.die[3] = std::max(plan.die[3], y);
    }
}

void readMlist(const ktDM &db, const std::string &path, MacroPlan &plan) {
    Tokens tokens = readTokens(path);
    while (!tokens.done()) {
        const std::string word = tokens.next();
        if (word == "VERSION") {
            plan.version = tokens.next();
            tokens.expect(";");
        } else if (word == "DESIGN") {
            plan.design = tokens.next();
            tokens.expect(";");
        } else if (word == "UNITS") {
            tokens.expect("DISTANCE");
            tokens.expect("MICRONS");
            plan.dbuPerMicron = tokens.number();
            tokens.expect(";");
        } else if (word == "DIEAREA") {
            readDieArea(tokens, plan);
        } else if (word == "COMPONENTS") {
            readComponents(tokens, db, plan);
        } else if (word == "END") {
            tokens.expect("DESIGN");
        } else {
            throw std::invalid_argument(std::format("{}: unexpected '{}'", path, word));
        }
    }
    if (plan.diePoints.empty()) {
        throw std::invalid_argument(std::format("{}: no DIEAREA", path));
    }
}

void readRules(const std::string &path, MacroPlan &plan) {
    Tokens tokens = readTokens(path);
    while (!tokens.done()) {
        const std::string key = tokens.next();
        const double microns = tokens.number();
        const double dbu = microns * plan.dbuPerMicron;
        if (key == "maximum_displacement_constraint") {
            plan.rules.maxDisplacement = dbu;
        } else if (key == "minimum_channel_spacing_between_macros_constraint") {
            plan.rules.minSpacing = dbu;
        } else if (key == "macro_halo") {
            plan.rules.halo = dbu;
        } else {
            throw std::invalid_argument(std::format("{}: unknown constraint '{}'", path, key));
        }
    }
}

std::string formatCoordinate(double value) {
    return std::format("{}", static_cast<long long>(value));
}

}  // namespace

MacroPlan readMacroPlan(const ktDM &db, const std::string &mlistPath,
                        const std::string &rulesPath) {
    MacroPlan plan;
    readMlist(db, mlistPath, plan);
    readRules(rulesPath, plan);
    return plan;
}

void writeMacroPlan(const ktDM &db, const std::string &path) {
    const MacroPlan &plan = db.macroPlan();
    std::ofstream out(path);
    if (!out) {
        throw std::invalid_argument(std::format("cannot write {}", path));
    }
    out << "VERSION " << plan.version << " ;\n";
    out << "DESIGN " << plan.design << " ;\n";
    out << "UNITS DISTANCE MICRONS " << formatCoordinate(plan.dbuPerMicron) << " ;\n\n";
    out << "DIEAREA";
    for (const auto &[x, y] : plan.diePoints) {
        out << " ( " << formatCoordinate(x) << ' ' << formatCoordinate(y) << " )";
    }
    out << " ;\n\n";
    out << "COMPONENTS " << plan.macros.size() << " ;\n";
    const Graph &graph = db.getGraph();
    for (const PlannedMacro &macro : plan.macros) {
        const Vertex &cell = graph.getCell(macro.cellId);
        out << "   - " << cell.name << ' ' << macro.master << " \n";
        out << "      + " << (macro.movable ? "PLACED" : "FIXED") << " ( "
            << formatCoordinate(cell.x) << ' ' << formatCoordinate(cell.y) << " ) "
            << orientName(cell.orient) << " ;\n";
    }
    out << "END COMPONENTS\n\n\n\nEND DESIGN\n\n";
}

}  // namespace ktplace
