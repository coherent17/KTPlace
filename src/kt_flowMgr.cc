/**
 * @file kt_flowMgr.cc
 * @brief Implementation of FlowMgr
 */

#include "kt_flowMgr.h"
#include "util/kt_reportTable.h"
#include "visualization/kt_animator.h"
#include "util/kt_scopedTimer.h"
#include "util/kt_log.h"
#include "detailPlacer/kt_fastdp.h"
#include "legalizer/kt_abacus.h"
#include "placer/simpl/kt_simpl.h"
#include "datamodel/kt_graph.h"
#include "adaptor/bookshelfToKTAdaptor.h"
#include "adaptor/lefdefToKTAdaptor.h"
#include <map>
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
    bool runPlacement(const std::string &algorithm = "simpl", const std::string &plotDir = "",
                      const std::string &snapshotDir = "");
    /// Legalize and then detail-place, for the algorithms that stop at a global
    /// placement. Split out of runPlacement because RePlAce and SimPL both end
    /// here, and the reporting is identical -- a second copy would drift.
    bool legalizeAndDetail(const std::string &plotDir, const constraintMgr *fences);
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
        // What placement did the design actually ship with? Worth logging: if it
        // is missing or degenerate every placer silently falls back to its own
        // seed, which looks like a placer bug and is not one.
        {
            std::size_t moved = 0;
            double lo = 1e300, hi = -1e300, loY = 1e300, hiY = -1e300;
            const Graph &g = db.getGraph();
            for (std::size_t v = 0; v < g.getNumVertices(); ++v) {
                const Vertex &vert = g.getVertex(v);
                if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
                    continue;
                }
                lo = std::min(lo, vert.x);
                hi = std::max(hi, vert.x);
                loY = std::min(loY, vert.y);
                hiY = std::max(hiY, vert.y);
                moved += (vert.x != 0.0 || vert.y != 0.0) ? 1 : 0;
            }
            ktlog.echo(
                "Loaded placement: {}/{} cells carry a position, bbox "
                "x[{:.1f},{:.1f}] y[{:.1f},{:.1f}]",
                moved, g.getNumVertices(), lo, hi, loY, hiY);
        }
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


namespace {
/// One thing wrong with a finished placement.
struct Defect {
    std::string what;
    std::size_t count = 0;
};

/// Verify a finished placement against every constraint we know how to check.
///
/// The legalizer and the detailed placer each self-check, and their counts are
/// reported, but those are the checks each stage knew to ask about. This is the
/// independent pass over the placement as it will actually be written, and it asks
/// the question a reader of the output file would ask: is this legal?
///
/// It is deliberately not a summary of the stages' own numbers. A stage reporting
/// zero overlaps and the delivered file containing overlaps is exactly the failure
/// a summary cannot catch, and it is the failure that matters, because the file is
/// what the next tool reads. The stages' counters also cannot see a cell outside the
/// die or outside its fence -- neither is its job -- so those two are only ever
/// counted here.
std::vector<Defect> verifyPlacement(const PlacementDB &db, const constraintMgr *fences) {
    std::vector<Defect> defects;
    const Graph &g = db.getGraph();

    // --- cells outside the die, and outside their fence -----------------------
    BBox box = fixedCellBBox(g);
    const auto da = db.getDieArea();
    if (da.second.first > da.first.first && da.second.second > da.first.second) {
        box = {da.first.first, da.first.second, da.second.first, da.second.second};
    }
    std::size_t outOfDie = 0;
    std::size_t offFence = 0;
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        const double eps = 1e-6;
        if (vert.x < box[0] - eps || vert.y < box[1] - eps || vert.x + vert.width > box[2] + eps ||
            vert.y + vert.height > box[3] + eps) {
            ++outOfDie;
            continue;
        }
        if (fences != nullptr && vert.regionId != constraintMgr::kNoRegion) {
            std::vector<double> flat{vert.x, vert.y};
            std::vector<int> ids{vert.regionId};
            offFence += fences->countViolations(flat, ids);
        }
    }

