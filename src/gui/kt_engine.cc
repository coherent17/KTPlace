// @file kt_engine.cc
// Implementation of Engine: the session and shell on their own thread.

#include "gui/kt_engine.h"

#include "session/kt_session.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <string>
#include <utility>

namespace ktplace {

Engine::Engine() {
    context_ = new QObject;
    context_->moveToThread(&thread_);
    thread_.setObjectName("ktplace-engine");
    thread_.start();
    QMetaObject::invokeMethod(
        context_,
        [this] {
            session_ = std::make_unique<ktSession>();
            shell_ = std::make_unique<TclShell>(*session_);
            for (const std::string &name : shell_->commandNames()) {
                commands_ << QString::fromStdString(name);
            }
        },
        Qt::BlockingQueuedConnection);
}

Engine::~Engine() {
    // Stop a running stage rather than wait for it to converge.
    Progress::instance().requestStop();
    // Queued behind any script still running, so this waits for it.
    QMetaObject::invokeMethod(
        context_,
        [this] {
            Progress::instance().clearObserver();
            Progress::instance().clearStop();
            shell_.reset();
            session_.reset();
        },
        Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
    // Not from inside the call above: that runs as an event delivered to
    // context_, and an object must not be deleted while handling its own event.
    delete context_;
}

void Engine::submit(const QString &script, QObject *receiver, Done done) {
    ++pending_;
    QPointer<QObject> target(receiver);
    QMetaObject::invokeMethod(
        context_,
        [this, text = script.toStdString(), target, done = std::move(done)] {
            // A stop asked for during the previous script is not for this one.
            Progress::instance().clearStop();
            TclShell::Result result = shell_->eval(text);
            --pending_;
            if (target.isNull()) {
                return;
            }
            // Queued to the receiver, which drops the call if the receiver is
            // destroyed before the event is delivered.
            QMetaObject::invokeMethod(
                target.data(),
                [done, result = std::move(result)] {
                    done(result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Engine::query(std::function<void(const ktSession &)> work, QObject *receiver,
                   std::function<void()> done) {
    QPointer<QObject> target(receiver);
    QMetaObject::invokeMethod(
        context_,
        [this, target, work = std::move(work), done = std::move(done)] {
            work(*session_);
            if (!target.isNull()) {
                QMetaObject::invokeMethod(target.data(), done, Qt::QueuedConnection);
            }
        },
        Qt::QueuedConnection);
}

void Engine::watchProgress(QObject *receiver, Live live) {
    QPointer<QObject> target(receiver);
    QMetaObject::invokeMethod(
        context_,
        [this, target, live = std::move(live)] {
            // Called on this thread, from inside the stage, between two of its
            // iterations: the session's cells are the design as it was read
            // and the stage's placement is in the sample, so the snapshot is
            // taken from the two.
            //
            // A watcher destroyed before its snapshot arrived never cleared the
            // flag, so a new one starts clear.
            framePending_->store(false);
            Progress::instance().setObserver(
                [this, target, live](ProgressSample &&sample) {
                    if (target.isNull()) {
                        return;
                    }
                    LiveUpdate update;
                    if (!sample.x.empty() && !framePending_->exchange(true)) {
                        update.layout =
                            LayoutSnapshot::capture(*session_, sample.x, sample.y, sample.hpwl);
                    }
                    sample.x = {};
                    sample.y = {};
                    update.sample = std::move(sample);
                    QMetaObject::invokeMethod(
                        target.data(),
                        [live, update = std::move(update), pending = framePending_] {
                            if (update.layout) {
                                pending->store(false);
                            }
                            live(update);
                        },
                        Qt::QueuedConnection);
                },
                /*positions=*/true);
        },
        // Queued, not blocking: it takes effect before any script submitted
        // after it, without holding the caller up behind a running one.
        Qt::QueuedConnection);
}

void Engine::requestStop() {
    if (busy()) {
        Progress::instance().requestStop();
    }
}

void Engine::setGlobal(const QString &key, const QString &value) {
    QMetaObject::invokeMethod(
        context_,
        [this, k = key.toStdString(), v = value.toStdString()] {
            shell_->setGlobal(k, v);
        },
        Qt::BlockingQueuedConnection);
}

}  // namespace ktplace
