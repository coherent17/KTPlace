// @file kt_runHistory.cc
// Implementation of RunHistory.

#include "gui/kt_runHistory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <algorithm>
#include <utility>

namespace ktplace {

namespace {

QString canonical(const QString &dir) {
    return QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
}

QJsonObject toJson(const RunRecord &r) {
    QJsonArray stages;
    for (const RunStage &s : r.stages) {
        stages.append(QJsonObject{{"stage", s.stage},
                                  {"detail", s.detail},
                                  {"hpwl", s.hpwl},
                                  {"seconds", s.seconds},
                                  {"stopped", s.stopped}});
    }
    return QJsonObject{{"id", r.id},
                       {"started", r.started.toString(Qt::ISODate)},
                       {"design", r.design},
                       {"label", r.label},
                       {"stages", stages}};
}

RunRecord fromJson(const QJsonObject &o) {
    RunRecord r;
    r.id = o.value("id").toString();
    r.started = QDateTime::fromString(o.value("started").toString(), Qt::ISODate);
    r.design = o.value("design").toString();
    r.label = o.value("label").toString();
    for (const QJsonValue &v : o.value("stages").toArray()) {
        const QJsonObject s = v.toObject();
        r.stages.push_back({s.value("stage").toString(), s.value("detail").toString(),
                            s.value("hpwl").toDouble(), s.value("seconds").toDouble(),
                            s.value("stopped").toBool()});
    }
    return r;
}

const RunStage *lastOf(const RunRecord &r, const QString &stage) {
    for (auto it = r.stages.rbegin(); it != r.stages.rend(); ++it) {
        if (it->stage == stage) {
            return &*it;
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- RunRecord

QString RunRecord::algorithm() const {
    const RunStage *s = lastOf(*this, QStringLiteral("global_place"));
    return s ? s->detail : QString();
}

std::optional<double> RunRecord::globalHpwl() const {
    const RunStage *s = lastOf(*this, QStringLiteral("global_place"));
    return s ? std::optional<double>(s->hpwl) : std::nullopt;
}

std::optional<double> RunRecord::finalHpwl() const {
    if (stages.size() < 2) {
        return std::nullopt;
    }
    return stages.back().hpwl;
}

QString RunRecord::verdict() const {
    const RunStage *s = lastOf(*this, QStringLiteral("check_placement"));
    return s ? s->detail : QString();
}

bool RunRecord::complete() const {
    return lastOf(*this, QStringLiteral("detail_place")) != nullptr;
}

bool RunRecord::stopped() const {
    return std::ranges::any_of(stages, [](const RunStage &s) {
        return s.stopped;
    });
}

double RunRecord::seconds() const {
    double total = 0.0;
    for (const RunStage &s : stages) {
        total += s.seconds;
    }
    return total;
}

// ---------------------------------------------------------------- RunHistory

RunHistory::RunHistory(QString file) : file_(std::move(file)) {
    session_ =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + '-' +
        QString::number(QRandomGenerator::global()->bounded(0x10000), 16).rightJustified(4, '0');
}

bool RunHistory::load() {
    runs_.clear();
    if (file_.isEmpty() || !QFileInfo::exists(file_)) {
        return true;
    }
    QFile in(file_);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    if (!doc.isObject()) {
        return false;
    }
    for (const QJsonValue &v : doc.object().value("runs").toArray()) {
        RunRecord r = fromJson(v.toObject());
        if (!r.id.isEmpty()) {
            runs_.push_back(std::move(r));
        }
    }
    return true;
}

bool RunHistory::save() const {
    if (file_.isEmpty()) {
        return true;
    }
    QJsonArray runs;
    for (const RunRecord &r : runs_) {
        runs.append(toJson(r));
    }
    // Written aside and renamed over, so a crash never leaves half a history.
    QSaveFile out(file_);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    out.write(QJsonDocument(QJsonObject{{"version", 1}, {"runs", runs}}).toJson());
    return out.commit();
}

bool RunHistory::update(std::size_t serial, const QString &design, std::vector<RunStage> stages) {
    const QString id = session_ + '-' + QString::number(serial);
    auto it = std::ranges::find_if(runs_, [&](const RunRecord &r) {
        return r.id == id;
    });
    if (it == runs_.end()) {
        RunRecord r;
        r.id = id;
        r.started = QDateTime::currentDateTime();
        r.design = design;
        r.stages = std::move(stages);
        runs_.push_back(std::move(r));
        if (runs_.size() > kMaxRuns) {
            runs_.erase(runs_.begin(),
                        runs_.begin() + static_cast<std::ptrdiff_t>(runs_.size() - kMaxRuns));
        }
        return true;
    }
    const auto same = [](const RunStage &a, const RunStage &b) {
        return a.stage == b.stage && a.detail == b.detail && a.hpwl == b.hpwl &&
               a.seconds == b.seconds && a.stopped == b.stopped;
    };
    if (std::ranges::equal(it->stages, stages, same)) {
        return false;
    }
    it->stages = std::move(stages);
    return true;
}

const RunRecord *RunHistory::find(const QString &id) const {
    const auto it = std::ranges::find_if(runs_, [&](const RunRecord &r) {
        return r.id == id;
    });
    return it == runs_.end() ? nullptr : &*it;
}

bool RunHistory::rename(const QString &id, const QString &label) {
    const auto it = std::ranges::find_if(runs_, [&](const RunRecord &r) {
        return r.id == id;
    });
    if (it == runs_.end() || it->label == label) {
        return false;
    }
    it->label = label;
    return true;
}

bool RunHistory::remove(const QString &id) {
    return std::erase_if(runs_, [&](const RunRecord &r) {
               return r.id == id;
           }) > 0;
}

void RunHistory::clear() {
    runs_.clear();
}

std::optional<RunHistory::Best> RunHistory::best(const QString &design,
                                                 const QString &algorithm) const {
    const QString want = canonical(design);
    std::optional<Best> best;
    for (const RunRecord &r : runs_) {
        if (!r.complete() || r.algorithm() != algorithm || canonical(r.design) != want) {
            continue;
        }
        const double h = *r.finalHpwl();
        if (!best || h < best->hpwl) {
            best = Best{h, r.id, r.verdict()};
        }
    }
    return best;
}

QString RunHistory::toTsv() const {
    QString out = QStringLiteral(
        "id\tstarted\tdesign\tlabel\talgorithm\tglobal_hpwl\tfinal_hpwl\tverdict\tstopped\t"
        "seconds\n");
    const auto num = [](const std::optional<double> &v) {
        return v ? QString::number(*v, 'g', 6) : QString();
    };
    for (const RunRecord &r : runs_) {
        out += QStringList{r.id,
                           r.started.toString(Qt::ISODate),
                           r.design,
                           r.label,
                           r.algorithm(),
                           num(r.globalHpwl()),
                           num(r.finalHpwl()),
                           r.verdict(),
                           r.stopped() ? QStringLiteral("yes") : QString(),
                           QString::number(r.seconds(), 'f', 2)}
                   .join('\t') +
               '\n';
    }
    return out;
}

}  // namespace ktplace
