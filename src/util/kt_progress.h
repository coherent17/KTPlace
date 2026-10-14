// @file kt_progress.h
// Where a long stage says how far it has got, and where it is asked to stop.
//
// A placer iterates for seconds to minutes. A front end that hosts it -- the GUI,
// which runs the engine on a thread of its own -- wants to watch the placement
// move and the wirelength fall while that happens, and to be able to end the run
// early with what it has. This is the meeting point: the stage reports one
// sample per outer iteration, and checks the stop flag between iterations.
//
// Nothing here draws, and nothing is reported unless a host has installed an
// observer; the command-line flow installs none, so a stage that checks
// active() first costs nothing there. The stop flag is the one piece meant to be
// touched from another thread: requestStop() is what a Stop button calls.
//
// A stage that stops early still finishes normally -- it returns the best
// placement it has, which the session commits as usual. Stopping the rest of a
// script is the command layer's business (see tcl/kt_tclShell.cc).

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ktplace {

/// One report from a running stage.
struct ProgressSample {
    /// Which run of a stage this belongs to; a new run gets a new number (see
    /// Progress::beginRun), so an observer knows when to start a new plot.
    std::size_t run = 0;
    /// The stage ("simpl") and the phase of it ("init", "global").
    std::string stage;
    std::string phase;
    /// The iteration within the phase, and the most there can be.
    std::size_t iter = 0;
    std::size_t total = 0;
    /// The wirelength of the placement shown, and of the stage's other bound
    /// when it keeps one (SimPL's lower bound); 0 when there is none.
    double hpwl = 0.0;
    double hpwlLower = 0.0;
    /// The scaled bin overflow of the placement shown; negative when the stage
    /// does not measure one.
    double overflow = -1.0;
    /// Every cell's lower-left corner, indexed by cell id. Filled only when the
    /// observer asked for positions; empty otherwise.
    std::vector<float> x;
    std::vector<float> y;
};

class Progress {
public:
    using Observer = std::function<void(ProgressSample &&)>;

    static Progress &instance() noexcept;

    Progress(const Progress &) = delete;
    Progress &operator=(const Progress &) = delete;

    /// Install the observer, called on the reporting stage's thread. With
    /// @p positions, samples carry every cell's position, which costs the stage
    /// a copy of the placement per report.
    void setObserver(Observer observer, bool positions);
    void clearObserver();

    /// Whether anyone is listening; a stage skips building samples otherwise.
    [[nodiscard]] bool active() const;
    /// Whether the observer wants positions in its samples.
    [[nodiscard]] bool wantsPositions() const;

    /// A new run of a stage begins; returns its number for the samples.
    std::size_t beginRun() noexcept;
    /// Hand @p sample to the observer, if there is one.
    void report(ProgressSample &&sample) const;

    /// Ask the running stage to stop at its next check. Any thread.
    void requestStop() noexcept;
    /// Forget a stop request; the host does this before it starts a new job.
    void clearStop() noexcept;
    [[nodiscard]] bool stopRequested() const noexcept;

private:
    Progress() = default;

    mutable std::mutex mutex_;
    Observer observer_;
    bool positions_ = false;
    std::atomic<std::size_t> runs_{0};
    std::atomic<bool> stop_{false};
};

}  // namespace ktplace
