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
    Impl(QObject *context, Callbacks callbacks)
        : context(context), callbacks(std::move(callbacks)) {}

    QObject *context;
    Callbacks callbacks;
    std::unique_ptr<QThread> thread;
    std::atomic<bool> stopRequested = false;
    std::shared_ptr<std::atomic<bool>> framePending = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<FlowMgr> session;
    std::shared_ptr<const LayoutGeometry> geometry;

    void openSession();
    void launch(std::function<RunResult()> job);
    RunResult evaluate(const std::string &command, const std::string &workDir);

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
                              deliver(LayoutFrame::committed(geometry, db, std::string(stage)),
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
    OptionDb::instance().clear();
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

    openSession();
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

RunResult Runner::Impl::evaluate(const std::string &command, const std::string &workDir) {
    if (!session) {
        kt_option options;
        options.workDir = workDir;
        std::error_code ec;
        std::filesystem::create_directories(options.workDir, ec);
        ktlog.configure(options.getLogFile(), false);
        openSession();
        session->begin(options);
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
    if (running()) {
        return false;
    }
    pImpl->launch([impl = pImpl.get(), request] {
        return impl->place(request);
    });
    return true;
}

bool Runner::execute(const std::string &command, const std::string &workDir) {
    if (running()) {
        return false;
    }
    pImpl->launch([impl = pImpl.get(), command, workDir] {
        return impl->evaluate(command, workDir);
    });
    return true;
}

void Runner::stop() {
    pImpl->stopRequested = true;
}

bool Runner::running() const {
    return pImpl->thread && !pImpl->thread->isFinished();
}

}  // namespace ktplace::gui