    // --- overlapping pairs ---------------------------------------------------
    // A uniform grid over the cells, so this is linear in the number of cells
    // rather than quadratic. At 700k cells the pairwise version is not an option,
    // and a checker that cannot run on the largest design in the suite is not a
    // checker.
    std::size_t overlaps = 0;
    {
        const double bin = std::max(
            {1.0, std::sqrt((box[2] - box[0]) * (box[3] - box[1]) / std::max<std::size_t>(1, nv))});
        std::map<std::pair<long long, long long>, std::vector<std::size_t>> buckets;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
                continue;
            }
            const long long bx = static_cast<long long>(std::floor(vert.x / bin));
            const long long by = static_cast<long long>(std::floor(vert.y / bin));
            buckets[{bx, by}].push_back(v);
        }
        const double eps = 1e-9;
        const auto hits = [&](std::size_t a, std::size_t b) {
            const Vertex &p = g.getVertex(a);
            const Vertex &q = g.getVertex(b);
            return p.x < q.x + q.width - eps && q.x < p.x + p.width - eps &&
                   p.y < q.y + q.height - eps && q.y < p.y + p.height - eps;
        };
        for (const auto &[key, members] : buckets) {
            for (std::size_t i = 0; i < members.size(); ++i) {
                for (std::size_t j = i + 1; j < members.size(); ++j) {
                    overlaps += hits(members[i], members[j]) ? 1u : 0u;
                }
            }
            // A cell can only overlap one in the eight neighbouring buckets.
            for (long long dx = -1; dx <= 1; ++dx) {
                for (long long dy = -1; dy <= 1; ++dy) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    auto it = buckets.find({key.first + dx, key.second + dy});
                    if (it == buckets.end()) {
                        continue;
                    }
                    for (std::size_t a : members) {
                        for (std::size_t b : it->second) {
                            overlaps += hits(a, b) ? 1u : 0u;
                        }
                    }
                }
            }
        }
    }

    if (outOfDie > 0) {
        defects.push_back({"cells outside the die", outOfDie});
    }
    if (overlaps > 0) {
        defects.push_back({"overlapping cell pairs", overlaps});
    }
    if (offFence > 0) {
        defects.push_back({"cells outside their fence", offFence});
    }
    return defects;
}
}  // namespace

