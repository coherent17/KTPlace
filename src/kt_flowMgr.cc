// @file kt_flowMgr.cc
// Implementation of FlowMgr

#include "kt_flowMgr.h"

#include "kt_option.h"

#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "detailPlacer/fastdp/fastdp_dm.h"
#include "detailPlacer/fastdp/fastdp_flowMgr.h"
#include "detailPlacer/fastdp/fastdp_placer.h"
#include "legalizer/abacus/abacus_dm.h"
#include "legalizer/abacus/abacus_flowMgr.h"
#include "legalizer/abacus/abacus_legalizer.h"
#include "legalizer/multirow/multirow_dm.h"
#include "legalizer/multirow/multirow_flowMgr.h"
#include "legalizer/multirow/multirow_legalizer.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/simpl/simpl_dm.h"
#include "placer/simpl/simpl_flowMgr.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_trace.h"
#include "visualization/kt_visualizer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {
namespace {

/// A stage's frame hook, handing its snapshots to the run's sink.
///
/// The flow is the one place a stage's snapshots meet the sink: a stage says
/// when a snapshot matters and hands over the positions, and never learns where
/// they go -- drawn now, or recorded to be drawn later. Every stage's FrameInfo carries the same fields, so one
/// adapter serves them all. The netlist and fences belong to the flow's
/// database, which outlives every stage.
template <class Info>
auto drawFrames(FrameSink &viz, const ktDM &db, const std::array<double, 4> &die) {
    const Graph *netlist = &db.getGraph();
    const constraintMgr *fences = &db.constraints();
    return [&viz, netlist, fences, die](const std::vector<float> &xs, const std::vector<float> &ys,
                                        const Info &info) {
        viz.frame(*netlist, xs, ys, die, fences,
                  VisualFrame{info.series, info.step, info.numSteps, info.note, info.hpwl,
                              info.hpwlInitial, info.overflow, info.mandatory});
    };
}

/// SimPL's own snapshots: the bin-density field and the bounds curve, which it
/// hands over as numbers.
void drawSimplDiagnostics(FrameSink &viz, simpl::simplDM &dm) {
    dm.setDensityHook([&viz](const simpl::DensityInfo &d) {
        viz.densityMap(DensityField{d.series, d.step, d.note, d.overflow, d.die, d.x0, d.y0, d.dx,
                                    d.dy, d.nbx, d.nby, d.occ, d.avail, d.blockages});
    });
    dm.setBoundsHook([&viz](const std::vector<simpl::BoundsPoint> &curve) {
        std::vector<BoundsSample> samples;
        samples.reserve(curve.size());
        for (const simpl::BoundsPoint &p : curve) {
            samples.push_back({p.upper, p.lower});
        }
        viz.boundsCurve("simpl_bounds", "SimPL: HPWL of the lower and upper bounds", samples);
    });
}

}  // namespace

std::string traceFile(const std::string &plotDir) {
    return (std::filesystem::path(plotDir) / "trace.ktrace").string();
}

class FlowMgr::Impl {
public:
    void run(const kt_option &options);

private:
    std::unique_ptr<ktDM> db;
    PlotOptions plot;
    /// Where snapshots go: the visualizer, or `trace` when the run records them.
    FrameSink *sink = &Visualizer::instance();
    std::unique_ptr<TraceWriter> trace;

    void loadDesign(const std::string &dirPath);
    void reportDesign();
    void placeDesign(const kt_option &options);
    void writeDesign(const std::string &outputPath);
    void runPlacement(const std::string &algorithm);
    void legalizeDesign();
    [[nodiscard]] std::size_t multiRowCells() const;
    void detailPlaceDesign();
    void checkDesign(const char *stage);
    void renderFinalImage();
    /// The committed placement as float arrays, the shape every draw call takes.
    [[nodiscard]] std::pair<std::vector<float>, std::vector<float>> positions() const;
    void finishAnimation();
};


