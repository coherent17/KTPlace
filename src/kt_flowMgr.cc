#include "kt_flowMgr.h"

#include "kt_option.h"
#include "kt_tclCmd.h"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace {

namespace {

enum class GlobalPlacer { Simpl, Ntuplace1 };

std::string snapshotFile(const std::string &plotDir) {
    return (std::filesystem::path(plotDir) / "snapshots.ktsnap").string();
}

GlobalPlacer chosenGlobalPlacer() {
    const std::string algorithm = OptionDb::instance()
                                      .getComponentOption(Component::Flow)
                                      .get<std::string>("algorithm")
                                      .value_or("simpl");
    if (algorithm == "simpl") {
        return GlobalPlacer::Simpl;
    }
    if (algorithm == "ntuplace1") {
        return GlobalPlacer::Ntuplace1;
    }
    ktlog.fatal("unknown placer '{}'; expected simpl or ntuplace1", algorithm);
}

std::vector<float> cellCoordinates(const Graph &graph, double Vertex::*axis) {
    std::vector<float> out(graph.getNumCells());
    for (std::size_t v = 0; v < out.size(); ++v) {
        out[v] = static_cast<float>(graph.getCell(v).*axis);
    }
    return out;
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
    FlowObserver observer;
    kt_option options;
    std::unique_ptr<ktDM> db;

    void begin(const kt_option &sessionOptions);
    void run(const kt_option &runOptions);
    std::string evaluate(const std::string &script);
    void finish();

    void readDesign(const std::string &dirPath);
    void globalPlace();
    void legalize();
    void detailPlace();
    std::string writePlacement(const std::string &path);
    void writePlots();
    [[nodiscard]] const ktDM &design() const;
    [[nodiscard]] bool isFreshlyLoaded(const std::string &dirPath) const;
    void reportDesign() const;

private:
    struct Committed {
        std::string stage;
        std::string step;
        double hpwl = 0.0;
        std::uint32_t checksum = 0;
    };

    PlotOptions plot;
    std::string loadedDesign;
    std::string sessionLog;
    std::unique_ptr<SnapshotWriter> snapshots;
    std::vector<Committed> committed;
    bool plotsReady = false;
    bool stepsRun = false;
    bool placementWritten = false;

    void startStep();
    void loadDesign(const std::string &dirPath);
    void runScript();
    void preparePlots();
    void runGlobalPlacement();
    void runLegalization();
    void runDetailedPlacement();
    void writeBookshelf(const std::string &outputPath) const;
    void drawPictures();
    void finishAnimation();
    void commit(std::string_view stage);
    void reportSummary() const;

    template <class Stage, class Solution>
    void runStage(Stage &stage, void (ktDM::*apply)(const Solution &), std::string_view name);
};

FlowMgr::FlowMgr() : pImpl(std::make_unique<Impl>()) {
    bindTclCommands(*this);
}

FlowMgr::~FlowMgr() {
    unbindTclCommands(*this);
}

FlowMgr::FlowMgr(FlowMgr &&) noexcept = default;
FlowMgr &FlowMgr::operator=(FlowMgr &&) noexcept = default;

void FlowMgr::run(const kt_option &options) {
    pImpl->run(options);
}

void FlowMgr::observe(FlowObserver observer) {
    pImpl->observer = std::move(observer);
}

void FlowMgr::begin(const kt_option &options) {
    pImpl->begin(options);
    bindTclCommands(*this);
}

std::string FlowMgr::evaluate(const std::string &script) {
    return pImpl->evaluate(script);
}

void FlowMgr::finish() {
    pImpl->finish();
}

bool FlowMgr::hasDesign() const {
    return pImpl->db != nullptr;
}

bool FlowMgr::isFreshlyLoaded(const std::string &dirPath) const {
    return pImpl->isFreshlyLoaded(dirPath);
}

const std::string &FlowMgr::designPath() const {
    return pImpl->options.inputPath;
}

void FlowMgr::readDesign(const std::string &dirPath) {
    pImpl->readDesign(dirPath);
}

void FlowMgr::globalPlace() {
    pImpl->globalPlace();
}

void FlowMgr::legalize() {
    pImpl->legalize();
}

void FlowMgr::detailPlace() {
    pImpl->detailPlace();
}

std::string FlowMgr::writePlacement(const std::string &path) {
    return pImpl->writePlacement(path);
}

void FlowMgr::writePlots() {
    pImpl->writePlots();
}

double FlowMgr::hpwl() const {
    return pImpl->design().hpwl();
}

void FlowMgr::reportDesign() const {
    pImpl->reportDesign();
}

bool FlowMgr::checkPlacement() const {
    const ktDM &db = pImpl->design();
    db.isLegalizePlacement();
    return db.verify().empty();
}

void FlowMgr::Impl::begin(const kt_option &sessionOptions) {
    options = sessionOptions;
    if (options.getLogFile() == sessionLog) {
        return;
    }
    sessionLog = options.getLogFile();
    ktlog.configure(sessionLog, options.verbose);
    ktlog.beginStep("setup");
    options.report();
}

void FlowMgr::Impl::run(const kt_option &runOptions) {
    const bool alreadyLoaded = isFreshlyLoaded(runOptions.inputPath);
    begin(runOptions);
    if (!options.inputPath.empty() && !alreadyLoaded) {
        readDesign(options.inputPath);
    }
    runScript();
    if (!stepsRun) {
        if (!db) {
            ktlog.fatal("nothing to place: give a design directory, or read_design in the script");
        }
        runGlobalPlacement();
        runLegalization();
        runDetailedPlacement();
        writeBookshelf(options.getOutputPath());
    }
    finish();
}

std::string FlowMgr::Impl::evaluate(const std::string &script) {
    return tcl::TclMgr::instance().interpreter.eval(script).asString();
}

void FlowMgr::Impl::finish() {
    if (stepsRun && db && !placementWritten) {
        ktlog.warning("the placement was not written; call write_placement to save it");
    }
    drawPictures();
    if (!committed.empty()) {
        ktlog.beginStep("summary");
        TimerRegistry::instance().report();
        reportSummary();
    }
}

bool FlowMgr::Impl::isFreshlyLoaded(const std::string &dirPath) const {
    return db && !dirPath.empty() && dirPath == loadedDesign && !stepsRun && committed.size() == 1;
}

void FlowMgr::Impl::readDesign(const std::string &dirPath) {
    options.inputPath = dirPath;
    loadedDesign = dirPath;
    snapshots.reset();
    plotsReady = false;
    committed.clear();
    loadDesign(dirPath);
}

void FlowMgr::Impl::globalPlace() {
    startStep();
    runGlobalPlacement();
}

void FlowMgr::Impl::legalize() {
    startStep();
    runLegalization();
}

void FlowMgr::Impl::detailPlace() {
    startStep();
    runDetailedPlacement();
}

std::string FlowMgr::Impl::writePlacement(const std::string &path) {
    startStep();
    const std::string target = path.empty() ? options.getOutputPath() : path;
    writeBookshelf(target);
    placementWritten = true;
    return target;
}

void FlowMgr::Impl::writePlots() {
    startStep();
    drawPictures();
}

const ktDM &FlowMgr::Impl::design() const {
    if (!db) {
        throw std::logic_error("no design is loaded; call read_design first");
    }
    return *db;
}

void FlowMgr::Impl::reportDesign() const {
    const ktDM &loaded = design();
    loaded.report();
    loaded.reportStatistics();
    loaded.reportUtilisation();
}

void FlowMgr::Impl::startStep() {
    (void)design();
    stepsRun = true;
}

void FlowMgr::Impl::loadDesign(const std::string &dirPath) {
    ktlog.beginStep("load");
    ScopedTimer timer("load", true);

    std::error_code ec;
    if (!std::filesystem::is_directory(dirPath, ec)) {
        ktlog.fatal("cannot read input directory: {}", dirPath);
    }

    const std::unique_ptr<InputReader> reader = makeInputReader(dirPath);
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

void FlowMgr::Impl::runScript() {
    if (options.tclFile.empty()) {
        return;
    }
    try {
        tcl::TclMgr::instance().loadScript(options.tclFile);
    } catch (const tcl::Error &error) {
        ktlog.fatal("{}: {}", options.tclFile, error.what());
    }
}

void FlowMgr::Impl::preparePlots() {
    if (plotsReady) {
        return;
    }
    plotsReady = true;
    plot = PlotOptions::fromOptions(options.getPlotDir());
    Visualizer::instance().configure(plot.dir, plot);
    if (plot.enabled()) {
        snapshots = std::make_unique<SnapshotWriter>(snapshotFile(plot.dir));
        snapshots->holdBack(plot.frameBudget * 2 / 5);
    }
}

template <class Stage, class Solution>
void FlowMgr::Impl::runStage(Stage &stage, void (ktDM::*apply)(const Solution &),
                             std::string_view name) {
    (void)stage.run();
    ((*db).*apply)(stage.solution());
    commit(name);
}

void FlowMgr::Impl::runGlobalPlacement() {
    ScopedTimer timer("global placement", true);
    const GlobalPlacer placer = chosenGlobalPlacer();
    preparePlots();

    switch (placer) {
        case GlobalPlacer::Simpl: {
            simpl::flowMgr stage(*db);
            if (recordStage(stage.dm(), snapshots.get(), observer, {"simpl", "simpl", 0})) {
                recordSimplBounds(stage.dm(), *snapshots);
            }
            if (!stage.dm().options().fences) {
                ktlog.echo("SimPL: fence enforcement disabled by 'add_option simpl fences 0'");
            }
            runStage(stage, &ktDM::setSimplSolution, "global placement");
            break;
        }
        case GlobalPlacer::Ntuplace1: {
            ntuplace1::flowMgr stage(*db);
            runStage(stage, &ktDM::setNtuplace1Solution, "global placement");
            break;
        }
    }
}

void FlowMgr::Impl::runLegalization() {
    ScopedTimer timer("legalization", true);
    preparePlots();
    if (snapshots) {
        snapshots->holdBack(0);
    }

    if (const std::size_t tall = db->multiRowCells(); tall > 0) {
        multirow::flowMgr stage(*db);
        ktlog.echo("multi-row legalization ({} cell(s) taller than a row)", tall);
        recordStage(stage.dm(), snapshots.get(), observer, {"legalize_multi", "", 0});
        runStage(stage, &ktDM::setMultiRowSolution, "legalization");
        return;
    }

    abacus::flowMgr stage(*db);
    recordStage(stage.dm(), snapshots.get(), observer, {"legalize", "", 0});
    runStage(stage, &ktDM::setAbacusSolution, "legalization");
}

void FlowMgr::Impl::runDetailedPlacement() {
    ScopedTimer timer("detailed placement", true);
    preparePlots();
    fastdp::flowMgr stage(*db);
    recordStage(stage.dm(), snapshots.get(), observer, {"detailplace", "", 0});
    runStage(stage, &ktDM::setFastdpSolution, "detailed placement");
}

void FlowMgr::Impl::writeBookshelf(const std::string &outputPath) const {
    ktlog.beginStep("write");
    ScopedTimer timer("write", true);
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        ktlog.fatal("Cannot open output file: {}", outputPath);
    }
    out << std::setprecision(10);
    const Graph &graph = db->getGraph();
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const Vertex &cell = graph.getCell(v);
        out << cell.name << '\t' << cell.x << '\t' << cell.y
            << "\t: " << (cell.isFixed ? "N /FIXED" : "N") << '\n';
    }
}

