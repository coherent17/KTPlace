// @file kt_session.cc
// Implementation of ktSession

#include "session/kt_session.h"

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
#include "placer/simpl/simpl_placer.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_visualizer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

ktSession::ktSession() = default;
ktSession::~ktSession() = default;
ktSession::ktSession(ktSession &&) noexcept = default;
ktSession &ktSession::operator=(ktSession &&) noexcept = default;

// ---------------------------------------------------------------- pictures

void ktSession::setPlotOptions(const PlotOptions &options) {
    plot = options;
    vizConfigured = false;
}

const PlotOptions &ktSession::plotOptions() const noexcept {
    return plot;
}

void ktSession::ensureVisualizer() {
    if (vizConfigured) {
        return;
    }
    // Configured once per run rather than inside a stage, so the legalizer and
    // the detailed placer add to the same GIF. The Visualizer is process-wide,
    // so this is also what points it away from an earlier session's directory.
    Visualizer::instance().configure(plot.dir, plot);
    vizConfigured = true;
}

// ---------------------------------------------------------------- the design

ktDM &ktSession::requireDesign(const char *stage) const {
    if (!db) {
        ktlog.fatal("{}: no design loaded; read a design first", stage);
    }
    return *db;
}

void ktSession::readDesign(const std::string &dirPath) {
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
    // A new design is a new run, and gets an animation of its own.
    vizConfigured = false;
}

bool ktSession::hasDesign() const noexcept {
    return db != nullptr;
}

void ktSession::reportDesign() const {
    const ktDM &d = requireDesign("report_design");
    d.report();
    d.reportUtilisation();
}

// ---------------------------------------------------------------- the stages

void ktSession::globalPlace(const std::string &algorithm) {
    ktDM &d = requireDesign("global_place");
    ScopedTimer timer("place");

    ensureVisualizer();

    // Global placement is by far the most frame-hungry stage, so it does not get
    // to spend the whole budget: two fifths are held back for legalization and
    // detailed placement, the stages that turn a legal-looking placement into a
    // real one.
    Visualizer::instance().holdBack(Visualizer::instance().options().frameBudget * 2 / 5);

    if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        // Fences come from the LEF/DEF reader; Bookshelf carries none, so an empty
        // set is a correctly unconstrained design. Ignoring fences scatters fenced
        // cells across the die and draws no regions.
        //
        // KTPLACE_SIMPL_FENCES=0 shows the placement without them, which is the only
        // way to see what the constraint costs.
        const char *fenceEnv = std::getenv("KTPLACE_SIMPL_FENCES");
        const bool fencesOn = (fenceEnv == nullptr) || (std::atoi(fenceEnv) != 0);
        if (!fencesOn) {
            ktlog.echo("SimPL: fence enforcement DISABLED by KTPLACE_SIMPL_FENCES=0");
        }

        // Cadence and diagnostics come from the one place visualisation settings
        // are declared, so there is no second list to keep in step.
        simpl::SimplParams params;
        params.traceEvery = plot.traceEvery;
        params.cgEvery = plot.cgFrameEvery;
        params.densityMaps = plot.densityMaps;
        // Build SimPL's own database from ours, run it there, commit the answer.
        // SimPL has no way to reach ktDM, so this is the one write.
        simpl::simplDM simplDB = simpl::buildSimplDM(d, plot.dir);
        simpl::flowMgr flow(simplDB);
        d.setGPSolution(flow.runGlobalPlacer(params, fencesOn));
        return;

    } else if (algorithm == "ntuplace1") {
        ktlog.echo("Running NTUPlace1 global placement (ratio partitioning)...");
        RatioPlacer placer(d);
        RatioPlaceParams params;
        if (const char *e = std::getenv("KTPLACE_NTU_LEAF_CELLS")) {
            params.targetLeafCells = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_MAX_LEVELS")) {
            params.maxLevels = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_RETRIES")) {
            params.maxRatioRetries = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_VERBOSE")) {
            params.verbose = std::atoll(e) != 0;
        }
        const RatioPlaceResult res = placer.place(params);
        reportNtuPlace1(res);
        return;

    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

void ktSession::legalize() {
    ktDM &d = requireDesign("legalize");
    ensureVisualizer();

    // Whatever the placer reserved is released, so the later stages get the whole
    // remaining budget.
    Visualizer::instance().holdBack(0);

    // Abacus places into a single row, so a cell taller than one has nowhere to
    // go and is left overlapping. When the design has such cells, the row-slicing
    // legalizer runs instead: it cuts cells out of every row they span, so a tall
    // cell occupies a real well and the cells around it place normally.
    if (multiRowCells() > 0) {
        ktlog.echo("Running multi-row legalization ({} cell(s) taller than a row)...",
                   multiRowCells());
        // The pass's own cadence; the sink handles the rest.
        MultiRowLegalizeParams params;
        params.frameEvery = plot.legalFrameEvery;
        // Build this pass's own database, run it there, commit the answer.
        multirow::multirowDM multiDB = multirow::buildMultiRowDM(d);
        multirow::flowMgr flow(multiDB);
        d.setMultiRowSolution(flow.runLegalizer(params));
        return;
    }

    ktlog.echo("Running Abacus legalization...");
    LegalizeParams lparams;
    if (const char *e = std::getenv("KTPLACE_ABACUS_MAX_ROW_DIST")) {
        lparams.maxRowDistance = static_cast<std::size_t>(std::atoll(e));
    }
    lparams.frameEvery = plot.legalFrameEvery;
    // Build Abacus's own database, run it there, commit the answer.
    abacus::abacusDM abacusDB = abacus::buildAbacusDM(d);
    abacus::flowMgr flow(abacusDB);
    d.setLegalizationSolution(flow.runLegalizer(lparams));
}

std::size_t ktSession::multiRowCells() const {
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

void ktSession::detailPlace() {
    ktDM &d = requireDesign("detail_place");
    ensureVisualizer();

    // Abacus minimises displacement, not wirelength, so legalization usually
    // costs a little HPWL; detailed placement wins it back.
    ktlog.echo("Running FastDP detailed placement...");
    DetailPlaceParams dparams;
    if (const char *e = std::getenv("KTPLACE_DP_WINDOW")) {
        dparams.localReorderWindow = static_cast<std::size_t>(std::atoll(e));
    }
    // Build FastDP's own database, run it there, commit the answer.
    fastdp::fastdpDM dpDB = fastdp::buildFastdpDM(d);
    fastdp::flowMgr flow(dpDB);
    d.setDetailedPlaceSolution(flow.runDetailedPlacer(dparams));
}

void ktSession::renderFinalImage() {
    const ktDM &d = requireDesign("render_final_image");
    ensureVisualizer();
    // A separate high-resolution still, because GIF frames have to stay small and
    // a big design's cells collapse to a pixel each at that size. The zoom is on
    // both axes: 8 turns a 768x768 frame into 6144x6144, about fourteen pixels
    // per standard cell. Written once, and it compresses to a couple of MB.
    Visualizer::instance().finalImage(d.getGraph(), &d.constraints(), d.hpwl());
}

bool ktSession::finishAnimation() {
    const ktDM &d = requireDesign("finish_animation");
    ensureVisualizer();
    // Every stage has now contributed, so the run can be told as one animation.
    // Assembling it here, and not at the end of global placement, is the whole
    // point: the legalizer's pull back onto the rows is usually the most
    // consequential motion of the run, and it happens after the placer is done.
    Visualizer &viz = Visualizer::instance();
    const auto [x, y] = positions();
    const bool written =
        viz.finish(d.getGraph(), x, y, d.placementDieBox(), &d.constraints(), d.hpwl());
    // Timed inside finish(): encoding the GIF is pure output cost, and can be
    // longer than the detailed placement on a small design.
    if (written) {
        ktlog.echo(
            "animation: {} frames -> {}/anim/placement.gif (global placement, then "
            "legalization, then detailed placement)",
            viz.frameCount(), viz.directory());
    } else {
        ktlog.echo(
            "animation: {} frame(s) recorded, no GIF written (one animation needs at "
            "least two frames)",
            viz.frameCount());
    }
    return written;
}

void ktSession::writeDesign(const std::string &outputPath) const {
    const ktDM &d = requireDesign("write_design");
    ScopedTimer timer("write");
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        ktlog.fatal("Cannot open output file: {}", outputPath);
    }
    // Database coordinates reach ~1.5e6, so the default 6 significant digits
    // would round positions by several units and can move a cell across a
    // placement-region boundary. Keep enough digits to round-trip.
    out << std::setprecision(10);
    const Graph &g = d.getGraph();
    const std::size_t nv = g.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        // Bookshelf .pl: "<name> <x> <y> : <orientation>"
        out << vert.name << '\t' << vert.x << '\t' << vert.y
            << "\t: " << (vert.isFixed ? "N /FIXED" : "N") << '\n';
    }
}