FlowMgr::FlowMgr() : pImpl(std::make_unique<Impl>()) {}
FlowMgr::~FlowMgr() = default;
FlowMgr::FlowMgr(FlowMgr &&) noexcept = default;
FlowMgr &FlowMgr::operator=(FlowMgr &&) noexcept = default;

void FlowMgr::run(const kt_option &options) {
    pImpl->run(options);
}


void FlowMgr::Impl::run(const kt_option &options) {
    plot = options.plot ? PlotOptions::fromOptions() : PlotOptions{};
    plot.dir = options.getPlotDir();
    // Recording replaces drawing: the run writes one trace and no pictures, and
    // ktplace-viz draws them from it afterwards.
    if (options.record && plot.enabled()) {
        std::error_code ec;
        std::filesystem::create_directories(plot.dir, ec);
        trace = std::make_unique<TraceWriter>(traceFile(plot.dir), plot);
        sink = trace.get();
    }

    loadDesign(options.inputPath);
    reportDesign();
    placeDesign(options);
    legalizeDesign();
    checkDesign("after legalization");
    detailPlaceDesign();
    checkDesign("after detail placement");
    renderFinalImage();
    finishAnimation();
    writeDesign(options.getOutputPath());
    TimerRegistry::instance().report();
}

void FlowMgr::Impl::loadDesign(const std::string &dirPath) {
    ScopedTimer timer("load");

    // A missing directory and an unrecognised one are different mistakes, and say
    // different things to whoever has to fix it.
    std::error_code ec;
    if (!std::filesystem::is_directory(dirPath, ec)) {
        ktlog.fatal("cannot read input directory: {}", dirPath);
    }

    std::unique_ptr<InputReader> reader = makeInputReader(dirPath);
    if (!reader) {
        ktlog.fatal("no input format recognises {}", dirPath);
    }
    ktlog.echo("Reading {} input from {}", reader->formatName(), dirPath);

    db = reader->read(dirPath);
    if (!db) {
        ktlog.fatal("Failed to read {} design from {}", reader->formatName(), dirPath);
    }
}

void FlowMgr::Impl::reportDesign() {
    db->report();
    db->reportUtilisation();
}

void FlowMgr::Impl::placeDesign(const kt_option &options) {
    ScopedTimer timer("place");
    runPlacement(options.algorithm);
}

