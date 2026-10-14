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
// No Q_OBJECT here or anywhere in the GUI: cross-thread calls are queued
// lambdas (QMetaObject::invokeMethod with a context object), which needs no moc.

#pragma once

#include "tcl/kt_tclShell.h"

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
};

}  // namespace ktplace