void FlowMgr::Impl::drawPictures() {
    if (!snapshots || !db) {
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
    } else {
        std::error_code ec;
        std::filesystem::remove(snapshots->path(), ec);
    }
    snapshots.reset();
    plotsReady = false;
}

void FlowMgr::Impl::finishAnimation() {
    const Graph &graph = db->getGraph();
    Visualizer &viz = Visualizer::instance();
    const bool written =
        viz.finish(graph, cellCoordinates(graph, &Vertex::x), cellCoordinates(graph, &Vertex::y),
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

void FlowMgr::Impl::commit(std::string_view stage) {
    committed.push_back({std::string(stage), ktlog.currentStep(), db->hpwl(), db->checksum()});
    const Committed &entry = committed.back();
    ktlog.echo("committed to ktDM: HPWL {:.6e}, checksum {:08x}", entry.hpwl, entry.checksum);
    if (observer.committed) {
        observer.committed(stage, *db);
    }
}

void FlowMgr::Impl::reportSummary() const {
    const auto optionalSeconds = [](const TimerStats *time, double TimerStats::*field) {
        return time != nullptr ? std::format("{:.3f}s", time->*field) : std::string();
    };
    ktReportTable table("Run summary");
    table.setHeaders({"stage", "HPWL", "change", "wall", "cpu", "ktDM"});
    for (std::size_t i = 0; i < committed.size(); ++i) {
        const Committed &entry = committed[i];
        const TimerStats *time = TimerRegistry::instance().find(entry.stage);
        const double before = i > 0 ? committed[i - 1].hpwl : 0.0;
        table.addRow({entry.step == entry.stage ? entry.stage
                                                : std::format("{} ({})", entry.stage, entry.step),
                      std::format("{:.6e}", entry.hpwl),
                      before > 0.0 ? std::format("{:+.2f}%", 100.0 * (entry.hpwl / before - 1.0))
                                   : std::string(),
                      optionalSeconds(time, &TimerStats::wallSeconds),
                      optionalSeconds(time, &TimerStats::cpuSeconds),
                      std::format("{:08x}", entry.checksum)});
    }
    table.emit();
}

}  // namespace ktplace