void FlowMgr::Impl::runPlacement(const std::string &algorithm) {
    if (!db) {
        ktlog.fatal("No placement database loaded");
    }

    // Configured here rather than inside a stage, so the legalizer and the
    // detailed placer add to the same GIF. A recorded run leaves the visualizer
    // alone: it is configured when the trace is drawn.
    if (!trace) {
        Visualizer::instance().configure(plot.dir, plot);
    }

    // Global placement is by far the most frame-hungry stage, so it does not get
    // to spend the whole budget: two fifths are held back for legalization and
    // detailed placement, the stages that turn a legal-looking placement into a
    // real one.
    sink->holdBack(plot.frameBudget * 2 / 5);

    if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        // Build SimPL's own database from ours, run it there, commit the answer.
        // SimPL has no way to reach ktDM, so this is the one write, and SimPL
        // assembles its own parameters from the options it was given.
        simpl::simplDM simplDB = simpl::buildSimplDM(*db);

        // Fences come from the LEF/DEF reader; Bookshelf carries none, so an empty
        // set is a correctly unconstrained design. Ignoring fences scatters fenced
        // cells across the die and draws no regions. `add_option simpl fences 0`
        // shows the placement without them, which is the only way to see what the
        // constraint costs.
        if (!simplDB.useFences()) {
            ktlog.echo("SimPL: fence enforcement disabled by 'add_option simpl fences 0'");
        }

        // How often SimPL offers a snapshot is SimPL's own option. One default is
        // the flow's: the warm-up CG iterates are only worth their cost as frames
        // of the GIF, so they are off unless the run animates. Setting
        // `simpl cg_every_init` overrides this, because SimPL applies its options
        // over the parameters it is given.
        simpl::SimplParams params = simplDB.params();
        if (!sink->animating()) {
            params.cgEveryInit = 0;
        }
        if (sink->enabled()) {
            sink->beginStage({"simpl", "simpl", 0});
            simplDB.setFrameHook(drawFrames<simpl::FrameInfo>(*sink, *db, simplDB.design().die));
            drawSimplDiagnostics(*sink, simplDB);
        }
        simpl::flowMgr flow(simplDB);
        db->setGPSolution(flow.runGlobalPlacer(params, simplDB.useFences()));
        return;

    } else if (algorithm == "ntuplace1") {
        ktlog.echo("Running NTUPlace1 global placement (ratio partitioning)...");
        RatioPlacer placer(*db);
        // No frame hook. The partitioner offers a snapshot per cut, but no stage
        // was ever begun for it, so the visualizer dropped every one of them;
        // installing a hook here would start drawing ~one frame per cut, which
        // is a change to what a run draws and belongs in its own commit.
        const RatioPlaceParams params = placer.params();
        const RatioPlaceResult res = placer.place(params);
        reportNtuPlace1(res);
        return;

    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

void FlowMgr::Impl::legalizeDesign() {
    // Whatever the placer reserved is released, so the later stages get the whole
    // remaining budget.
    sink->holdBack(0);

    // Abacus places into a single row, so a cell taller than one has nowhere to
    // go and is left overlapping. When the design has such cells, the row-slicing
    // legalizer runs instead: it cuts cells out of every row they span, so a tall
    // cell occupies a real well and the cells around it place normally.
    if (multiRowCells() > 0) {
        ktlog.echo("Running multi-row legalization ({} cell(s) taller than a row)...",
                   multiRowCells());
        // Build this pass's own database, run it there, commit the answer. The
        // pass assembles its own parameters, cadence included, from its options.
        multirow::multirowDM multiDB = multirow::buildMultiRowDM(*db);
        const MultiRowLegalizeParams params = multiDB.params();
        if (sink->enabled()) {
            sink->beginStage({"legalize_multi", "", 0});
            multiDB.setFrameHook(drawFrames<multirow::FrameInfo>(*sink, *db, multiDB.design().die));
        }
        multirow::flowMgr flow(multiDB);
        db->setMultiRowSolution(flow.runLegalizer(params));
        return;
    }

    ktlog.echo("Running Abacus legalization...");
    // Build Abacus's own database, run it there, commit the answer.
    abacus::abacusDM abacusDB = abacus::buildAbacusDM(*db);
    if (sink->enabled()) {
        sink->beginStage({"legalize", "", 0});
        abacusDB.setFrameHook(drawFrames<abacus::FrameInfo>(*sink, *db, abacusDB.design().die));
    }
    const LegalizeParams lparams = abacusDB.params();

    abacus::flowMgr flow(abacusDB);
    db->setLegalizationSolution(flow.runLegalizer(lparams));
}

std::size_t FlowMgr::Impl::multiRowCells() const {
    if (!db) {
        return 0;
    }
    // The shortest row sets the bar: a cell taller than every row has no single
    // row to go in, and that is the case the row slicer exists for.
    double minHeight = std::numeric_limits<double>::max();
    for (const RowInfo &ri : db->getRows()) {
        if (ri.height > 0.0 && !ri.subrows.empty()) {
            minHeight = std::min(minHeight, ri.height);
        }
    }
    if (minHeight == std::numeric_limits<double>::max()) {
        return 0;
    }
    const double tall = minHeight * 1.5;
    const Graph &g = db->getGraph();
    std::size_t n = 0;
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &vert = g.getCell(v);
        if (!vert.isFixed && !vert.isTerminal && vert.height > tall) {
            ++n;
        }
    }
    return n;
}

