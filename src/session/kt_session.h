// @file kt_session.h
// One design and the stages that act on it, each callable on its own.
//
// FlowMgr runs these in a fixed order for the command line. A console or a GUI
// calls them one at a time, in whatever order the user asks for, and reads the
// placement back between calls; that is what this class is for.

#pragma once

#include "datamodel/kt_dm.h"
#include "visualization/kt_plotOptions.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

class ktSession {
public:
    /// One stage run on the current design, for a run history.
    struct StageRecord {
        /// "read_design", "global_place", "legalize", "detail_place" or
        /// "check_placement" -- the command's name.
        std::string stage;
        /// What ran: the reader's format, the algorithm, the legalizer, or the
        /// check's verdict ("legal", "3 defects").
        std::string detail;
        /// The wirelength once the stage was done.
        double hpwl = 0.0;
        double seconds = 0.0;
        /// The stage was asked to stop and returned early.
        bool stopped = false;
    };

    ktSession();
    ~ktSession();

    ktSession(const ktSession &) = delete;
    ktSession &operator=(const ktSession &) = delete;
    ktSession(ktSession &&) noexcept;
    ktSession &operator=(ktSession &&) noexcept;

    // ------------------------------------------------------------ pictures

    /// What the stages draw and where. `options.dir` empty means draw nothing,
    /// which is the default. Takes effect at the next stage that draws; the
    /// animation of the current run starts over.
    void setPlotOptions(const PlotOptions &options);
    [[nodiscard]] const PlotOptions &plotOptions() const noexcept;

    // ------------------------------------------------------------ the design

    /// Read the design in `dir`, auto-detecting its format. Replaces any design
    /// already loaded. Fails via ktlog.fatal when the directory is missing, no
    /// reader recognises it, or the reader fails.
    void readDesign(const std::string &dir);
    [[nodiscard]] bool hasDesign() const noexcept;
    /// The directory the design was read from; empty before the first read.
    [[nodiscard]] const std::string &designPath() const noexcept;
    /// Counts the designs read so far, so a caller can tell a re-read of the
    /// same directory from the design it already knew.
    [[nodiscard]] std::size_t designSerial() const noexcept;
    /// The stages run on the current design, oldest first, its read included.
    /// A new readDesign() starts the log over.
    [[nodiscard]] const std::vector<StageRecord> &stageLog() const noexcept;

    /// Log the design summary and its utilisation.
    void reportDesign() const;

    // ------------------------------------------------------------ the stages
    //
    // Each one needs a loaded design and fails via ktlog.fatal without one. They
    // do not otherwise insist on an order: each starts from whatever placement
    // the database holds.

    /// Global placement: "simpl" or "ntuplace1".
    void globalPlace(const std::string &algorithm);
    /// Abacus, or the row-slicing legalizer when the design has cells taller
    /// than a row.
    void legalize();
    /// FastDP detailed placement.
    void detailPlace();

    /// Draw the high-resolution still of the current placement (final.png).
    void renderFinalImage();
    /// Hold the current placement and assemble the run's animation
    /// (placement.gif). Returns whether a GIF was written.
    bool finishAnimation();

    /// Write the placement as a Bookshelf .pl.
    void writeDesign(const std::string &path) const;

    // ------------------------------------------------------------ queries

    [[nodiscard]] double hpwl() const;
    /// Everything wrong with the current placement; empty when it is legal.
    [[nodiscard]] std::vector<ktDM::Defect> checkPlacement() const;
    /// checkPlacement(), logged as the independent placement-check table, with a
    /// warning when the placement is not legal. `stage` names the point in the
    /// run ("after legalization"). Returns the defects.
    std::vector<ktDM::Defect> reportPlacementCheck(const std::string &stage) const;
    /// Movable cells taller than one and a half of the shortest row; non-zero
    /// sends legalize() to the multi-row legalizer.
    [[nodiscard]] std::size_t multiRowCells() const;

    // ------------------------------------------------------------ read-only views
    //
    // For a canvas: references into the database, valid until the next
    // readDesign(). A stage moves cells in place, so the views do not go stale
    // across stages.

    [[nodiscard]] const ktDM &design() const;
    [[nodiscard]] const DieInfo &die() const;
    [[nodiscard]] const Graph &graph() const;
    /// Every cell, terminals included, indexed by cell id; x/y are the current
    /// position (lower-left corner).
    [[nodiscard]] std::span<const Vertex> cells() const;
    /// The placement die box {xMin, yMin, xMax, yMax}.
    [[nodiscard]] std::array<double, 4> placementDieBox() const;
    /// The current placement as float arrays, the shape every draw call takes.
    /// A copy, so not for the hot path; cells() is.
    [[nodiscard]] std::pair<std::vector<float>, std::vector<float>> positions() const;

private:
    /// The loaded design and the stage the call is for, or ktlog.fatal.
    ktDM &requireDesign(const char *stage) const;
    /// Point the process-wide Visualizer at this session's settings, once per
    /// run: the first stage that draws does it, so the legalizer and the
    /// detailed placer add to the same GIF as the placer.
    void ensureVisualizer();

    /// Append to the stage log; `started` is when the stage began.
    void record(std::string stage, std::string detail, double started, bool stopped = false) const;

    std::unique_ptr<ktDM> db;
    PlotOptions plot;
    bool vizConfigured = false;
    std::string path_;
    std::size_t serial_ = 0;
    // Mutable because a check is a stage of the run worth recording, and the
    // check itself does not change the design.
    mutable std::vector<StageRecord> log_;
};

}  // namespace ktplace
