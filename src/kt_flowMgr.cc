// @file kt_flowMgr.cc
// Implementation of FlowMgr

#include "kt_flowMgr.h"

#include "kt_option.h"

#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "detailPlacer/fastdp/fastdp_dm.h"
#include "detailPlacer/fastdp/fastdp_flowMgr.h"
#include "globalPlacer/ntuplace1/ntuplace1_dm.h"
#include "globalPlacer/ntuplace1/ntuplace1_flowMgr.h"
#include "globalPlacer/simpl/simpl_dm.h"
#include "globalPlacer/simpl/simpl_flowMgr.h"
#include "legalizer/abacus/abacus_dm.h"
#include "legalizer/abacus/abacus_flowMgr.h"
#include "legalizer/multirow/multirow_dm.h"
#include "legalizer/multirow/multirow_flowMgr.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_snapshots.h"
#include "visualization/kt_visualizer.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace {

namespace {

std::string snapshotFile(const std::string &plotDir) {
    return (std::filesystem::path(plotDir) / "snapshots.ktsnap").string();
}

bool recordStage(auto &dm, SnapshotWriter *snapshots, const FlowObserver &observer,
                 const StageVisual &stage) {
    if (snapshots == nullptr && !observer.frame) {
        return false;
    }
    if (snapshots != nullptr) {
        snapshots->beginStage(stage);
    }
    dm.setFrameHook(
        [snapshots, frame = observer.frame, name = stage.stage, die = dm.design().die](
            const std::vector<float> &xs, const std::vector<float> &ys, const auto &info) {
            if (snapshots != nullptr) {
                snapshots->frame(
                    xs, ys, die, true,
                    VisualFrame{info.series, info.step, info.numSteps, info.note, info.hpwl,
                                info.hpwlInitial, info.overflow, info.mandatory});
            }
            if (frame) {
                frame(name, xs, ys, info.hpwl);
            }
        });
    return snapshots != nullptr;
}

void recordSimplBounds(simpl::simplDM &dm, SnapshotWriter &snapshots) {
    dm.setBoundsHook([&snapshots](std::span<const simpl::BoundsPoint> curve) {
        std::vector<BoundsSample> samples;
        samples.reserve(curve.size());
        for (const simpl::BoundsPoint &point : curve) {
            samples.push_back({point.upper, point.lower});
        }
        snapshots.boundsCurve("simpl_convergence",
                              "SimPL convergence: HPWL of the lower and upper bounds", samples);
    });
}

}  // namespace

class FlowMgr::Impl {
public:
    void run(const kt_option &options);

    FlowObserver observer;

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
    void commit(std::string_view stage);
    void reportSummary() const;

    struct Committed {
        std::string stage;
        std::string step;
        double hpwl = 0.0;
        std::uint32_t checksum = 0;
    };
    std::vector<Committed> committed;
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

void FlowMgr::observe(FlowObserver observer) {
    pImpl->observer = std::move(observer);
}


void FlowMgr::Impl::run(const kt_option &options) {
    parseTcl(options);
    plot = PlotOptions::fromOptions(options.getPlotDir());

    loadDesign(options.inputPath);
    placeDesign();
    legalizeDesign();
    detailPlaceDesign();
    writeDesign(options.getOutputPath());
    drawPictures();
    ktlog.beginStep("summary");
    TimerRegistry::instance().report();
    reportSummary();
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
    ktlog.beginStep("load");
    ScopedTimer timer("load", true);

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
    reportDesign();
    commit("load");
}

void FlowMgr::Impl::reportDesign() {
    db->report();
    db->reportStatistics();
    db->reportUtilisation();
}

void FlowMgr::Impl::commit(std::string_view stage) {
    committed.push_back({std::string(stage), ktlog.currentStep(), db->hpwl(), db->checksum()});
    ktlog.echo("committed to ktDM: HPWL {:.6e}, checksum {:08x}", committed.back().hpwl,
               committed.back().checksum);
    if (observer.committed) {
        observer.committed(stage, *db);
    }
}

void FlowMgr::Impl::reportSummary() const {
    ktReportTable t("Run summary");
    t.setHeaders({"stage", "HPWL", "change", "wall", "cpu", "ktDM"});
    for (std::size_t i = 0; i < committed.size(); ++i) {
        const Committed &c = committed[i];
        const TimerStats *time = TimerRegistry::instance().find(c.stage);
        const double before = i > 0 ? committed[i - 1].hpwl : 0.0;
        t.addRow({c.step == c.stage ? c.stage : std::format("{} ({})", c.stage, c.step),
                  std::format("{:.6e}", c.hpwl),
                  i > 0 && before > 0.0 ? std::format("{:+.2f}%", 100.0 * (c.hpwl / before - 1.0))
                                        : std::string(),
                  time != nullptr ? std::format("{:.3f}s", time->wallSeconds) : std::string(),
                  time != nullptr ? std::format("{:.3f}s", time->cpuSeconds) : std::string(),
                  std::format("{:08x}", c.checksum)});
    }
    t.emit();
}

void FlowMgr::Impl::placeDesign() {
    ScopedTimer timer("global placement", true);

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
        simpl::flowMgr flow(*db);
        if (recordStage(flow.dm(), snapshots.get(), observer, {"simpl", "simpl", 0})) {
            recordSimplBounds(flow.dm(), *snapshots);
        }
        if (!flow.dm().options().fences) {
            ktlog.echo("SimPL: fence enforcement disabled by 'add_option simpl fences 0'");
        }
        (void)flow.run();
        db->setSimplSolution(flow.solution());
        commit("global placement");
        return;

    } else if (algorithm == "ntuplace1") {
        // Build this placer's own database, run it there, commit the answer: the
        // same shape as every other stage.
        ntuplace1::flowMgr flow(*db);
        (void)flow.run();
        db->setNtuplace1Solution(flow.solution());
        commit("global placement");
        return;

    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

void FlowMgr::Impl::legalizeDesign() {
    ScopedTimer timer("legalization", true);
    // Whatever the placer reserved is released, so the later stages get the whole
    // remaining budget.
    if (snapshots) {
        snapshots->holdBack(0);
    }

    if (const std::size_t tall = db->multiRowCells(); tall > 0) {
        multirow::flowMgr flow(*db);
        ktlog.echo("multi-row legalization ({} cell(s) taller than a row)", tall);
        recordStage(flow.dm(), snapshots.get(), observer, {"legalize_multi", "", 0});
        (void)flow.run();
        db->setMultiRowSolution(flow.solution());
        commit("legalization");
        return;
    }

    abacus::flowMgr flow(*db);
    recordStage(flow.dm(), snapshots.get(), observer, {"legalize", "", 0});
    (void)flow.run();
    db->setAbacusSolution(flow.solution());
    commit("legalization");
}


void FlowMgr::Impl::detailPlaceDesign() {
    ScopedTimer timer("detailed placement", true);
    fastdp::flowMgr flow(*db);
    recordStage(flow.dm(), snapshots.get(), observer, {"detailplace", "", 0});
    (void)flow.run();
    db->setFastdpSolution(flow.solution());
    commit("detailed placement");
}


void FlowMgr::Impl::drawPictures() {
    if (!snapshots) {
        return;
    }
    ktlog.beginStep("draw");
    ScopedTimer timer("draw", true);
    snapshots->close();
    ktlog.echo("replaying {} recorded snapshot(s)", snapshots->frameCount());
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
            "animation: {} frames -> {}/placement.gif (global placement, then "
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
    ktlog.beginStep("write");
    ScopedTimer timer("write", true);
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
