// @file kt_runHistory.h
// The run history: every design the GUI has read, with the stages run on it
// and what each left the wirelength at, kept across sessions in a JSON file.
//
// A run is one read of a design and everything done to it until the next read
// (see ktSession::stageLog). The window asks the engine for the session's log
// after each script and hands it to update(), so the history follows typed
// commands, menu actions and sourced scripts alike.
//
// Results per design and algorithm come from here too: best() is the shortest
// final wirelength among the runs that got as far as detailed placement, which
// is what the Runs panel shows on each design's row.

#pragma once

#include <QDateTime>
#include <QString>
#include <cstddef>
#include <optional>
#include <vector>

namespace ktplace {

struct RunStage {
    QString stage;
    QString detail;
    double hpwl = 0.0;
    double seconds = 0.0;
    bool stopped = false;
};

struct RunRecord {
    QString id;
    QDateTime started;
    /// The design directory, as it was given to read_design.
    QString design;
    QString label;
    std::vector<RunStage> stages;

    /// The algorithm of the last global placement; empty if none ran.
    [[nodiscard]] QString algorithm() const;
    /// The wirelength after the last global placement.
    [[nodiscard]] std::optional<double> globalHpwl() const;
    /// The wirelength after the last stage, once anything beyond the read ran.
    [[nodiscard]] std::optional<double> finalHpwl() const;
    /// The last placement check's verdict ("legal", "3 defects"); empty if
    /// the placement was never checked.
    [[nodiscard]] QString verdict() const;
    /// Whether the run got as far as detailed placement.
    [[nodiscard]] bool complete() const;
    /// Whether a stage of it was stopped early.
    [[nodiscard]] bool stopped() const;
    /// Time spent in the stages, the read included.
    [[nodiscard]] double seconds() const;
};

class RunHistory {
public:
    /// Kept in @p file; empty keeps it in memory only.
    explicit RunHistory(QString file = {});

    [[nodiscard]] const QString &file() const {
        return file_;
    }

    /// Read the file, replacing what is held. A missing file is an empty
    /// history; returns false only when the file exists and cannot be read.
    bool load();
    /// Write the file, if there is one. Returns false when writing failed.
    bool save() const;

    /// Record the stages of this process's @p serial-th read (of @p design),
    /// adding the run on first sight. Returns whether anything changed.
    bool update(std::size_t serial, const QString &design, std::vector<RunStage> stages);

    /// Oldest first.
    [[nodiscard]] const std::vector<RunRecord> &runs() const {
        return runs_;
    }
    [[nodiscard]] const RunRecord *find(const QString &id) const;
    bool rename(const QString &id, const QString &label);
    bool remove(const QString &id);
    void clear();

    struct Best {
        double hpwl = 0.0;
        QString runId;
        QString verdict;
    };
    /// The shortest final wirelength of the complete runs of @p design with
    /// @p algorithm. Designs compare by their cleaned-up absolute path.
    [[nodiscard]] std::optional<Best> best(const QString &design, const QString &algorithm) const;

    /// The history as a tab-separated table, one row per run.
    [[nodiscard]] QString toTsv() const;

    /// Runs beyond this many are dropped, oldest first.
    static constexpr std::size_t kMaxRuns = 1000;

private:
    QString file_;
    /// Distinguishes this process's runs from earlier sessions' in the ids.
    QString session_;
    std::vector<RunRecord> runs_;
};

}  // namespace ktplace
