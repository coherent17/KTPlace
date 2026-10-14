// @file kt_progress.cc
// Implementation of Progress.

#include "util/kt_progress.h"

#include <utility>

namespace ktplace {

Progress &Progress::instance() noexcept {
    static Progress progress;
    return progress;
}

void Progress::setObserver(Observer observer, bool positions) {
    const std::lock_guard lock(mutex_);
    observer_ = std::move(observer);
    positions_ = positions && observer_ != nullptr;
}

void Progress::clearObserver() {
    setObserver(nullptr, false);
}

bool Progress::active() const {
    const std::lock_guard lock(mutex_);
    return observer_ != nullptr;
}

bool Progress::wantsPositions() const {
    const std::lock_guard lock(mutex_);
    return positions_;
}

std::size_t Progress::beginRun() noexcept {
    return ++runs_;
}

void Progress::report(ProgressSample &&sample) const {
    Observer observer;
    {
        // Copied out, so the observer runs unlocked and may itself ask
        // wantsPositions() or install another observer.
        const std::lock_guard lock(mutex_);
        observer = observer_;
    }
    if (observer) {
        observer(std::move(sample));
    }
}

void Progress::requestStop() noexcept {
    stop_.store(true);
}

void Progress::clearStop() noexcept {
    stop_.store(false);
}

bool Progress::stopRequested() const noexcept {
    return stop_.load();
}

}  // namespace ktplace
