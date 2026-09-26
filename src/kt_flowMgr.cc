/**
 * @file kt_flowMgr.cc
 * @brief Implementation of FlowMgr
 */

#include "kt_flowMgr.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "util/kt_log.h"
#include "placer/kt_quadPlacer.h"
#include "datamodel/kt_graph.h"
#include "adaptor/bookshelfToKTAdaptor.h"
#include "adaptor/lefdefToKTAdaptor.h"
#include <memory>
#include <fstream>
#include <string>
#include <algorithm>
#include <iomanip>
#include <stdexcept>
#include <chrono>
#include <filesystem>

namespace ktplace {

// PIMPL implementation
class FlowMgr::Impl {
public:
    std::unique_ptr<PlacementDB> db;
    std::unique_ptr<BookshelfInputAdapter> bookshelfAdapter;
    std::unique_ptr<LefDefInputAdapter> lefdefAdapter;
    bool loaded = false;
    bool placed = false;

    // Internal methods
    bool loadInput(const std::string &baseName, const std::string &dirPath);
    bool loadBookshelf(const std::string &baseName, const std::string &dirPath);
    bool loadBookshelfFromFiles(const std::string &nodesFile, const std::string &netsFile,
                                const std::string &plFile = "", const std::string &sclFile = "",
                                const std::string &wtsFile = "");
    bool runPlacement(const std::string &algorithm = "quadratic", const std::string &plotDir = "");
    bool writePlacement(const std::string &outputPath, const std::string &format = "bookshelf");
    PlacementDB &getPlacementDB();
    const PlacementDB &getPlacementDB() const;
    bool isLoaded() const;
    void clear();
};

// FlowMgr implementation

FlowMgr::FlowMgr() : pImpl(std::make_unique<Impl>()) {
    pImpl->db = std::make_unique<PlacementDB>();
    pImpl->bookshelfAdapter = std::make_unique<BookshelfInputAdapter>();
}

FlowMgr::~FlowMgr() = default;

FlowMgr::FlowMgr(FlowMgr &&) noexcept = default;
FlowMgr &FlowMgr::operator=(FlowMgr &&) noexcept = default;

void FlowMgr::run(const std::string &inputBaseName, const std::string &inputDirPath,
                  const std::string &outputPath, const std::string &algorithm,
                  const std::string &outputFormat, const std::string &plotDir) {
    // Phase timers accumulate into the registry; the summary is reported once
    // at the end of the run.
    {
        ScopedTimer timer("load");
        if (!pImpl->loadInput(inputBaseName, inputDirPath)) {
            throw std::runtime_error("Failed to load input files");
        }
    }

    // Report loaded statistics
    {
        PlacementDB &db = pImpl->getPlacementDB();
        auto [numCells, numNets] = db.getStats();
        ktlog.echo("Loaded: {} cells ({} terminals), {} nets, {} pins, {} rows", numCells,
                   db.getNumTerminals(), numNets, db.getNumPins(), db.getNumRows());
    }

    // Run placement
    {
        ScopedTimer timer("place");
        if (!pImpl->runPlacement(algorithm, plotDir)) {
            throw std::runtime_error("Placement algorithm failed");
        }
    }

    // Write output
    {
        ScopedTimer timer("write");
        if (!pImpl->writePlacement(outputPath, outputFormat)) {
            throw std::runtime_error("Failed to write output");
        }
    }

    TimerRegistry::instance().report();
}

// Implementation of Impl methods

bool FlowMgr::Impl::loadInput(const std::string &baseName, const std::string &dirPath) {
    // Auto-detect the input format: a directory containing LEF/DEF files is
    // loaded through the LEF/DEF adapter; otherwise Bookshelf is assumed.
    namespace fs = std::filesystem;
    bool hasDef = false;
    std::error_code ec;
    fs::directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if ((name.size() >= 4 && name.rfind(".def") == name.size() - 4) ||
            (name.size() >= 7 && name.rfind(".def.gz") == name.size() - 7)) {
            hasDef = true;
            break;
        }
    }
    if (ec) {
        ktlog.fatal("cannot scan input directory: {}", dirPath);
    }
    if (hasDef) {
        ktlog.echo("Detected LEF/DEF input in {}", dirPath);
        clear();
        lefdefAdapter = std::make_unique<LefDefInputAdapter>(std::make_unique<PlacementDB>());
        if (!lefdefAdapter->readFromDirectory(dirPath)) {
            ktlog.fatal("Failed to load LEF/DEF format from {}", dirPath);
        }
        db = lefdefAdapter->releasePlacementDB();
        loaded = true;
        placed = false;
        return true;
    }
    return loadBookshelf(baseName, dirPath);
}

