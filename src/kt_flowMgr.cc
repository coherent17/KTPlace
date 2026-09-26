/**
 * @file kt_flowMgr.cc
 * @brief Implementation of FlowMgr
 */

#include "kt_flowMgr.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "util/kt_log.h"
#include "placer/kt_quadPlacer.h"
#include "legalizer/kt_abacus.h"
#include "placer/simpl/kt_simpl.h"
#include "datamodel/kt_graph.h"
#include "adaptor/bookshelfToKTAdaptor.h"
#include "adaptor/lefdefToKTAdaptor.h"
#include <memory>
#include <fstream>
#include <string>
#include <algorithm>
#include <cstdlib>
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
    bool runPlacement(const std::string &algorithm = "quadratic", const std::string &plotDir = "",
                      const std::string &snapshotDir = "");
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
        const std::string snapshotDir = outputPath.find_last_of('/') == std::string::npos
                                            ? std::string(".")
                                            : outputPath.substr(0, outputPath.find_last_of('/'));
        // By default every run records a per-iteration SVG frame + HPWL/galley
        // report next to the result, so an iteration can be inspected without
        // remembering the -p flag.  An explicit -p directory still wins.
        const std::string effectivePlotDir =
            !plotDir.empty() ? plotDir : snapshotDir + "/" + inputBaseName + "_plots";
        if (!pImpl->runPlacement(algorithm, effectivePlotDir, snapshotDir)) {
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

bool FlowMgr::Impl::runPlacement(const std::string &algorithm, const std::string &plotDir,
                                 const std::string &snapshotDir) {
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
        const PlacerResult res = placer.place(200, 0.10, plotDir, regions, snapshotDir);
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
    } else if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        SimplePlacer placer(*db);
        // Bookshelf carries no placement regions; the LEF/DEF reader is the only
        // source of fences, so they are not consulted here. SimPL's own spreading
        // comes from legalizing, not from a fence-aware field.
        SimplParams params;
        params.traceEvery = 10;
        // Per-iteration frames, for watching the LSS/LAL interaction.
        if (const char *e = std::getenv("KTPLACE_SIMPL_TRACE_EVERY")) {
            params.traceEvery = static_cast<std::size_t>(std::atoll(e));
        }
        const SimplResult res = placer.place(params, plotDir, snapshotDir);
        ktReportTable summary("Solver results");
        summary.setHeaders({"metric", "initial", "final"});
        summary.addRow({"movable cells", "", fmt::format("{}", res.numMovable)});
        summary.addRow({"fixed cells", "", fmt::format("{}", res.numFixed)});
        summary.addRow({"nets", "", fmt::format("{}", res.nets)});
        summary.addRow({"init iterations", "", fmt::format("{}", res.initIters)});
        summary.addRow({"global iterations", "", fmt::format("{}", res.globalIters)});
        summary.addRow({"bin grid", "", fmt::format("{}x{}", res.binsX, res.binsY)});
        summary.addRow({"matrix build (s)", "", fmt::format("{:.6}", res.buildSeconds)});
        summary.addRow({"look-ahead (s)", "", fmt::format("{:.6}", res.spreadSeconds)});
        summary.addRow({"linear solves (s)", "", fmt::format("{:.6}", res.solveSeconds)});
        summary.addRow({"HPWL seed", fmt::format("{:.6}", res.hpwlSeed), ""});
        summary.addRow({"HPWL lower bound", "", fmt::format("{:.6}", res.hpwlLower)});
        summary.addRow({"HPWL final", "", fmt::format("{:.6}", res.hpwlFinal)});
        summary.addRow({"bound gap", "", fmt::format("{:.6}", res.gap)});
        summary.addRow({"scaled overflow (lower)", "", fmt::format("{:.6}", res.overflowLower)});
        summary.addRow({"scaled overflow (final)", "", fmt::format("{:.6}", res.overflowFinal)});
        summary.emit();

        // Global placement leaves the cells overlapping and off-row. Abacus
        // removes the overlap with the least movement it can, and self-checks
        // the result so a legalization bug shows up as a count, not as a
        // silently bad placement.
        ktlog.echo("Running Abacus legalization...");
        AbacusLegalizer legalizer(*db);
        LegalizeParams lparams;
        if (const char *e = std::getenv("KTPLACE_ABACUS_MAX_ROW_DIST")) {
            lparams.maxRowDistance = static_cast<std::size_t>(std::atoll(e));
        }
        if (!plotDir.empty()) {
            lparams.plotDir = plotDir + "/legalize";
            lparams.frameEvery = std::getenv("KTPLACE_ABACUS_FRAME_EVERY")
                                     ? static_cast<std::size_t>(
                                           std::atoll(std::getenv("KTPLACE_ABACUS_FRAME_EVERY")))
                                     : 20000;
        }
        const LegalizeResult lres = legalizer.legalize(lparams);
        ktReportTable lsummary("Legalization (Abacus)");
        lsummary.setHeaders({"metric", "value"});
        lsummary.addRow({"cells placed", fmt::format("{}", lres.cellsPlaced)});
        lsummary.addRow({"cells unplaced", fmt::format("{}", lres.unplaced)});
        lsummary.addRow({"squared displacement", fmt::format("{:.6}", lres.totalSquaredDisplacement)});
        lsummary.addRow({"max displacement", fmt::format("{:.6}", lres.maxDisplacement)});
        lsummary.addRow({"HPWL before", fmt::format("{:.6}", lres.hpwlBefore)});
        lsummary.addRow({"HPWL after", fmt::format("{:.6}", lres.hpwlAfter)});
        lsummary.addRow({"time (s)", fmt::format("{:.6}", lres.seconds)});
        lsummary.addRow({"overlapping pairs", fmt::format("{}", lres.overlappingPairs)});
        lsummary.addRow({"cells off row", fmt::format("{}", lres.offRow)});
        lsummary.addRow({"cells off site", fmt::format("{}", lres.offSite)});
        lsummary.addRow({"cells over macro", fmt::format("{}", lres.overFixed)});
        lsummary.addRow({"cells out of rows", fmt::format("{}", lres.outOfRows)});
        lsummary.emit();
        if (lres.overlappingPairs != 0 || lres.offRow != 0 || lres.overFixed != 0) {
            ktlog.echo("WARNING: legalization is not legal; see counts above");
        }

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
