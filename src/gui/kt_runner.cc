#include "gui/kt_runner.h"

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <atomic>
#include <exception>
#include <filesystem>
#include <utility>

namespace ktplace::gui {

namespace {

struct StopRequested {};

}  // namespace

class Runner::Impl {
public:
    Impl(QObject *context, Callbacks callbacks)
        : context(context), callbacks(std::move(callbacks)) {}

    QObject *context;
    Callbacks callbacks;
    std::unique_ptr<QThread> thread;
    std::atomic<bool> stopRequested = false;
    std::shared_ptr<std::atomic<bool>> framePending = std::make_shared<std::atomic<bool>>(false);

    void post(std::function<void()> work) const {
        QMetaObject::invokeMethod(context, std::move(work), Qt::QueuedConnection);
    }

    void deliver(LayoutFrame frame, bool throttled) const {
        if (throttled && framePending->exchange(true)) {
            return;
        }
        post([onFrame = callbacks.frame, pending = framePending, frame = std::move(frame),
              throttled]() mutable {
            if (throttled) {
                pending->store(false);
            }
            if (onFrame) {
                onFrame(std::move(frame));
            }
        });
    }

    void join() {
        if (thread) {
            thread->wait();
            thread.reset();
        }
    }

    RunResult place(const RunRequest &request);
};

RunResult Runner::Impl::place(const RunRequest &request) {
    OptionDb::instance().clear();
    OptionDb::instance().set(Component::Visualizer, "plot", OptionValue{0});
    OptionDb::instance().set(Component::Simpl, "trace_every", OptionValue{1});
    TimerRegistry::instance().reset();

    kt_option options;
    options.inputPath = request.design;
    options.workDir = request.workDir;
    options.tclFile = request.script;

    std::error_code ec;
    std::filesystem::create_directories(options.workDir, ec);
    ktlog.configure(options.getLogFile(), false);
    ktlog.beginStep("setup");
    ktlog.echo("KTPlace GUI v{}: placing {}", version, options.inputPath);

    std::shared_ptr<const LayoutGeometry> geometry;
    FlowMgr flow;
    flow.observe({.committed =
                      [this, &geometry](std::string_view stage, const ktDM &db) {
                          if (!geometry) {
                              geometry = LayoutGeometry::of(db);
                          }
                          deliver(LayoutFrame::committed(geometry, db, std::string(stage)), false);
                          if (stopRequested) {
                              throw StopRequested{};
                          }
                      },
                  .frame =
                      [this, &geometry](std::string_view stage, const std::vector<float> &x,
                                        const std::vector<float> &y, double hpwl) {
                          if (stopRequested) {
                              throw StopRequested{};
                          }
                          deliver({geometry, x, y, hpwl, std::string(stage)}, true);
                      }});

    RunResult result;
    try {
        flow.run(options);
        result = {RunResult::Outcome::Placed, options.getOutputPath()};
        ktlog.echo("Placement completed successfully!");
    } catch (const StopRequested &) {
        result = {RunResult::Outcome::Stopped, "stopped"};
        ktlog.warning("run stopped before it finished; nothing was written");
    } catch (const FatalError &error) {
        result = {RunResult::Outcome::Failed, error.what()};
    } catch (const std::exception &error) {
        result = {RunResult::Outcome::Failed, error.what()};
        ktlog.warning("run failed: {}", error.what());
    }
    ktlog.shutdown();
    return result;
}

Runner::Runner(QObject *context, Callbacks callbacks)
    : pImpl(std::make_unique<Impl>(context, std::move(callbacks))) {}

Runner::~Runner() {
    stop();
    pImpl->join();
}

bool Runner::start(const RunRequest &request) {
    if (running()) {
        return false;
    }
    pImpl->join();
    pImpl->stopRequested = false;
    pImpl->framePending->store(false);
    Impl *impl = pImpl.get();
    pImpl->thread.reset(QThread::create([impl, request] {
        const RunResult result = impl->place(request);
        impl->post([onFinished = impl->callbacks.finished, result] {
            if (onFinished) {
                onFinished(result);
            }
        });
    }));
    pImpl->thread->start();
    return true;
}

void Runner::stop() {
    pImpl->stopRequested = true;
}

bool Runner::running() const {
    return pImpl->thread && !pImpl->thread->isFinished();
}

}  // namespace ktplace::gui
