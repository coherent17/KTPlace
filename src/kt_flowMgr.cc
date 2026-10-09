// @file kt_flowMgr.cc
// Implementation of FlowMgr

#include "kt_flowMgr.h"

#include "kt_option.h"

#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "detailPlacer/kt_fastdp.h"
#include "legalizer/kt_abacus.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/simpl/kt_simpl.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_animator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

class FlowMgr::Impl {
public:
    std::unique_ptr<PlacementDB> db;
    std::unique_ptr<InputReader> input;
    bool loaded = false;
    bool placed = false;

    void run(const kt_option &options);
    void loadDesign(const std::string &dirPath);
    void reportDesign();
    const constraintMgr *fences() const;
    void placeDesign(const kt_option &options);
    void writeDesign(const std::string &outputPath);
    void runPlacement(const std::string &algorithm, const std::string &plotDir,
                      const std::string &snapshotDir);
    void legalizeDesign(const std::string &plotDir, const constraintMgr *fences);
    void detailPlaceDesign(const std::string &plotDir, const constraintMgr *fences);
    void reportPlacementQuality();
    void checkDesign(const constraintMgr *fences);
    void renderFinalImage(const std::string &plotDir, const constraintMgr *fences);
    void finishAnimation(const std::string &plotDir);
    double hpwlFinalPlaced_ = -1.0;
    double hpwlGlobalBound_ = -1.0;
    void writePlacement(const std::string &outputPath);
    void clear();
};

FlowMgr::FlowMgr() : pImpl(std::make_unique<Impl>()) {
    pImpl->db = std::make_unique<PlacementDB>();
}

FlowMgr::~FlowMgr() = default;
FlowMgr::FlowMgr(FlowMgr &&) noexcept = default;
FlowMgr &FlowMgr::operator=(FlowMgr &&) noexcept = default;

void FlowMgr::run(const kt_option &options) {
    pImpl->run(options);
}

void FlowMgr::Impl::run(const kt_option &options) {
    loadDesign(options.inputPath);
    reportDesign();
    placeDesign(options);
    legalizeDesign(options.getPlotDir(), fences());
    detailPlaceDesign(options.getPlotDir(), fences());
    reportPlacementQuality();
    checkDesign(fences());
    renderFinalImage(options.getPlotDir(), fences());
    finishAnimation(options.getPlotDir());
    writeDesign(options.getOutputPath());
    TimerRegistry::instance().report();
}

const constraintMgr *FlowMgr::Impl::fences() const {
    return input ? input->constraints() : nullptr;
}

void FlowMgr::Impl::reportDesign() {
    // Said before the placer, not after: once the solver is running, every number
    // downstream is derived from a density model, and on an over-full design that
    // model is describing an impossibility.
    db->report();
    db->reportUtilisation();
}

void FlowMgr::Impl::loadDesign(const std::string &dirPath) {
    ScopedTimer timer("load");
    clear();

    // A missing directory and an unrecognised one are different mistakes, and say
    // different things to whoever has to fix it.
    std::error_code ec;
    if (!std::filesystem::is_directory(dirPath, ec)) {
        ktlog.fatal("cannot read input directory: {}", dirPath);
    }

    input = makeInputReader(dirPath);
    if (!input) {
        ktlog.fatal("no input format recognises {}", dirPath);
    }
    ktlog.echo("Reading {} input from {}", input->formatName(), dirPath);

    db = input->read(dirPath);
    if (!db) {
        ktlog.fatal("Failed to read {} design from {}", input->formatName(), dirPath);
    }
    loaded = true;
}

void FlowMgr::Impl::placeDesign(const kt_option &options) {
    ScopedTimer timer("place");
    const std::filesystem::path out(options.getOutputPath());
    const std::string snapshotDir = out.parent_path().empty() ? "." : out.parent_path().string();

    runPlacement(options.algorithm, options.getPlotDir(), snapshotDir);
}

void FlowMgr::Impl::writeDesign(const std::string &outputPath) {
    ScopedTimer timer("write");
    writePlacement(outputPath);
}

