#include "gui/kt_runner.h"

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "tcl/kt_tcl.h"
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
    Impl(QObject *owner, Callbacks runCallbacks)
        : context(owner), callbacks(std::move(runCallbacks)) {}

    QObject *context;
    Callbacks callbacks;
    std::unique_ptr<QThread> thread;
    std::atomic<bool> stopRequested = false;
    std::shared_ptr<std::atomic<bool>> framePending = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<FlowMgr> session;
    std::shared_ptr<const LayoutGeometry> geometry;

    void openSession();
    void beginSession(const RunRequest &request);
    void launch(std::function<RunResult()> job);
    RunResult load(const RunRequest &request);
    RunResult evaluate(const std::string &command, const RunRequest &request);

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

void Runner::Impl::openSession() {
    session = std::make_unique<FlowMgr>();
    geometry.reset();
    session->observe({.committed =
                          [this](std::string_view stage, const ktDM &db) {
                              if (!geometry || stage == "load") {
                                  geometry = LayoutGeometry::of(db);
                              }
                              deliver(LayoutFrame::getCommitted(geometry, db, std::string(stage)),
                                      false);
                              if (stopRequested) {
                                  throw StopRequested{};
                              }
                          },
                      .frame =
                          [this](std::string_view stage, const std::vector<float> &x,
                                 const std::vector<float> &y, double hpwl) {
                              if (stopRequested) {
                                  throw StopRequested{};
                              }
                              deliver({geometry, x, y, hpwl, std::string(stage)}, true);
                          }});
}

RunResult Runner::Impl::place(const RunRequest &request) {
    OptionDb::getInstance().clear();
    const bool reuseSession = session && session->isFreshlyLoaded(request.design);
    if (!reuseSession) {
        TimerRegistry::getInstance().reset();
    }

    kt_option options;
    options.inputPath = request.design;
    options.workDir = request.workDir;
    options.tclFile = request.script;
    options.mode = Mode::Gui;

    std::error_code ec;
    std::filesystem::create_directories(options.workDir, ec);
    if (!reuseSession) {
        openSession();
    }
    RunResult result;
    try {
        session->run(options);
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
    return result;
}

void Runner::Impl::beginSession(const RunRequest &request) {
    OptionDb::getInstance().clear();
    kt_option options;
    options.inputPath = request.design;
    options.workDir = request.workDir;
    options.mode = Mode::Gui;
    std::error_code ec;
    std::filesystem::create_directories(options.workDir, ec);
    openSession();
    session->begin(options);
}

RunResult Runner::Impl::load(const RunRequest &request) {
    session.reset();
    beginSession(request);
    try {
        session->readDesign(request.design);
        return {RunResult::Outcome::Loaded, request.design};
    } catch (const FatalError &error) {
        return {RunResult::Outcome::Failed, error.what()};
    } catch (const std::exception &error) {
        return {RunResult::Outcome::Failed, error.what()};
    }
}

RunResult Runner::Impl::evaluate(const std::string &command, const RunRequest &request) {
    if (!session) {
        beginSession(request);
    }
    RunResult result;
    result.fromConsole = true;
    try {
        result.message = session->evaluate(command);
        result.outcome = RunResult::Outcome::Evaluated;
    } catch (const tcl::Error &error) {
        result.message = error.what();
    } catch (const StopRequested &) {
        result = {RunResult::Outcome::Stopped, "stopped", true};
    } catch (const FatalError &error) {
        result.message = error.what();
    } catch (const std::exception &error) {
        result.message = error.what();
    }
    return result;
}

void Runner::Impl::launch(std::function<RunResult()> job) {
    join();
    stopRequested = false;
    framePending->store(false);
    thread.reset(QThread::create([this, job = std::move(job)] {
        const RunResult result = job();
        post([onFinished = callbacks.finished, result] {
            if (onFinished) {
                onFinished(result);
            }
        });
    }));
    thread->start();
}

Runner::Runner(QObject *context, Callbacks callbacks)
    : pImpl(std::make_unique<Impl>(context, std::move(callbacks))) {}

Runner::~Runner() {
    stop();
    pImpl->join();
    pImpl->session.reset();
    ktlog.shutdown();
}

bool Runner::start(const RunRequest &request) {
    if (isRunning()) {
        return false;
    }
    pImpl->launch([impl = pImpl.get(), request] {
        return impl->place(request);
    });
    return true;
}

bool Runner::open(const RunRequest &request) {
    if (isRunning()) {
        return false;
    }
    pImpl->launch([impl = pImpl.get(), request] {
        return impl->load(request);
    });
    return true;
}

bool Runner::execute(const std::string &command, const RunRequest &request) {
    if (isRunning()) {
        return false;
    }
    pImpl->launch([impl = pImpl.get(), command, request] {
        return impl->evaluate(command, request);
    });
    return true;
}

void Runner::stop() {
    pImpl->stopRequested = true;
}

bool Runner::isRunning() const {
    return pImpl->thread && !pImpl->thread->isFinished();
}

}  // namespace ktplace::gui