bool FlowMgr::Impl::legalizeAndDetail(const std::string &plotDir, const constraintMgr *fences) {
    // Everything the placer reserved is released here, so the legalizer and the
    // detailed placer can use the whole remaining budget.
    if (PlacementAnimator::instance().enabled()) {
        PlacementAnimator::instance().holdBack(0);
    }

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
        lparams.frameEvery =
            std::getenv("KTPLACE_ABACUS_FRAME_EVERY")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ABACUS_FRAME_EVERY")))
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
    lsummary.addRow({"commit failures", fmt::format("{}", lres.commitFailures)});
    lsummary.emit();
    if (lres.overlappingPairs != 0 || lres.offRow != 0 || lres.overFixed != 0) {
        ktlog.echo("WARNING: legalization is not legal; see counts above");
    }

    // The legalizer minimises displacement, not wirelength, so a legal
    // placement usually costs a little HPWL against the global placement it
    // came from. Detailed placement wins it back.
    ktlog.echo("Running FastDP detailed placement...");
    FastDetailedPlacer dp(*db);
    DetailPlaceParams dparams;
    if (const char *e = std::getenv("KTPLACE_DP_WINDOW")) {
        dparams.localReorderWindow = static_cast<std::size_t>(std::atoll(e));
    }
    if (!plotDir.empty()) {
        dparams.plotDir = plotDir + "/detailplace";
    }
    const DetailPlaceResult dres = dp.place(dparams);
    ktReportTable dsummary("Detailed placement (FastDP)");
    dsummary.setHeaders({"metric", "value"});
    dsummary.addRow({"global swaps", fmt::format("{}", dres.globalSwaps)});
    dsummary.addRow({"vertical swaps", fmt::format("{}", dres.verticalSwaps)});
    dsummary.addRow({"reorder moves", fmt::format("{}", dres.reorderMoves)});
    dsummary.addRow({"cluster moves", fmt::format("{}", dres.clusterMoves)});
    dsummary.addRow({"HPWL before", fmt::format("{:.6}", dres.hpwlBefore)});
    dsummary.addRow({"HPWL after", fmt::format("{:.6}", dres.hpwlAfter)});
    dsummary.addRow({"HPWL change",
                     fmt::format("{:.2}%", 100.0 * (dres.hpwlAfter - dres.hpwlBefore) /
                                               (dres.hpwlBefore > 0.0 ? dres.hpwlBefore : 1.0))});
    dsummary.addRow({"time (s)", fmt::format("{:.6}", dres.seconds)});
    dsummary.addRow({"overlapping pairs", fmt::format("{}", dres.overlappingPairs)});
    dsummary.addRow({"cells off row", fmt::format("{}", dres.offRow)});
    dsummary.addRow({"cells off site", fmt::format("{}", dres.offSite)});
    dsummary.addRow({"cells over macro", fmt::format("{}", dres.overFixed)});
    dsummary.emit();
    if (dres.overlappingPairs != 0 || dres.offRow != 0 || dres.overFixed != 0) {
        ktlog.echo("WARNING: detailed placement broke legality; see counts above");
    }
    // Independent check of the placement as it will be written. Last, so it sees
    // the effect of both stages, and separate from their own self-checks, so that a
    // disagreement between what a stage claims and what the file contains is visible
    // rather than averaged away.
    {
        const std::vector<Defect> defects = verifyPlacement(*db, fences);
        ktReportTable check("Placement check (independent, after legalization)");
        check.setHeaders({"check", "result"});
        if (defects.empty()) {
            check.addRow({"overlapping cell pairs", "0"});
            check.addRow({"cells outside the die", "0"});
            check.addRow({"cells outside their fence", fences != nullptr ? "0" : "n/a"});
            check.addRow({"verdict", "PASS"});
        } else {
            for (const Defect &d : defects) {
                check.addRow({d.what, fmt::format("{}", d.count)});
            }
            check.addRow({"verdict", "FAIL"});
        }
        check.emit();
        if (!defects.empty()) {
            ktlog.echo(
                "WARNING: the placement that will be written is not legal; see the "
                "placement check above");
        }
    }

    // Every stage has now contributed, so the run can be told as one animation.
    // Assembling it here, and not at the end of global placement, is the whole
    // point: the legalizer's pull back onto the rows is usually the most
    // consequential motion of the run, and it happens after the placer is done.
    if (const auto &anim = PlacementAnimator::instance(); anim.enabled()) {
        if (anim.finish()) {
            ktlog.echo(
                "animation: {} frames -> {}/anim/placement.gif (global placement, then "
                "legalization, then detailed placement)",
                anim.frameCount(), plotDir);
        } else {
            ktlog.echo(
                "animation: {} frame(s) recorded, no GIF written (one animation needs at "
                "least two frames)",
                anim.frameCount());
        }
    }
    placed = true;
    return true;
}