namespace {

struct Defect {
    std::string what;
    std::size_t count = 0;
};

// Independent pass over the placement as it will be written. The stages each
// self-check, but only for what they knew to ask about: a stage reporting zero
// overlaps while the delivered file has them is the failure worth catching, since
// the file is what the next tool reads. Off-die and out-of-fence cells are also
// only counted here.
std::vector<Defect> verifyPlacement(const PlacementDB &db, const constraintMgr *fences) {
    std::vector<Defect> defects;
    const Graph &g = db.getGraph();

    // The same notion of "the die" the placer used. Two different ones would fail
    // a legal placement: the placer spreads over the union of the fixed geometry
    // and the rows, so a checker built from the
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

    // Overlaps. Bulk cells go in a grid with a bin sized for a typical cell, and
    // each pair is examined once, in its bucket and the four forward neighbours.
    // The bin must NOT be sized from the largest cell: ibm01 has one cell 12752
    // units tall, so that makes the bin the size of the die and the check a
    // 12500^2 comparison. Tall cells are outside the grid for the same reason and
    // are compared against everything directly.
    std::size_t overlaps = 0;
    {
        double bin = 0.0;
        std::size_t nTall = 0;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell) {
                continue;
            }
            // "Tall" against the rows, not an absolute height: a cell more than
            // four rows high cannot be found by a standard-cell-sized bin.
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
        // A few cells per bucket: enough that the map does not dominate, coarse
        // enough that a standard cell does not span many bins (which is what made
        // the original miss pairs).
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
            // Two fixed cells overlapping is the input's business, not ours.
            if (p.isFixed && q.isFixed) {
                return false;
            }
            return p.x < q.x + q.width - eps && q.x < p.x + p.width - eps &&
                   p.y < q.y + q.height - eps && q.y < p.y + p.height - eps;
        };
        // Only forward neighbours: all eight examines each cross-bucket pair twice.
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
        // Tall cells against every vertex, not just those with a higher index.
        // Restricting to b > a misses a tall cell paired with a lower-indexed
        // normal cell, since the grid pairs normal cells with normal cells only
        // (558 counted where a direct scan found 827). A tall-tall pair is still
        // accepted only once, by index.
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

void FlowMgr::Impl::legalizeDesign(const std::string &plotDir, const constraintMgr *fences) {
    // Whatever the placer reserved is released, so the later stages get the whole
    // remaining budget.
    if (PlacementAnimator::instance().enabled()) {
        PlacementAnimator::instance().holdBack(0);
    }

    // Abacus removes the overlap global placement left, with the least movement
    // it can, and self-checks so a legalization bug surfaces as a count.
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

    // Abacus places into single rows, so a cell taller than one row stays where
    // global placement left it, overlapping (120 on ibm01). There is no multi-row
    // legalizer in this build, so the affected cells are named rather than left to
    // be inferred from a count.
    if (lres.outOfRows > 0) {
        // After the fact, so the pre-flight estimate above can be checked.
        ktlog.warning(
            "{} cell(s) taller than one row could not be placed and are still at their global "
            "placement positions. The placement is not legal; see \"cells out of rows\" above.",
            lres.outOfRows);
    }
}

void FlowMgr::Impl::detailPlaceDesign(const std::string &plotDir, const constraintMgr *fences) {
    // Abacus minimises displacement, not wirelength, so legalization usually
    // costs a little HPWL; detailed placement wins it back.
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

    // The last stage that moves cells has now run, so this is the finished
    // placement and may be written.
    placed = true;
}

void FlowMgr::Impl::reportPlacementQuality() {
    if (hpwlFinalPlaced_ <= 0.0 || hpwlGlobalBound_ <= 0.0) {
        return;
    }
    // Rankable result, beside the bound it is derived from, so a change can be
    // judged on the metric the paper publishes rather than on the intermediate
    // upper bound, which moves for reasons that do not survive legalization.
    ktReportTable t("Placement quality (after legalization and detail placement)");
    t.setHeaders({"metric", "value"});
    t.addRow({"HPWL detailed", fmt::format("{:.6}", hpwlFinalPlaced_)});
    t.addRow({"HPWL global upper bound", fmt::format("{:.6}", hpwlGlobalBound_)});
    t.addRow({"legalization + detail change",
              fmt::format("{:.2}%", 100.0 * (hpwlFinalPlaced_ - hpwlGlobalBound_) /
                                        (hpwlGlobalBound_ > 0.0 ? hpwlGlobalBound_ : 1.0))});
    t.addRow({"paper reference (adaptec1)", "77410738"});
    t.emit();
}

void FlowMgr::Impl::checkDesign(const constraintMgr *fences) {
    // Last, so it sees the effect of both stages, and separate from their
    // self-checks, so a disagreement between what a stage claims and what the file
    // contains stays visible.
    {
        // Timed on its own: it reads the whole placement, changes nothing, and is
        // the phase that goes quadratic if the spatial index degrades.
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
    }
}

void FlowMgr::Impl::renderFinalImage(const std::string &plotDir, const constraintMgr *fences) {
    // A separate high-resolution still, because GIF frames have to stay small and
    // a big design's cells collapse to a pixel each at that size. The zoom is on
    // both axes: 8 turns a 768x768 frame into 6144x6144, about fourteen pixels
    // per standard cell. Written once, and it compresses to a couple of MB.
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
                // PNG, not PPM: no viewer opens a PPM, and flat colour compresses
                // to a couple of megabytes at this resolution.
                writeFinalFrameRaster(finalDir + "/final.png", db->getGraph(), fences, zoom,
                                      hpwlFinalPlaced_);
                // The vector form, and the one to open when a region needs looking
                // at closely. One <rect> per cell, so "every cell is in the
                // picture" is a count, which is what the CI smoke test checks.
                writeFinalFrameSvg(finalDir + "/final.svg", db->getGraph(), fences,
                                   hpwlFinalPlaced_);
                // 113 MB of raw pixels at this size, so only on request: the PNG is
                // the artefact anyone looks at.
                if (std::getenv("KTPLACE_FINAL_PPM") != nullptr) {
                    writeFinalFrameRaster(finalDir + "/final.ppm", db->getGraph(), fences, zoom,
                                          hpwlFinalPlaced_);
                }
                ktlog.echo("final high-resolution image: {}/final.png ({}x{})", finalDir,
                           static_cast<int>(std::lround(zoom * 768.0)),
                           static_cast<int>(std::lround(zoom * 768.0)));
            }
        }
    }
}

