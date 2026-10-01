// @file kt_flowMgr.cc// Implementation of FlowMgr


#include "kt_flowMgr.h"
#include "util/kt_reportTable.h"
#include "visualization/kt_animator.h"

#include "util/kt_scopedTimer.h"
#include "util/kt_log.h"
#include "detailPlacer/kt_fastdp.h"
#include "legalizer/kt_abacus.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/simpl/kt_simpl.h"

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

namespace {
/// One-line report of a single timer, issued when the phase named by @p name
/// completes, so a long run tells its cost as it goes rather than only in the
/// summary table at the end (TimerRegistry::report). The registry accumulates,
/// so this reports the phase's own totals at the moment they are final.
void reportPhase(const std::string &name) {
    if (const TimerStats *s = TimerRegistry::instance().find(name)) {
        ktlog.echo("phase {}: {:.3f}s wall, {:.3f}s cpu, {} call(s)", name, s->wallSeconds,
                   s->cpuSeconds, s->calls);
    }
}
/// Report what the design asks for before any placement runs, and say plainly when
/// the ask is impossible.
///
/// A legalizer that cannot legalize is usually not broken, it is being handed
/// something that does not fit. ibm01 packs its cells into 4.127e6 units of row
/// but its cells total 4.23e6, and 245 of them are four to nine rows tall against
/// a sixteen-unit row. Every downstream number -- overflow, the density term, the
/// legalizer's region growth, the final overlap count -- is then a measurement of
/// an impossible input, and the run finishes with an illegal placement and a
/// report full of confident numbers. Checking the arithmetic first costs nothing
/// and turns "the legalizer is broken" into "this design is 102% full".
struct Utilisation {
    double cellArea = 0.0;
    double fixedArea = 0.0;
    double rowArea = 0.0;
    double rowHeight = 0.0;
    std::size_t multiRow = 0;
    std::size_t cells = 0;
};

Utilisation measureUtilisation(const PlacementDB &db) {
    Utilisation u;
    const Graph &g = db.getGraph();
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        const double a = vert.width * vert.height;
        // A terminal is fixed area, not absent area. In the ISPD 2005 Bookshelf
        // suites the macros *are* the terminals -- adaptec1 carries no other fixed
        // cell, and its .pl marks exactly the 543 terminals and nothing else. So
        // skipping terminals here reported adaptec1's fixed area as zero, which
        // reads as "this design has no macros" when it has hundreds of them, and
        // understates the demand on the rows by all of their area.
        if (vert.isFixed || vert.isTerminal) {
            u.fixedArea += a;
        } else {
            u.cellArea += a;
            ++u.cells;
        }
    }
    double pitch = std::numeric_limits<double>::max();
    for (const PlacementDB::RowInfo &r : db.getRows()) {
        if (!(r.pitch() > 0.0) || !(r.height > 0.0)) {
            continue;
        }
        u.rowHeight = std::max(u.rowHeight, r.height);
        pitch = std::min(pitch, r.pitch());
        for (const PlacementDB::SubrowInfo &si : r.subrows) {
            if (si.xhi(r.pitch()) > si.xlo()) {
                u.rowArea += (si.xhi(r.pitch()) - si.xlo()) * r.height;
            }
        }
    }
    // Cells that cannot fit a single row. Counted here because it is the other
    // way a design can be unplaceable at any density.
    if (u.rowHeight > 0.0) {
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type == VertexType::Cell && !vert.isFixed && !vert.isTerminal &&
                vert.height > u.rowHeight * 1.5) {
                ++u.multiRow;
            }
        }
    }
    (void)pitch;
    return u;
}