void FlowMgr::Impl::detailPlaceDesign() {
    // Abacus minimises displacement, not wirelength, so legalization usually
    // costs a little HPWL; detailed placement wins it back.
    ktlog.echo("Running FastDP detailed placement...");
    // Build FastDP's own database, run it there, commit the answer.
    fastdp::fastdpDM dpDB = fastdp::buildFastdpDM(*db);
    if (sink->enabled()) {
        sink->beginStage({"detailplace", "", 0});
        dpDB.setFrameHook(drawFrames<fastdp::FrameInfo>(*sink, *db, dpDB.design().die));
    }
    const DetailPlaceParams dparams = dpDB.params();
    fastdp::flowMgr flow(dpDB);
    db->setDetailedPlaceSolution(flow.runDetailedPlacer(dparams));
}

void FlowMgr::Impl::checkDesign(const char *stage) {
    // Separate from each stage's self-check, so a disagreement between what a
    // stage claims and what the file actually contains stays visible.
    {
        // Timed on its own: it reads the whole placement, changes nothing, and is
        // the phase that goes quadratic if the spatial index degrades.
        const ScopedTimer checkTimer("place-check");
        const std::vector<ktDM::Defect> defects = db->verify();
        ktReportTable check(fmt::format("Placement check (independent, {})", stage));
        check.setHeaders({"check", "result"});
        if (defects.empty()) {
            check.addRow({"overlapping cell pairs", "0"});
            check.addRow({"cells outside the die", "0"});
            check.addRow({"cells outside their fence", db->hasFences() ? "0" : "n/a"});
            check.addRow({"verdict", "PASS"});
        } else {
            for (const ktDM::Defect &d : defects) {
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

std::pair<std::vector<float>, std::vector<float>> FlowMgr::Impl::positions() const {
    const Graph &g = db->getGraph();
    std::vector<float> x(g.getNumCells());
    std::vector<float> y(g.getNumCells());
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        x[v] = static_cast<float>(g.getCell(v).x);
        y[v] = static_cast<float>(g.getCell(v).y);
    }
    return {x, y};
}

void FlowMgr::Impl::renderFinalImage() {
    sink->finalImage(db->getGraph(), &db->constraints(), db->hpwl());
}

void FlowMgr::Impl::finishAnimation() {
    if (trace) {
        (void)trace->finish(db->getGraph(), positions().first, positions().second,
                            db->placementDieBox(), &db->constraints(), db->hpwl());
        trace->close();
        ktlog.echo("recorded {} snapshot(s) to {}; draw them with: ktplace-viz {}",
                   trace->frameCount(), trace->path(), trace->path());
        return;
    }
    Visualizer &viz = Visualizer::instance();
    const bool written = viz.finish(db->getGraph(), positions().first, positions().second,
                                    db->placementDieBox(), &db->constraints(), db->hpwl());
    if (written) {
        ktlog.echo(
            "animation: {} frames -> {}/anim/placement.gif (global placement, then "
            "legalization, then detailed placement)",
            Visualizer::instance().frameCount(), viz.directory());
    } else {
        ktlog.echo(
            "animation: {} frame(s) recorded, no GIF written (one animation needs at "
            "least two frames)",
            Visualizer::instance().frameCount());
    }
}

void FlowMgr::Impl::writeDesign(const std::string &outputPath) {
    ScopedTimer timer("write");
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        ktlog.fatal("Cannot open output file: {}", outputPath);
    }
    out << std::setprecision(10);
    const Graph &g = db->getGraph();
    const std::size_t nv = g.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        // Bookshelf .pl: "<name> <x> <y> : <orientation>"
        out << vert.name << '\t' << vert.x << '\t' << vert.y
            << "\t: " << (vert.isFixed ? "N /FIXED" : "N") << '\n';
    }
}

}  // namespace ktplace