void FlowMgr::Impl::finishAnimation(const std::string &plotDir) {
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
    }
}

void FlowMgr::Impl::runPlacement(const std::string &algorithm, const std::string &plotDir,
                                 const std::string &snapshotDir) {
    if (!loaded) {
        ktlog.fatal("No placement database loaded");
    }

    // Configured here rather than inside a stage, so the legalizer and the
    // detailed placer add to the same GIF. Declared before both the animator setup
    // and the budget split below, which used to disagree about the frame count.
    std::size_t animFrameBudget = 0;
    // The value is read, not just its presence: KTPLACE_ANIM=0 used to enable the
    // animation, so every run meant to skip it still spent the encode time.
    const char *animEnv = std::getenv("KTPLACE_ANIM");
    const bool animOn = animEnv == nullptr || std::atoi(animEnv) != 0;
    if (!plotDir.empty() && animOn) {
        // The budget decides how much of the run the GIF shows. At 300 a run of a
        // few dozen iterations spent the lot before legalization began, so it
        // stopped where it gets interesting. Frames cost time and size, not
        // correctness. KTPLACE_ANIM_MAX_FRAMES still caps it for a quick look.
        //
        // Frame scale against the 768x768 frame size. Two is the largest at which
        // a few hundred CG frames still fit the byte budget below and a standard
        // cell stays a couple of pixels rather than one.
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

        // Budget by bytes, not by frame count. Placement frames are nearly
        // incompressible, so size is essentially w * h * frames / 8: a frame count
        // alone allowed an 8 GB file. Sizing to a byte target keeps the resolution
        // and cuts the frame count instead.
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
        // Two fifths held back. Global placement records a frame per solve
        // iteration and can spend anything it is given; with a fifth held back the
        // stages that make the placement legal got a handful of frames.
        PlacementAnimator::instance().holdBack(total * 2 / 5);
    }

    if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        SimplePlacer placer(*db);
        // Fences come from the LEF/DEF reader; Bookshelf carries none, so a null
        // pointer there means a correctly unconstrained design. Ignoring fences
        // scatters fenced cells across the die and draws no regions.
        const constraintMgr *regions = input ? input->constraints() : nullptr;
        // KTPLACE_SIMPL_FENCES=0 shows the placement without them, which is the
        // only way to see what the constraint costs. Off means neither enforced
        // nor drawn, so the frames do not show regions that are not honoured.
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
        // Every CG iteration of every round is offered to the animation, so the GIF
        // shows the solve converging and not just the outer loop. The animator
        // subsamples to fit its budget, so this costs thinning, not truncation.
        params.cgEvery = 1;
        if (const char *e = std::getenv("KTPLACE_SIMPL_CG_EVERY")) {
            params.cgEvery = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY_MAPS")) {
            params.densityMaps = std::atoll(e) != 0;
        }
        const SimplResult res = placer.place(params, plotDir, snapshotDir, regions);
        reportSimpl(res);
        hpwlGlobalBound_ = res.hpwlFinal;

    } else if (algorithm == "ntuplace1") {
        ktlog.echo("Running NTUPlace1 global placement (ratio partitioning)...");
        RatioPlacer placer(*db);
        const constraintMgr *regions = input ? input->constraints() : nullptr;
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
        reportNtuPlace1(res);
        hpwlGlobalBound_ = res.hpwlFinal;

    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

void FlowMgr::Impl::writePlacement(const std::string &outputPath) {
    if (!placed) {
        ktlog.fatal("No placement result available");
    }

    std::ofstream out(outputPath);
    if (!out.is_open()) {
        ktlog.fatal("Cannot open output file: {}", outputPath);
    }
    // Database coordinates reach ~1.5e6, so the default 6 significant digits
    // would round positions by several units and can move a cell across a
    // placement-region boundary. Keep enough digits to round-trip.
    out << std::setprecision(10);
    const Graph &g = db->getGraph();
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        // Bookshelf .pl: "<name> <x> <y> : <orientation>"
        out << vert.name << '\t' << vert.x << '\t' << vert.y
            << "\t: " << (vert.isFixed ? "N /FIXED" : "N") << '\n';
    }
}

void FlowMgr::Impl::clear() {
    // input before db: the reader owns the fences the placement is held to.
    input.reset();
    db.reset();
    loaded = false;
    placed = false;
}

}  // namespace ktplace