bool FlowMgr::Impl::runPlacement(const std::string &algorithm, const std::string &plotDir,
                                 const std::string &snapshotDir) {
    if (!loaded) {
        ktlog.fatal("No placement database loaded");
    }

    // One animation for the whole run, armed before the first stage and closed
    // after the last. Configuring it here, rather than inside a stage, is what
    // lets the legalizer and the detailed placer add frames to the same GIF the
    // placer started.
    if (!plotDir.empty() && std::getenv("KTPLACE_ANIM") != nullptr) {
        const std::size_t maxFrames =
            std::getenv("KTPLACE_ANIM_MAX_FRAMES")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ANIM_MAX_FRAMES")))
                : std::size_t{300};
        // 12 centiseconds (120 ms) per frame. The default 6 was quick enough that
        // a 300-frame animation flashed past in under two seconds, which is not
        // long enough to follow a placement moving.
        const int delayCs = std::getenv("KTPLACE_ANIM_DELAY_CS")
                                ? std::atoi(std::getenv("KTPLACE_ANIM_DELAY_CS"))
                                : 12;
        // Three frames per recorded placement: the two in-between plus the
        // placement itself, which is what turns a per-iteration jump into motion.
        const int blend =
            std::getenv("KTPLACE_ANIM_BLEND") ? std::atoi(std::getenv("KTPLACE_ANIM_BLEND")) : 3;
        PlacementAnimator::instance().configure(plotDir + "/anim", maxFrames, delayCs, blend);
    } else {
        PlacementAnimator::instance().reset();
    }

    // Global placement is by far the most frame-hungry stage, so it does not get
    // to spend the whole budget: a quarter is held back for legalization and
    // detailed placement, which are the stages that turn a legal-looking but
    // unusable placement into a real one.
    if (PlacementAnimator::instance().enabled()) {
        const std::size_t total =
            std::getenv("KTPLACE_ANIM_MAX_FRAMES")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ANIM_MAX_FRAMES")))
                : std::size_t{480};
        PlacementAnimator::instance().holdBack(total / 5);
    }

    if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        SimplePlacer placer(*db);
        // Fences come from the LEF/DEF reader; Bookshelf carries none, so there the
        // pointer is null and the design is correctly unconstrained. Passing them
        // is what makes a fenced ISPD 2015 design come out legal: the reader
        // stamps Vertex::regionId, and the placer both holds each cell inside its
        // region and draws the regions, so a run that ignores this silently scatters
        // fenced cells across the die and shows no fence at all.
        const constraintMgr *regions = nullptr;
        if (lefdefAdapter && lefdefAdapter->getConstraints().numRegions() > 0) {
            regions = &lefdefAdapter->getConstraints();
        }
        // KTPLACE_SIMPL_FENCES=0 turns the fences off, for a run that wants to see
        // what the placement would be without them -- the difference between the two
        // is the cost of the constraint, which is otherwise only visible as a
        // longer wirelength with nothing to attribute it to. Off means the fences are
        // neither enforced nor drawn, so the frames do not show regions that are not
        // being honoured; the report says "off" rather than leaving it ambiguous.
        const char *fenceEnv = std::getenv("KTPLACE_SIMPL_FENCES");
        const bool fencesOn = (fenceEnv == nullptr) || (std::atoi(fenceEnv) != 0);
        if (!fencesOn) {
            regions = nullptr;
            ktlog.echo("SimPL: fence enforcement DISABLED by KTPLACE_SIMPL_FENCES=0");
        }
        SimplParams params;
        params.traceEvery = 5;
        // Per-iteration frames, for watching the LSS/LAL interaction.
        if (const char *e = std::getenv("KTPLACE_SIMPL_TRACE_EVERY")) {
            params.traceEvery = static_cast<std::size_t>(std::atoll(e));
        }
        // A frame every N conjugate-gradient iterations inside each solve. On by
        // default now: a per-iteration record is what makes the animation show the
        // solve converging rather than only the outer loop, and with blending in
        // place the extra frames are what the motion is made of.
        params.cgEvery = 5;
        if (const char *e = std::getenv("KTPLACE_SIMPL_CG_EVERY")) {
            params.cgEvery = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY_MAPS")) {
            params.densityMaps = std::atoll(e) != 0;
        }
        const SimplResult res = placer.place(params, plotDir, snapshotDir, regions);
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
        summary.addRow({"look-ahead legalization", "", res.usedLookAhead ? "on" : "OFF (raw LSS)"});
        summary.addRow({"fence regions", "",
                        regions == nullptr ? (fencesOn ? "none" : "OFF (disabled)")
                                           : fmt::format("{}", regions->numRegions())});
        summary.addRow({"cells held in fence", "", fmt::format("{}", res.fenceClamps)});
        summary.addRow({"cells pushed out of a fence", "", fmt::format("{}", res.fencePushes)});
        summary.addRow(
            {"cells outside their fence at exit", "", fmt::format("{}", res.fenceViolations)});
        summary.addRow({"HPWL seed", fmt::format("{:.6}", res.hpwlSeed), ""});
        summary.addRow({"HPWL lower bound", "", fmt::format("{:.6}", res.hpwlLower)});
        summary.addRow({"HPWL final", "", fmt::format("{:.6}", res.hpwlFinal)});
        summary.addRow({"returned from iteration", "",
                        fmt::format("{} of {}", res.bestIter, res.globalIters)});
        summary.addRow({"bound gap", "", fmt::format("{:.6}", res.gap)});
        summary.addRow({"scaled overflow (lower)", "", fmt::format("{:.6}", res.overflowLower)});
        summary.addRow({"scaled overflow (final)", "", fmt::format("{:.6}", res.overflowFinal)});
        summary.addRow({"SVG frames written", "", fmt::format("{}", res.framesWritten)});
        summary.emit();

        legalizeAndDetail(plotDir, regions);
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
