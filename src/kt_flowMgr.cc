// @file kt_flowMgr.cc
// Implementation of FlowMgr: the command line's fixed script over ktSession.

#include "kt_flowMgr.h"

#include "kt_option.h"

#include "datamodel/kt_optionDb.h"
#include "session/kt_session.h"
#include "tcl/kt_tcl.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_plotOptions.h"

#include <memory>
#include <string>

namespace ktplace {

class FlowMgr::Impl {
public:
    void run(const kt_option &options);

private:
    ktSession session;
};


FlowMgr::FlowMgr() : pImpl(std::make_unique<Impl>()) {}
FlowMgr::~FlowMgr() = default;
FlowMgr::FlowMgr(FlowMgr &&) noexcept = default;
FlowMgr &FlowMgr::operator=(FlowMgr &&) noexcept = default;

void FlowMgr::run(const kt_option &options) {
    pImpl->run(options);
}

void FlowMgr::configure(const kt_option &options) {
    if (options.tclFile.empty()) {
        return;
    }
    try {
        tcl::TclMgr::instance().loadScript(options.tclFile);
    } catch (const tcl::Error &error) {
        ktlog.fatal("{}: {}", options.tclFile, error.what());
    }
}


void FlowMgr::Impl::run(const kt_option &options) {
    configure(options);
    PlotOptions plot = options.plot ? PlotOptions::fromOptions() : PlotOptions{};
    plot.dir = options.getPlotDir();
    session.setPlotOptions(plot);

    session.readDesign(options.inputPath);
    session.reportDesign();
    // The placer holds back part of the animation's frame budget for the stages
    // after it, and legalize() releases it; see ktSession.
    // Which placer runs is the script's choice (`add_option flow algorithm
    // ntuplace1`); SimPL when nothing says otherwise.
    const std::string algorithm =
        getComponentOption<components::Id::Flow, std::string>("algorithm").value_or("simpl");
    if (algorithm != "simpl" && algorithm != "ntuplace1") {
        ktlog.fatal("unknown placer '{}'; expected simpl or ntuplace1", algorithm);
    }
    session.globalPlace(algorithm);
    // Each stage's result is checked as it is committed (ktDM), so the log says
    // which stage broke the placement, if one did.
    session.legalize();
    session.detailPlace();
    // The still and the animation come after the last stage, so both show the
    // placement that is written.
    session.renderFinalImage();
    (void)session.finishAnimation();
    session.writeDesign(options.getOutputPath());
    TimerRegistry::instance().report();
}

}  // namespace ktplace
