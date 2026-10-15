// @file kt_flowMgr.cc
// Implementation of FlowMgr

#include "kt_flowMgr.h"

#include "kt_option.h"

#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "detailPlacer/fastdp/fastdp_dm.h"
#include "detailPlacer/fastdp/fastdp_flowMgr.h"
#include "legalizer/abacus/abacus_dm.h"
#include "legalizer/abacus/abacus_flowMgr.h"
#include "legalizer/multirow/multirow_dm.h"
#include "legalizer/multirow/multirow_flowMgr.h"
#include "placer/ntuplace1/ntuplace1_dm.h"
#include "placer/ntuplace1/ntuplace1_flowMgr.h"
#include "placer/simpl/simpl_dm.h"
#include "placer/simpl/simpl_flowMgr.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_snapshots.h"
#include "visualization/kt_visualizer.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ktplace {

namespace {

std::string snapshotFile(const std::string &plotDir) {
    return (std::filesystem::path(plotDir) / "snapshots.ktsnap").string();
}

bool recordStage(auto &dm, SnapshotWriter *snapshots, const StageVisual &stage) {
    if (snapshots == nullptr) {
        return false;
    }
    snapshots->beginStage(stage);
    dm.setFrameHook([snapshots, die = dm.design().die](const std::vector<float> &xs,
                                                       const std::vector<float> &ys,
                                                       const auto &info) {
        snapshots->frame(xs, ys, die, true,
                         VisualFrame{info.series, info.step, info.numSteps, info.note, info.hpwl,
                                     info.hpwlInitial, info.overflow, info.mandatory});
    });
    return true;
}

void recordSimplBounds(simpl::simplDM &dm, SnapshotWriter &snapshots) {
    dm.setBoundsHook([&snapshots](std::span<const simpl::BoundsPoint> curve) {
        std::vector<BoundsSample> samples;
        samples.reserve(curve.size());
        for (const simpl::BoundsPoint &point : curve) {
            samples.push_back({point.upper, point.lower});
        }
        snapshots.boundsCurve("simpl_bounds", "SimPL: HPWL of the lower and upper bounds", samples);
    });
}

}  // namespace

class FlowMgr::Impl {
public:
    void run(const kt_option &options);

private:
    std::unique_ptr<ktDM> db;
    PlotOptions plot;
    std::unique_ptr<SnapshotWriter> snapshots;

    void parseTcl(const kt_option &options);
    void loadDesign(const std::string &dirPath);
    void reportDesign();
    void placeDesign();
    void writeDesign(const std::string &outputPath);
    void legalizeDesign();
    void detailPlaceDesign();
    void drawPictures();
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
    parseTcl(options);
    plot = options.plot ? PlotOptions::fromOptions() : PlotOptions{};
    plot.dir = options.getPlotDir();

    loadDesign(options.inputPath);
    reportDesign();
    placeDesign();
    legalizeDesign();
    detailPlaceDesign();
    writeDesign(options.getOutputPath());
    drawPictures();
    TimerRegistry::instance().report();
}

void FlowMgr::Impl::parseTcl(const kt_option &options) {
    if (options.tclFile.empty()) {
        return;
    }
    try {
        tcl::TclMgr::instance().loadScript(options.tclFile);
    } catch (const tcl::Error &error) {
        ktlog.fatal("{}: {}", options.tclFile, error.what());
    }
}