bool FlowMgr::Impl::loadBookshelf(const std::string &baseName, const std::string &dirPath) {
    clear();

    // Create adapter with the database
    bookshelfAdapter = std::make_unique<BookshelfInputAdapter>(std::make_unique<PlacementDB>());

    // Read from directory
    if (!bookshelfAdapter->readFromDirectory(baseName, dirPath)) {
        ktlog.fatal("Failed to load Bookshelf format from {}", dirPath);
    }

    // Transfer ownership of database
    db = bookshelfAdapter->releasePlacementDB();
    loaded = true;
    placed = false;

    return true;
}

bool FlowMgr::Impl::loadBookshelfFromFiles(const std::string &nodesFile,
                                           const std::string &netsFile, const std::string &plFile,
                                           const std::string &sclFile, const std::string &wtsFile) {
    clear();

    // Create adapter with the database
    bookshelfAdapter = std::make_unique<BookshelfInputAdapter>(std::make_unique<PlacementDB>());

    // Read from files
    if (!bookshelfAdapter->readFromFiles(nodesFile, netsFile, plFile, sclFile, wtsFile)) {
        ktlog.fatal("Failed to load Bookshelf format files");
    }

    // Transfer ownership of database
    db = bookshelfAdapter->releasePlacementDB();
    loaded = true;
    placed = false;

    return true;
}

bool FlowMgr::Impl::runPlacement(const std::string &algorithm, const std::string &plotDir) {
    if (!loaded) {
        ktlog.fatal("No placement database loaded");
    }

    if (algorithm == "quadratic") {
        ktlog.echo("Running global placement (clique/star net model, WL + density, PCG)...");
        QuadraticPlacer placer(*db);
        // Region ("fence") constraints come from the LEF/DEF reader; the
        // Bookshelf format has no equivalent, so this is null there.
        const constraintMgr *regions = nullptr;
        if (lefdefAdapter) {
            regions = &lefdefAdapter->getConstraints();
        }
        const PlacerResult res = placer.place(200, 0.10, plotDir, regions);
        // Units live in the metric name so the value columns stay purely
        // numeric and get right-aligned by the table.
        ktReportTable summary("Solver results");
        summary.setHeaders({"metric", "initial", "final"});
        summary.addRow({"movable cells", "", fmt::format("{}", res.numMovable)});
        summary.addRow({"star nodes", "", fmt::format("{}", res.numStars)});
        summary.addRow({"outer iterations", "", fmt::format("{}", res.numIterations)});
        summary.addRow({"matrix build (s)", "", fmt::format("{:.6}", res.buildSeconds)});
        summary.addRow({"global place (s)", "", fmt::format("{:.6}", res.solveSeconds)});
        summary.addRow({"density overflow", fmt::format("{:.6}", res.densityOverflowInitial),
                        fmt::format("{:.6}", res.densityOverflowFinal)});
        summary.addRow(
            {"HPWL", fmt::format("{:.6}", res.hpwlInitial), fmt::format("{:.6}", res.hpwlFinal)});
        // A ratio, not a signed percentage: spreading raises wirelength well
        // above the seed, and a degenerate seed (every cell on one point, as
        // in several public Bookshelf suites) makes any percentage meaningless.
        summary.addRow({"HPWL / seed", "",
                        res.hpwlInitial > 0.0
                            ? fmt::format("{:.2f}x", res.hpwlFinal / res.hpwlInitial)
                            : std::string("n/a (degenerate seed)")});
        summary.emit();
        placed = true;
        return true;
    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

bool FlowMgr::Impl::writePlacement(const std::string &outputPath, const std::string &format) {
    if (!placed) {
        ktlog.fatal("No placement result available");
    }

    if (format == "bookshelf") {
        std::ofstream out(outputPath);
        if (!out.is_open()) {
            ktlog.fatal("Cannot open output file: {}", outputPath);
        }
        // Database coordinates reach ~1.5e6, so the default 6 significant
        // digits would round positions by several units and can move a cell
        // across a placement-region boundary. Keep enough digits to round-trip.
        out << std::setprecision(10);
        const Graph &g = db->getGraph();
        const std::size_t nv = g.getNumVertices();
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell)
                continue;
            // Bookshelf .pl: "<name> <x> <y> : <orientation>"
            out << vert.name << '\t' << vert.x << '\t' << vert.y
                << "\t: " << (vert.isFixed ? "N /FIXED" : "N") << '\n';
        }
        out.close();
        return true;
    } else {
        ktlog.fatal("Unknown output format: {}", format);
    }
}

PlacementDB &FlowMgr::Impl::getPlacementDB() {
    if (!db) {
        throw std::runtime_error("PlacementDB not initialized");
    }
    return *db;
}

const PlacementDB &FlowMgr::Impl::getPlacementDB() const {
    if (!db) {
        throw std::runtime_error("PlacementDB not initialized");
    }
    return *db;
}

bool FlowMgr::Impl::isLoaded() const {
    return loaded;
}

void FlowMgr::Impl::clear() {
    db.reset();
    bookshelfAdapter.reset();
    lefdefAdapter.reset();
    loaded = false;
    placed = false;
}

}  // namespace ktplace
