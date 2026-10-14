// @file kt_engine.h
// The engine thread behind the GUI: one ktSession and the TclShell over it,
// living on a thread of their own so a placement never freezes the window.
//
// Everything the GUI does to the design is a Tcl script submitted here, which
// keeps the GUI a thin client of the command layer (see tcl/kt_tclShell.h):
// a menu action and a typed command are the same thing, and both are in the log.
//
// A Tcl interpreter may only be used from the thread that created it, so the
// shell is built, used and destroyed on the engine thread. Scripts run one at a
// time in submission order. Results come back on the thread of the receiver
// passed to submit(), normally the GUI thread.
//
// While a stage runs, it reports its progress (util/kt_progress.h); the engine
// forwards each report to the receiver given to watchProgress(), with a
// snapshot of the placement the stage holds at that moment. Snapshots are
// dropped, never queued, while the previous one has not been delivered, so a
// window that draws slower than the stage iterates sees the latest placement
// rather than a backlog. requestStop() ends the running stage early and fails
// the rest of its script.
//
// No Q_OBJECT here or anywhere in the GUI: cross-thread calls are queued
// lambdas (QMetaObject::invokeMethod with a context object), which needs no moc.
//
// The progress observer is process-wide, so there is one Engine at a time.

#pragma once

#include "gui/kt_layoutSnapshot.h"
#include "tcl/kt_tclShell.h"
#include "util/kt_progress.h"

#include <QStringList>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

class QObject;

namespace ktplace {

class ktSession;

class Engine {
public:
    using Done = std::function<void(const TclShell::Result &)>;

    /// One progress report as the GUI receives it: the sample without its
    /// position arrays, and the placement they described as a snapshot, or
    /// null when that snapshot was dropped (see the file comment).
    struct LiveUpdate {
        ProgressSample sample;
        std::shared_ptr<const LayoutSnapshot> layout;
    };
    using Live = std::function<void(const LiveUpdate &)>;

    /// Starts the engine thread and builds the session and shell on it.
    Engine();
    /// Waits for a running script to finish, then tears the shell down on its
    /// own thread.
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /// Evaluate @p script on the engine thread. @p done is then called on
    /// @p receiver's thread, unless @p receiver has been destroyed by then.
    void submit(const QString &script, QObject *receiver, Done done);

    /// Run @p work on the engine thread with read access to the session, then
    /// @p done on @p receiver's thread (dropped if the receiver is gone).
    ///
    /// For what the view needs and a user did not type -- a snapshot to draw,
    /// the details of a clicked cell -- so it stays out of the console and the
    /// log. It queues behind any running script, so it sees the design as that
    /// script left it. @p work hands its result to @p done through captures.
    void query(std::function<void(const ktSession &)> work, QObject *receiver,
               std::function<void()> done);

    /// Send every progress report to @p live on @p receiver's thread. One
    /// watcher; a second call replaces the first.
    void watchProgress(QObject *receiver, Live live);

    /// Ask the running stage to stop, from any thread. It returns the best
    /// placement it has, and the rest of the script fails. Cleared when the
    /// next script starts.
    void requestStop();

    /// Set element @p key of the shell's `ktplace` array (see TclShell).
    void setGlobal(const QString &key, const QString &value);

    /// Whether a submitted script has not finished yet.
    [[nodiscard]] bool busy() const {
        return pending_.load() > 0;
    }

    /// The shell's own commands, for completion; fixed once the engine is up.
    [[nodiscard]] const QStringList &commandNames() const {
        return commands_;
    }

private:
    QThread thread_;
    QObject *context_ = nullptr;  // lives on thread_; the target of queued calls
    std::unique_ptr<ktSession> session_;
    std::unique_ptr<TclShell> shell_;
    QStringList commands_;
    std::atomic<int> pending_{0};
    // A snapshot is on its way to the watcher and not delivered yet.
    std::shared_ptr<std::atomic<bool>> framePending_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace ktplace