void FlowMgr::Impl::loadDesign(const std::string &dirPath) {
    ScopedTimer timer("load");

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

void FlowMgr::Impl::placeDesign() {
    ScopedTimer timer("place");

    // Which placer runs is the flow's business: it is the one that knows what
    // placers exist. The script chooses with `add_option flow algorithm ...`, and
    // SimPL runs when nothing says otherwise.
    const std::string algorithm = OptionDb::instance()
                                      .getComponentOption(Component::Flow)
                                      .get<std::string>("algorithm")
                                      .value_or("simpl");
    if (algorithm != "simpl" && algorithm != "ntuplace1") {
        ktlog.fatal("unknown placer '{}'; expected simpl or ntuplace1", algorithm);
    }
    if (!db) {
        ktlog.fatal("No placement database loaded");
    }

    // Configured here rather than inside a stage, so the legalizer and the
    // detailed placer add to the same GIF.
    Visualizer::instance().configure(plot.dir, plot);
    if (plot.enabled()) {
        snapshots = std::make_unique<SnapshotWriter>(snapshotFile(plot.dir));
        // Global placement is by far the most frame-hungry stage, so it does not
        // get to spend the whole budget: two fifths are held back for legalization
        // and detailed placement, the stages that turn a legal-looking placement
        // into a real one.
        snapshots->holdBack(plot.frameBudget * 2 / 5);
    }

    if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        simpl::simplDM simplDB = simpl::buildSimplDM(*db);
        if (recordStage(simplDB, snapshots.get(), {"simpl", "simpl", 0})) {
            recordSimplBounds(simplDB, *snapshots);
        }
        if (!simplDB.options().fences) {
            ktlog.echo("SimPL: fence enforcement disabled by 'add_option simpl fences 0'");
        }
        simpl::flowMgr flow(simplDB);
        db->setGPSolution(flow.runGlobalPlacer());
        return;

    } else if (algorithm == "ntuplace1") {
        // Build this placer's own database, run it there, commit the answer: the
        // same shape as every other stage.
        ntuplace1::ntuplace1DM placeDB = ntuplace1::buildNtuplace1DM(*db);
        ntuplace1::flowMgr flow(placeDB);
        db->setGPSolution(flow.runGlobalPlacer());
        return;

    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

void FlowMgr::Impl::legalizeDesign() {
    // Whatever the placer reserved is released, so the later stages get the whole
    // remaining budget.
    if (snapshots) {
        snapshots->holdBack(0);
    }

    if (const std::size_t tall = db->multiRowCells(); tall > 0) {
        ktlog.echo("Running multi-row legalization ({} cell(s) taller than a row)...", tall);
        multirow::multirowDM multiDB = multirow::buildMultiRowDM(*db);
        recordStage(multiDB, snapshots.get(), {"legalize_multi", "", 0});
        multirow::flowMgr flow(multiDB);
        db->setMultiRowSolution(flow.runLegalizer());
        return;
    }

    ktlog.echo("Running Abacus legalization...");
    abacus::abacusDM abacusDB = abacus::buildAbacusDM(*db);
    recordStage(abacusDB, snapshots.get(), {"legalize", "", 0});
    abacus::flowMgr flow(abacusDB);
    db->setLegalizationSolution(flow.runLegalizer());
}


void FlowMgr::Impl::detailPlaceDesign() {
    ktlog.echo("Running FastDP detailed placement...");
    fastdp::fastdpDM dpDB = fastdp::buildFastdpDM(*db);
    recordStage(dpDB, snapshots.get(), {"detailplace", "", 0});
    fastdp::flowMgr flow(dpDB);
    db->setDetailedPlaceSolution(flow.runDetailedPlacer());
}


void FlowMgr::Impl::drawPictures() {
    if (!snapshots) {
        return;
    }
    ScopedTimer timer("draw");
    snapshots->close();
    ktlog.echo("Drawing {} recorded snapshot(s)...", snapshots->frameCount());
    (void)replaySnapshots(snapshots->path(), Visualizer::instance(), db->getGraph(),
                          &db->constraints());
    Visualizer::instance().finalImage(db->getGraph(), &db->constraints(), db->hpwl());
    finishAnimation();
    if (plot.keepSnapshots) {
        ktlog.echo("Kept the recorded snapshots: {}", snapshots->path());
        return;
    }
    std::error_code ec;
    std::filesystem::remove(snapshots->path(), ec);
}

void FlowMgr::Impl::finishAnimation() {
    const Graph &graph = db->getGraph();
    const auto coordinates = [&graph](double Vertex::*axis) {
        std::vector<float> out(graph.getNumCells());
        for (std::size_t v = 0; v < out.size(); ++v) {
            out[v] = static_cast<float>(graph.getCell(v).*axis);
        }
        return out;
    };
    Visualizer &viz = Visualizer::instance();
    const bool written = viz.finish(graph, coordinates(&Vertex::x), coordinates(&Vertex::y),
                                    db->placementDieBox(), &db->constraints(), db->hpwl());
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