// ---------------------------------------------------------------- queries

double ktSession::hpwl() const {
    return requireDesign("report_hpwl").hpwl();
}

std::vector<ktDM::Defect> ktSession::checkPlacement() const {
    return requireDesign("check_placement").verify();
}

std::vector<ktDM::Defect> ktSession::reportPlacementCheck(const std::string &stage) const {
    const ktDM &d = requireDesign("check_placement");
    // Separate from each stage's self-check, so a disagreement between what a
    // stage claims and what the file actually contains stays visible.
    //
    // Timed on its own: it reads the whole placement, changes nothing, and is
    // the phase that goes quadratic if the spatial index degrades.
    const ScopedTimer checkTimer("place-check");
    std::vector<ktDM::Defect> defects = d.verify();
    ktReportTable check(fmt::format("Placement check (independent, {})", stage));
    check.setHeaders({"check", "result"});
    if (defects.empty()) {
        check.addRow({"overlapping cell pairs", "0"});
        check.addRow({"cells outside the die", "0"});
        check.addRow({"cells outside their fence", d.hasFences() ? "0" : "n/a"});
        check.addRow({"verdict", "PASS"});
    } else {
        for (const ktDM::Defect &defect : defects) {
            check.addRow({defect.what, fmt::format("{}", defect.count)});
        }
        check.addRow({"verdict", "FAIL"});
    }
    check.emit();
    if (!defects.empty()) {
        ktlog.warning(
            "the placement that will be written is not legal; see the placement check "
            "above");
    }
    return defects;
}

// ---------------------------------------------------------------- views

const ktDM &ktSession::design() const {
    return requireDesign("design");
}

const DieInfo &ktSession::die() const {
    return requireDesign("die").die();
}

const Graph &ktSession::graph() const {
    return requireDesign("graph").getGraph();
}

std::span<const Vertex> ktSession::cells() const {
    return requireDesign("cells").getGraph().getCells();
}

std::array<double, 4> ktSession::placementDieBox() const {
    return requireDesign("placement_die_box").placementDieBox();
}

std::pair<std::vector<float>, std::vector<float>> ktSession::positions() const {
    const Graph &g = requireDesign("positions").getGraph();
    std::vector<float> x(g.getNumCells());
    std::vector<float> y(g.getNumCells());
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        x[v] = static_cast<float>(g.getCell(v).x);
        y[v] = static_cast<float>(g.getCell(v).y);
    }
    return {x, y};
}

}  // namespace ktplace
