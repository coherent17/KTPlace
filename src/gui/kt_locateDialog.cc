#include "gui/kt_locateDialog.h"

#include "gui/kt_namePattern.h"

#include <QAbstractListModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

constexpr int searchDelayMs = 200;

class MatchModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

    void reset(const std::vector<std::string> *source, std::vector<std::uint32_t> found) {
        beginResetModel();
        names = source;
        ids = std::move(found);
        endResetModel();
    }

    [[nodiscard]] int rowCount(const QModelIndex &parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(ids.size());
    }

    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override {
        if (role != Qt::DisplayRole || names == nullptr || !index.isValid()) {
            return {};
        }
        return QString::fromStdString((*names)[ids[static_cast<std::size_t>(index.row())]]);
    }

    [[nodiscard]] const std::vector<std::uint32_t> &found() const {
        return ids;
    }

private:
    const std::vector<std::string> *names = nullptr;
    std::vector<std::uint32_t> ids;
};

}  // namespace

class LocateDialog::Impl {
public:
    std::shared_ptr<const LayoutGeometry> geometry;
    QLineEdit *pattern = nullptr;
    QComboBox *target = nullptr;
    QLabel *summary = nullptr;
    QListView *results = nullptr;
    MatchModel *model = nullptr;
    QTimer delay;
    std::function<void(Target, std::vector<std::uint32_t>, bool)> listener;

    [[nodiscard]] Target currentTarget() const {
        return target->currentIndex() == 0 ? Target::Cells : Target::Nets;
    }

    void run() {
        if (!geometry || pattern->text().isEmpty()) {
            model->reset(nullptr, {});
            summary->setText(geometry ? QStringLiteral("Type a name or a pattern")
                                      : QStringLiteral("No design is shown"));
            return;
        }
        const std::vector<std::string> &names =
            currentTarget() == Target::Cells ? geometry->name : geometry->netName;
        model->reset(&names, matchNames(names, NamePattern(pattern->text().toStdString())));
        summary->setText(QStringLiteral("%1 of %2 %3")
                             .arg(model->found().size())
                             .arg(names.size())
                             .arg(currentTarget() == Target::Cells ? QStringLiteral("cells")
                                                                   : QStringLiteral("nets")));
    }

    void choose(std::vector<std::uint32_t> ids, bool zoom) const {
        if (listener && !ids.empty()) {
            listener(currentTarget(), std::move(ids), zoom);
        }
    }
};

LocateDialog::LocateDialog(QWidget *parent) : QDialog(parent), pImpl(std::make_unique<Impl>()) {
    setWindowTitle(QStringLiteral("Locate"));
    pImpl->pattern = new QLineEdit(this);
    pImpl->pattern->setPlaceholderText(QStringLiteral("Name or pattern: o12*, *clk*, n?7"));
    pImpl->pattern->setClearButtonEnabled(true);
    pImpl->target = new QComboBox(this);
    pImpl->target->addItems({QStringLiteral("Cells"), QStringLiteral("Nets")});
    pImpl->summary = new QLabel(this);
    pImpl->model = new MatchModel(this);
    pImpl->results = new QListView(this);
    pImpl->results->setModel(pImpl->model);
    pImpl->results->setUniformItemSizes(true);
    pImpl->results->setSelectionMode(QAbstractItemView::ExtendedSelection);

    pImpl->delay.setSingleShot(true);
    pImpl->delay.setInterval(searchDelayMs);
    connect(&pImpl->delay, &QTimer::timeout, this, [impl = pImpl.get()] {
        impl->run();
    });
    connect(pImpl->pattern, &QLineEdit::textEdited, &pImpl->delay, qOverload<>(&QTimer::start));
    connect(pImpl->pattern, &QLineEdit::returnPressed, this, [impl = pImpl.get()] {
        impl->delay.stop();
        impl->run();
    });
    connect(pImpl->target, &QComboBox::currentIndexChanged, this, [impl = pImpl.get()] {
        impl->run();
    });
    connect(pImpl->results, &QListView::doubleClicked, this,
            [impl = pImpl.get()](const QModelIndex &index) {
                impl->choose({impl->model->found()[static_cast<std::size_t>(index.row())]}, true);
            });

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *select =
        buttons->addButton(QStringLiteral("Select"), QDialogButtonBox::ActionRole);
    QPushButton *selectZoom =
        buttons->addButton(QStringLiteral("Select and Zoom"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    const auto chosenIds = [impl = pImpl.get()] {
        const QModelIndexList picked = impl->results->selectionModel()->selectedRows();
        if (picked.isEmpty()) {
            return impl->model->found();
        }
        std::vector<std::uint32_t> ids;
        ids.reserve(static_cast<std::size_t>(picked.size()));
        for (const QModelIndex &index : picked) {
            ids.push_back(impl->model->found()[static_cast<std::size_t>(index.row())]);
        }
        return ids;
    };
    connect(select, &QPushButton::clicked, this, [impl = pImpl.get(), chosenIds] {
        impl->choose(chosenIds(), false);
    });
    connect(selectZoom, &QPushButton::clicked, this, [impl = pImpl.get(), chosenIds] {
        impl->choose(chosenIds(), true);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    auto *query = new QHBoxLayout;
    query->addWidget(pImpl->pattern, 1);
    query->addWidget(pImpl->target);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(query);
    layout->addWidget(pImpl->summary);
    layout->addWidget(pImpl->results, 1);
    layout->addWidget(buttons);
    resize(420, 480);
    pImpl->run();
}

LocateDialog::~LocateDialog() = default;

void LocateDialog::setLayoutGeometry(std::shared_ptr<const LayoutGeometry> geometry) {
    if (geometry == pImpl->geometry) {
        return;
    }
    pImpl->geometry = std::move(geometry);
    pImpl->run();
}

void LocateDialog::search(const QString &pattern, Target target) {
    pImpl->pattern->setText(pattern);
    pImpl->target->setCurrentIndex(target == Target::Cells ? 0 : 1);
    pImpl->delay.stop();
    pImpl->run();
}

void LocateDialog::onChosen(
    std::function<void(Target, std::vector<std::uint32_t>, bool)> listener) {
    pImpl->listener = std::move(listener);
}

const std::vector<std::uint32_t> &LocateDialog::matches() const {
    return pImpl->model->found();
}

}  // namespace ktplace::gui
