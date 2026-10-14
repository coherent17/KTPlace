// @file kt_flowMgr.cc
// Implementation of FlowMgr: the command line's fixed script over ktSession.

#include "kt_flowMgr.h"

#include "kt_option.h"

#include "session/kt_session.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_plotOptions.h"

#include <memory>

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


void FlowMgr::Impl::run(const kt_option &options) {
    PlotOptions plot = options.plot ? PlotOptions::fromOptions() : PlotOptions{};
    plot.dir = options.getPlotDir();
    session.setPlotOptions(plot);

    session.readDesign(options.inputPath);
    session.reportDesign();
    // The placer holds back part of the animation's frame budget for the stages
    // after it, and legalize() releases it; see ktSession.
    session.globalPlace(options.algorithm);
    session.legalize();
    // Checked after each stage that should leave the placement legal, so the log
    // says which stage broke it.
    (void)session.reportPlacementCheck("after legalization");
    session.detailPlace();
    (void)session.reportPlacementCheck("after detail placement");
    // The still and the animation come after the last stage, so both show the
    // placement that is written.
    session.renderFinalImage();
    (void)session.finishAnimation();
    session.writeDesign(options.getOutputPath());
    TimerRegistry::instance().report();
}

}  // namespace ktplace
