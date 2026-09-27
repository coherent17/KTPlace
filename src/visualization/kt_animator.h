/**
 * @file kt_animator.h
 * @brief One animation sink for a whole placement run.
 */

#pragma once

#include "visualization/kt_plotter.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ktplace {

/**
 * @brief Collects placement stills from every stage of a run into one GIF.
 *
 * A placement run is global placement, then legalization, then detailed
 * placement, and each of those moves cells for a different reason. Watching
 * only the first and being told the run finished is not much of a record: the
 * interesting part is often the legalizer pulling a scattered placement back
 * onto its rows, which happens after the placer has stopped drawing.
 *
 * So the animation belongs to the run, not to any one stage. Every stage hands
 * its frames here and the frames come out in the order they were recorded,
 * whoever produced them. That has one concrete consequence worth stating: there
 * is a single frame counter for the whole run, so a design that records a
 * conjugate-gradient iterate every iteration of every outer iteration runs out
 * of budget during global placement and contributes nothing from legalization.
 * The per-stage counters this replaces would each have had a full budget, and
 * the tail of the run -- the part that actually made the placement legal --
 * would have been the part to get squeezed out. The cap is therefore reported
 * loudly rather than absorbed quietly.
 *
 * This is a process-wide singleton because it is a property of the run, not of
 * any component: the three stages are constructed independently and have no
 * common owner to pass it through. It holds no state until configure() is
 * called, and configure() resets the counter, so a run that never configures it
 * -- every unit test, a run with no plot directory -- simply records nothing.
 * A second configure() in the same process is a fresh run, not an append.
 */
class PlacementAnimator {
public:
    /// The run's animator. Configured by the flow, fed by every stage.
    [[nodiscard]] static PlacementAnimator &instance();

    PlacementAnimator(const PlacementAnimator &) = delete;
    PlacementAnimator &operator=(const PlacementAnimator &) = delete;

    /**
     * @brief Start a new animation.
     *
     * @param outDir    directory the GIF is created in; created if missing
     * @param maxFrames ceiling on recorded stills for the whole run
     * @param delayCs   delay between GIF frames, in hundredths of a second
     */
    void configure(const std::string &outDir, std::size_t maxFrames, int delayCs,
                   int blend = 3);

    /// Forget the current run. Recorded frames are left on disk.
    void reset();

    /**
     * @brief In-between frames emitted per recorded placement.
     *
     * Consecutive solves move cells by a small fraction of a cell width, so a
     * frame per iteration is a series of near-identical images separated by a
     * jump, and the motion reads as a flicker rather than as cells travelling.
     * Blending renders the straight line between two placements, which at these
     * displacements is indistinguishable from the motion being interpolated, and
     * turns the jump into movement.
     *
     * Implemented by holding only the previous placement, so the cost is one
     * extra copy of the coordinates and `blend` times the rendering -- not
     * `blend` times the memory.
     */
    [[nodiscard]] int blend() const {
        return blend_;
    }

    /// True once configure() has armed this animator for a run.
    [[nodiscard]] bool enabled() const {
        return enabled_;
    }

    /// True once the frame ceiling has been reached.
    [[nodiscard]] bool capped() const {
        return capped_;
    }

    /// Frames recorded so far this run.
    [[nodiscard]] std::size_t frameCount() const {
        return frame_;
    }

    /**
     * @brief Hold frames back from the stage that is about to run.
     *
     * A cap that is first-come-first-served spends itself on whichever stage
     * records the most frames, which for a real design is global placement: a
     * conjugate-gradient iterate every few iterations of every outer iteration
     * is thousands of stills, and it will consume the entire budget before the
     * legalizer draws anything. The result is an animation that stops exactly
     * where the placement stops being interesting -- all of the spreading, none
     * of the legalizing.
     *
     * So the flow holds back a share of the budget while the placer runs and
     * releases it before legalization, which guarantees the tail is always
     * representable no matter how fine the placer's cadence is.
     *
     * @param n frames to keep in reserve; 0 means no reservation
     */
    void holdBack(std::size_t n);

    /// Frames still recordable, ignoring any hold-back.
    [[nodiscard]] std::size_t budget() const {
        return frame_ >= maxFrames_ ? 0 : maxFrames_ - frame_;
    }

    /**
     * @brief Rasterise the current placement as the next frame of the run.
     *
     * A no-op when the animator is not configured, when the cap is reached, or
     * when @p x and @p y are not one entry per graph vertex. The cap is checked
     * before the work, not after, so a capped run costs nothing.
     *
     * @param note human-readable stage label, drawn into the frame
     */
    void record(const Graph &g, const std::vector<float> &x, const std::vector<float> &y,
                const BBox &die, std::size_t step, std::size_t total, double hpwl,
                double hpwlInitial, double resid, const std::string &note);

    /**
     * @brief Write the collected stills as one animated GIF.
     *
     * @return true if at least two stills were found and the GIF was written
     */
    [[nodiscard]] bool finish(const std::string &gifName = "placement.gif") const;

private:
    PlacementAnimator() = default;

    std::filesystem::path dir_;
    std::size_t maxFrames_ = 480;
    int delayCs_ = 12;
    int blend_ = 3;
    /// The previous recorded placement, kept only to interpolate from.
    std::vector<float> prevX_, prevY_;
    bool havePrev_ = false;
    std::size_t frame_ = 0;
    std::size_t held_ = 0;
    bool enabled_ = false;
    bool capped_ = false;
};

}  // namespace ktplace
