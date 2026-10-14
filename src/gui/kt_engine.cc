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
    // Queued behind any script still running, so this waits for it.
    QMetaObject::invokeMethod(
        context_,
        [this] {
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

void Engine::setGlobal(const QString &key, const QString &value) {
    QMetaObject::invokeMethod(
        context_,
        [this, k = key.toStdString(), v = value.toStdString()] {
            shell_->setGlobal(k, v);
        },
        Qt::BlockingQueuedConnection);
}

}  // namespace ktplace