void reportUtilisation(const PlacementDB &db) {
    const Utilisation u = measureUtilisation(db);
    // Movable demand against the rows, which is the number that decides whether
    // the design is placeable. The fixed cells are already placed: they are the
    // macros and the I/O pad ring, and they obstruct the rows rather than compete
    // for them, so charging their area to the row area double-counts it. On
    // adaptec1 that inflated the figure from 58% to 89%, which reads as a design
    // that barely fits when it has 40% of the rows to spare -- and it is the
    // number a reader goes to when a placement is illegal and the design is not.
    // Both are reported: the macro area is real and it is what makes a design hard,
    // but it is not demand for rows.
    const double util = u.rowArea > 0.0 ? 100.0 * u.cellArea / u.rowArea : 0.0;
    const double withFixed = u.rowArea > 0.0 ? 100.0 * (u.cellArea + u.fixedArea) / u.rowArea : 0.0;
    ktReportTable t("Design utilisation (before placement)");
    t.setHeaders({"measure", "value"});
    t.addRow({"movable cell area", fmt::format("{:.6e}", u.cellArea)});
    t.addRow({"fixed cell area", fmt::format("{:.6e}", u.fixedArea)});
    t.addRow({"row (placeable) area", fmt::format("{:.6e}", u.rowArea)});
    t.addRow({"utilisation (movable / rows)", fmt::format("{:.2}%", util)});
    t.addRow({"utilisation (incl. fixed cells)", fmt::format("{:.2}%", withFixed)});
    t.addRow({"movable cells", fmt::format("{}", u.cells)});
    if (u.rowHeight > 0.0) {
        t.addRow({"row height", fmt::format("{:.3}", u.rowHeight)});
        t.addRow({"cells taller than one row", fmt::format("{}", u.multiRow)});
    }
    t.emit();
    if (u.rowArea > 0.0 && util > 100.0) {
        ktlog.warning(
            "the design needs {:.6e} of movable cell area but only {:.6e} of row is placeable, "
            "so it is {:.1f}% full. No legal placement exists for this input: the cells do not "
            "fit, however the placer is retried.",
            u.cellArea, u.rowArea, util);
    }
    if (u.multiRow > 0) {
        // Said here, before placement runs, rather than only after legalization
        // fails: this is a property of the input, so the reader learns it before
        // spending several minutes on a global placement that cannot end legal.
        ktlog.warning(
            "{} cell(s) are taller than one row (row height {:.3}) and the legalizer only "
            "places into single rows, so those cells will be left unplaced and the result will "
            "not be legal. Legalizing this design needs a multi-height legalizer, which this "
            "build does not have.",
            u.multiRow, u.rowHeight);
    }
}
}  // namespace

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

    /// HPWL after legalization and detailed placement -- the number the paper
    /// reports, and the only one that ranks two global-placement runs. Kept so
    /// the summary can show it next to SimPL's own upper bound, because the two
    /// differ by more than the differences being compared: the upper bound is
    /// the look-ahead legalized placement, taken before either stage runs.
    double hpwlFinalPlaced_ = -1.0;
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
    reportPhase("load");

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
        // Before the placer, not after: once the solver is running, every number
        // downstream is derived from a density model, and on an over-full design
        // that model is describing an impossibility. The reader needs to know the
        // design did not fit before they read a wirelength off it.
        reportUtilisation(*pImpl->db);
        if (!pImpl->runPlacement(algorithm, effectivePlotDir, snapshotDir)) {
            throw std::runtime_error("Placement algorithm failed");
        }
    }
    reportPhase("place");

    // Write output
    {
        ScopedTimer timer("write");
        if (!pImpl->writePlacement(outputPath, outputFormat)) {
            throw std::runtime_error("Failed to write output");
        }
    }
    reportPhase("write");

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
    // The same definition the placer used. Two different notions of "the die"
    // mean a legal placement can be reported as illegal: the placer spreads over
    // the union of the fixed geometry and the rows, and a checker built from the
    // fixed geometry alone fails every cell in a row that reaches past the pads.
    const std::array<double, 4> box = placementDieBox(db);
    std::size_t outOfDie = 0;
    std::size_t offFence = 0;
    const std::size_t nv = g.getNumVertices();
    // Row height, for deciding what counts as a tall cell below.
    double rowHeight = 0.0;
    for (const PlacementDB::RowInfo &ri : db.getRows()) {
        if (ri.height > rowHeight) {
            rowHeight = ri.height;
        }
    }
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
    //
    // Three things have to be true at once for this to be usable on a real suite:
    // it must not miss a pair, it must not count one twice, and it must finish on
    // a 400k-cell design. The version before this was wrong twice. It sized the
    // grid bin from the LARGEST cell so the neighbour search would be exhaustive,
    // which is sound but degenerate: ibm01 carries a single cell 12752 units tall,
    // so the bin became the size of the die, every cell landed in one bucket, and
    // the check became a 12500^2 pairwise comparison. It also reported "no
    // overlap" on inputs where a frame plainly showed a cell on a macro, because
    // it skipped terminals, and in the Bookshelf suites the macros ARE the
    // terminals.
    //
    // So: bulk cells go in a grid with a bin sized for a typical cell, and every
    // pair is examined once by looking inside a bucket and at the four forward
    // neighbours. Tall cells are not in the grid at all; they are rare, and a tall
    // cell cannot be found by a small-bin search, so they are compared against
    // everything directly.
    std::size_t overlaps = 0;
    {
        double bin = 0.0;
        std::size_t nTall = 0;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell) {
                continue;
            }
            // "Tall" is judged against the rows, not an absolute number: a cell
            // more than four rows high cannot be located by a bin sized for a
            // standard cell, whatever its width.
            const double rows = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (rows > 4.0) {
                ++nTall;
                continue;
            }
            bin += vert.width * vert.height;
        }
        const double meanArea = nv > 0 ? bin / std::max<std::size_t>(1, nv - nTall) : 0.0;
        double cell = meanArea > 0.0 ? std::sqrt(meanArea) : 1.0;
        const double dieW = std::max(box[2] - box[0], 1.0);
        const double dieH = std::max(box[3] - box[1], 1.0);
        // Aim for a few cells per bucket, but never so many buckets that the map
        // dominates, and never a bin so fine that a standard cell spans many of
        // them (which is what made the original miss pairs).
        const std::size_t target = 64;
        cell = std::max(cell, std::max(dieW, dieH) / 512.0);
        cell = std::max(cell, 1e-9);
        (void)target;

        std::map<std::pair<long long, long long>, std::vector<std::size_t>> buckets;
        std::vector<std::size_t> tallCells;
        std::vector<char> isTall(nv, 0);
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell) {
                continue;
            }
            const double rows = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (rows > 4.0) {
                tallCells.push_back(v);
                isTall[v] = 1;
                continue;
            }
            const long long bx = static_cast<long long>(std::floor(vert.x / cell));
            const long long by = static_cast<long long>(std::floor(vert.y / cell));
            buckets[{bx, by}].push_back(v);
        }
        const double eps = 1e-9;
        const auto hits = [&](std::size_t a, std::size_t b) {
            const Vertex &p = g.getVertex(a);
            const Vertex &q = g.getVertex(b);
            // Two fixed cells overlapping each other is the input's business, not
            // the placer's, so it is not reported.
            if (p.isFixed && q.isFixed) {
                return false;
            }
            return p.x < q.x + q.width - eps && q.x < p.x + p.width - eps &&
                   p.y < q.y + q.height - eps && q.y < p.y + p.height - eps;
        };
        // Only forward neighbours: scanning all eight examines every cross-bucket
        // pair twice, once from each side.
        static const int kFwd[4][2] = {{1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (const auto &kv : buckets) {
            const std::vector<std::size_t> &mine = kv.second;
            for (std::size_t a = 0; a < mine.size(); ++a) {
                for (std::size_t b = a + 1; b < mine.size(); ++b) {
                    overlaps += hits(mine[a], mine[b]) ? 1u : 0u;
                }
            }
            for (const auto &d : kFwd) {
                auto it = buckets.find({kv.first.first + d[0], kv.first.second + d[1]});
                if (it == buckets.end()) {
                    continue;
                }
                for (const std::size_t a : mine) {
                    for (const std::size_t b : it->second) {
                        overlaps += hits(a, b) ? 1u : 0u;
                    }
                }
            }
        }
        // Tall cells against everything.
        //
        // The scan is over ALL vertices, not just those above `a` in index order.
        // Restricting it to b > a silently skipped every pair of a tall cell with
        // a normal cell of lower index, because the normal cells are not in the
        // grid either (the grid pairs normal cells with normal cells only). That
        // is where a count of 558 came from an input that a direct scan of the
        // written file put at 827. A checker that under-counts is the failure mode
        // this whole routine exists to prevent, so the rule is instead: compare
        // against every cell, and accept a tall-tall pair only once by index.
        for (const std::size_t a : tallCells) {
            for (std::size_t b = 0; b < nv; ++b) {
                if (b == a) {
                    continue;
                }
                const Vertex &q = g.getVertex(b);
                if (q.type != VertexType::Cell) {
                    continue;
                }
                // Both tall: the same pair is reached from both sides, so keep
                // only the one where this is the lower vertex index.
                if (isTall[b] && b < a) {
                    continue;
                }
                if (hits(a, b)) {
                    ++overlaps;
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
    lparams.constraints = fences;
    if (!plotDir.empty()) {
        lparams.plotDir = plotDir + "/legalize";
        lparams.frameEvery =
            std::getenv("KTPLACE_ABACUS_FRAME_EVERY")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ABACUS_FRAME_EVERY")))
                : 20000;
    }
    const LegalizeResult lres = legalizer.legalize(lparams);
    reportPhase("legalize");
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
        ktlog.warning("legalization is not legal; see the counts above");
    }

    // Abacus places into single rows, so a cell taller than one row has nowhere
    // to go: it is reported unplaced and stays where global placement left it,
    // overlapping whatever is there. On ibm01 that is 120 cells, and the
    // legalization check below fails on exactly those.
    //
    // There is no multi-row legalizer here. That is a known limitation of this
    // build, not something a caller can turn on, so it is reported as a warning
    // naming the cells that will be left behind rather than left to be inferred
    // from a count in a table.
    if (lres.outOfRows > 0) {
        // The count, after the fact, so the pre-flight warning above can be
        // cross-checked against what actually happened rather than trusted.
        ktlog.warning(
            "{} cell(s) taller than one row could not be placed and are still at their global "
            "placement positions. The placement is not legal; see \"cells out of rows\" above.",
            lres.outOfRows);
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
    dparams.constraints = fences;
    if (!plotDir.empty()) {
        dparams.plotDir = plotDir + "/detailplace";
    }
    const DetailPlaceResult dres = dp.place(dparams);
    reportPhase("detail-place");
    ktReportTable dsummary("Detailed placement (FastDP)");
    dsummary.setHeaders({"metric", "value"});
    dsummary.addRow({"global swaps", fmt::format("{}", dres.globalSwaps)});
    dsummary.addRow({"vertical swaps", fmt::format("{}", dres.verticalSwaps)});
    dsummary.addRow({"reorder moves", fmt::format("{}", dres.reorderMoves)});
    dsummary.addRow({"cluster moves", fmt::format("{}", dres.clusterMoves)});
    dsummary.addRow({"HPWL before", fmt::format("{:.6}", dres.hpwlBefore)});
    hpwlFinalPlaced_ = dres.hpwlAfter;
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
        ktlog.warning("detailed placement broke legality; see the counts above");
    }
    // Independent check of the placement as it will be written. Last, so it sees
    // the effect of both stages, and separate from their own self-checks, so that a
    // disagreement between what a stage claims and what the file contains is visible
    // rather than averaged away.
    {
        // Timed on its own because it is the one phase that reads the whole
        // placement and changes nothing. It is also the phase that goes quadratic
        // if the spatial index degrades, so its cost has to be visible: a run that
        // suddenly takes twenty minutes longer is the checker, not the placer.
        const ScopedTimer checkTimer("place-check");
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
            ktlog.warning(
                "the placement that will be written is not legal; see the placement check "
                "above");
        }
        reportPhase("place-check");
    }

    // The last thing that moves cells has now run and been verified, so this is
    // the finished placement. Write one high-resolution still of it: the
    // animation frames are kept small because a GIF has to be, and at that size
    // a big design's cells collapse to a pixel each, so the "look at the result"
    // picture is rendered separately at a larger scale. The zoom applies to both
    // image axes, so the default of 8 turns the 768x768 frame into a
    // 6144x6144 still -- on adaptec1 that is about fourteen pixels across for a
    // standard cell, which is the point at which the cells stop being a texture
    // and start being cells. It is written once per run and compresses to a
    // couple of megabytes as a PNG, so the resolution costs disk, not time.
    if (!plotDir.empty()) {
        double zoom = 8.0;
        if (const char *e = std::getenv("KTPLACE_FINAL_ZOOM")) {
            const double v = std::atof(e);
            if (v >= 1.0) {
                zoom = v;
            }
        }
        {
            ScopedTimer finalTimer("final-image");
            const std::string finalDir = plotDir + "/final";
            std::error_code ec;
            std::filesystem::create_directories(finalDir, ec);
            if (ec) {
                ktlog.warning(
                    "cannot create the final-image plot directory '{}': {}. "
                    "No high-resolution final still written.",
                    finalDir, ec.message());
            } else {
                // The finished placement is the picture worth keeping, and a PPM is
                // not one: no browser and no image viewer opens it. So the final
                // still is written as a PNG, which is also small -- a placement
                // frame is flat colour and compresses to a couple of megabytes at
                // this resolution.
                writeFinalFrameRaster(finalDir + "/final.png", db->getGraph(), fences, zoom,
                                      dres.hpwlAfter);
                // The vector form of the same picture, and the one to open when a
                // region has to be looked at closely. It is also the exact record
                // of the drawing -- one <rect> per cell -- so "every cell is in
                // the picture" is a count rather than an estimate, which is what
                // the smoke test in CI checks.
                writeFinalFrameSvg(finalDir + "/final.svg", db->getGraph(), fences, dres.hpwlAfter);
                // The lossless copy beside it. At 6144x6144 that is 113 MB of
                // raw pixels, so it is written only on request: the PNG is the
                // artefact anyone looks at, and the PPM exists for tooling that
                // wants to measure the image rather than view it.
                if (std::getenv("KTPLACE_FINAL_PPM") != nullptr) {
                    writeFinalFrameRaster(finalDir + "/final.ppm", db->getGraph(), fences, zoom,
                                          dres.hpwlAfter);
                }
                ktlog.echo("final high-resolution image: {}/final.png ({}x{})", finalDir,
                           static_cast<int>(std::lround(zoom * 768.0)),
                           static_cast<int>(std::lround(zoom * 768.0)));
            }
        }
        reportPhase("final-image");
    }

    // Every stage has now contributed, so the run can be told as one animation.
    // Assembling it here, and not at the end of global placement, is the whole
    // point: the legalizer's pull back onto the rows is usually the most
    // consequential motion of the run, and it happens after the placer is done.
    if (const auto &anim = PlacementAnimator::instance(); anim.enabled()) {
        // Timed because encoding the GIF is pure output cost: it can be longer
        // than the detailed placement on a small design, and without a number here
        // it looks like the run hung at the end.
        ScopedTimer animTimer("anim-finish");
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
        reportPhase("anim-finish");
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
    // Set from the byte target below, and read again where the budget is split
    // between the stages. Declared here so both uses see the same number: the
    // split used to fall back to a hardcoded 600 while the animator was
    // configured from a different count, so the two could disagree about how many
    // frames the run was allowed.
    std::size_t animFrameBudget = 0;
    // The value is read, not just its presence. KTPLACE_ANIM=0 tested as
    // "variable exists", so every experiment run with it off still spent the
    // encode time writing a 40-120 MB GIF -- and the GIF it wrote was the run's
    // output, so a directory kept a GIF from a run that had asked for none.
    // On by default. The value is read rather than merely tested for presence --
    // KTPLACE_ANIM=0 used to enable the animation, because "the variable exists"
    // was the whole test -- and the default is on because the frames and the GIF
    // are how a run is inspected, while a caller who wants them gone sets 0.
    const char *animEnv = std::getenv("KTPLACE_ANIM");
    const bool animOn = animEnv == nullptr || std::atoi(animEnv) != 0;
    if (!plotDir.empty() && animOn) {
        // 1200 frames by default, raised from 300. The budget is what decides how
        // much of the run the animation actually shows: global placement records a
        // frame per solve iteration and detailed placement one per pass, and at 300
        // a run of a few dozen iterations spends the lot before legalization
        // begins, so the GIF stops where it becomes interesting. Frames cost
        // encoding time and file size, not correctness, so the trade is made in
        // favour of showing the run -- and KTPLACE_ANIM_MAX_FRAMES still caps it
        // for a quick look.
        // Resolved before the budget, because the budget is derived from it.
        //
        // Animation frame scale, against the 768x768 frame size, and two rather
        // than three. Every conjugate-gradient iterate of every solve is now kept,
        // which on adaptec1 is a few hundred frames rather than 12; at 2304 each
        // one costs ~0.6 MB and the file passes 150 MB, which no browser opens. Two
        // is the largest scale at which the full sequence still fits the byte
        // budget below, and it keeps a standard cell at a couple of pixels rather
        // than one. KTPLACE_ANIM_ZOOM overrides it if the trade is worth making.
        double animZoom = 2.0;
        if (const char *e = std::getenv("KTPLACE_ANIM_ZOOM")) {
            const double v = std::atof(e);
            if (v >= 1.0) {
                animZoom = v;
            }
        }
        // Byte target for the finished GIF, overridable. 64 MB opens in a browser
        // and in a file manager's preview.
        const std::size_t animGifByteCap =
            std::getenv("KTPLACE_ANIM_MAX_BYTES")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ANIM_MAX_BYTES")))
                : std::size_t{96} << 20;

        // Budget the GIF by the bytes it will actually be, not by a frame count.
        // The frames are nearly incompressible -- a placement frame is flat
        // colour and GIF's LZW still needs about 0.9 bits per pixel on it -- so
        // the file size is essentially width * height * frames / 8, and a frame
        // count alone cannot keep it in bounds. At 2304x2304 the old 1200-frame
        // budget allowed 8 GB; the run that produced 208 frames wrote 122 MB,
        // which no browser will open. Sizing the budget to a byte target keeps the
        // resolution, which is what makes the animation worth looking at, and cuts
        // the frame count instead.
        const double animW = 768.0 * animZoom;
        const double animH = 768.0 * animZoom;
        const double kBytesPerPixel = 0.9 / 8.0;  // measured, not assumed
        // bytes = width * height * bytesPerPixel * frames, so the frame count that
        // fills the budget divides it out. Multiplying by the bytes-per-pixel
        // instead gave 12 frames, which is a flicker and not an animation.
        const std::size_t sizeCapFrames = static_cast<std::size_t>(
            static_cast<double>(animGifByteCap) / (animW * animH * kBytesPerPixel));
        const std::size_t maxFrames =
            std::getenv("KTPLACE_ANIM_MAX_FRAMES")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ANIM_MAX_FRAMES")))
                : std::max<std::size_t>(12, sizeCapFrames);
        animFrameBudget = maxFrames;
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
        PlacementAnimator::instance().configure(plotDir + "/anim", maxFrames, delayCs, blend,
                                                animZoom);
    } else {
        PlacementAnimator::instance().reset();
    }

    // Global placement is by far the most frame-hungry stage, so it does not get
    // to spend the whole budget: a quarter is held back for legalization and
    // detailed placement, which are the stages that turn a legal-looking but
    // unusable placement into a real one.
    if (PlacementAnimator::instance().enabled()) {
        const std::size_t total = animFrameBudget;
        // Two fifths held back, rather than the fifth it used to be. Global
        // placement records a frame per solve iteration, so on a run of a few
        // dozen iterations it can spend anything it is given and leave nothing:
        // with a fifth held back the legalizer and the detailed placer -- the two
        // stages that actually turn a legal-looking placement into a legal one --
        // were getting a handful of frames between them, and the animation ended
        // exactly where it becomes interesting. Two fifths gives the later stages
        // room to be seen while still leaving global placement the majority.
        PlacementAnimator::instance().holdBack(total * 2 / 5);
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
        // A frame every N conjugate-gradient iterations inside each solve, and N is
        // 1: every iteration of every round is offered to the animation, which is
        // what makes the GIF show the solve converging rather than only the outer
        // loop. The animator subsamples to fit the byte budget, so offering
        // everything costs thinning, not truncation.
        params.cgEvery = 1;
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
        if (hpwlFinalPlaced_ > 0.0) {
            // Rankable result, beside the bound it is derived from, so a change
            // can be judged on the metric the paper publishes rather than on the
            // intermediate upper bound, which moves for reasons that do not
            // survive legalization.
            ktReportTable placed("Placement quality (after legalization and detail placement)");
            placed.setHeaders({"metric", "value"});
            placed.addRow({"HPWL detailed", fmt::format("{:.6}", hpwlFinalPlaced_)});
            placed.addRow({"HPWL global upper bound", fmt::format("{:.6}", res.hpwlFinal)});
            placed.addRow({"legalization + detail change",
                           fmt::format("{:.2}%", 100.0 * (hpwlFinalPlaced_ - res.hpwlFinal) /
                                                     (res.hpwlFinal > 0.0 ? res.hpwlFinal : 1.0))});
            placed.addRow({"paper reference (adaptec1)", "77410738"});
            placed.emit();
        }
        return true;

    } else if (algorithm == "ntuplace1") {
        ktlog.echo("Running NTUPlace1 global placement (ratio partitioning)...");
        RatioPlacer placer(*db);
        const constraintMgr *regions = nullptr;
        if (lefdefAdapter && lefdefAdapter->getConstraints().numRegions() > 0) {
            regions = &lefdefAdapter->getConstraints();
        }
        RatioPlaceParams params;
        params.plotDir = plotDir;
        if (const char *e = std::getenv("KTPLACE_NTU_LEAF_CELLS")) {
            params.targetLeafCells = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_MAX_LEVELS")) {
            params.maxLevels = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_RETRIES")) {
            params.maxRatioRetries = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_MIN_NET_WEIGHT")) {
            params.minNetWeight = std::atof(e);
        }
        if (const char *e = std::getenv("KTPLACE_NTU_VERBOSE")) {
            params.verbose = std::atoll(e) != 0;
        }
        const RatioPlaceResult res = placer.place(params, regions);
        ktReportTable summary("NTUplace1 solver results");
        summary.setHeaders({"metric", "value"});
        summary.addRow({"movable cells", fmt::format("{}", res.numMovable)});
        summary.addRow({"fixed cells", fmt::format("{}", res.numFixed)});
        summary.addRow({"hypergraph nets", fmt::format("{}", res.nets)});
        summary.addRow({"cuts accepted", fmt::format("{}", res.cuts)});
        summary.addRow({"ratio retries", fmt::format("{}", res.ratioRetries)});
        summary.addRow({"retries per cut", fmt::format("{:.3}", res.meanImbalance)});
        summary.addRow({"recursion depth reached", fmt::format("{}", res.maxDepth)});
        summary.addRow({"smallest leaf", fmt::format("{}", res.minLeafCells)});
        summary.addRow({"HPWL (pre-legalization)", fmt::format("{:.6}", res.hpwlFinal)});
        summary.addRow({"paper reference (adaptec1)", "44800000"});
        summary.emit();

        // The ratio partitioner deliberately leaves the design overfull: GP here is
        // only a geometric ordering, and the legalizer plus detail placer is what
        // turns it into a legal placement. That is the same split the paper uses.
        legalizeAndDetail(plotDir, regions);
        if (hpwlFinalPlaced_ > 0.0) {
            ktReportTable placed("Placement quality (after legalization and detail placement)");
            placed.setHeaders({"metric", "value"});
            placed.addRow({"HPWL detailed", fmt::format("{:.6}", hpwlFinalPlaced_)});
            placed.addRow({"HPWL from partitioning", fmt::format("{:.6}", res.hpwlFinal)});
            placed.addRow({"legalization + detail change",
                           fmt::format("{:.2}%", 100.0 * (hpwlFinalPlaced_ - res.hpwlFinal) /
                                                     (res.hpwlFinal > 0.0 ? res.hpwlFinal : 1.0))});
            placed.emit();
        }
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
