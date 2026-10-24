#include "gui/kt_dialogPlacement.h"

#include <QMessageBox>
#include <QWidget>

namespace ktplace::gui {

void placeOver(QWidget &dialog, const QWidget &anchor) {
    dialog.adjustSize();
    const QWidget &frame = *anchor.window();
    dialog.move(frame.frameGeometry().center() - dialog.rect().center());
}

QMessageBox *showNote(QWidget &parent, const QString &title, const QString &html) {
    auto *note =
        new QMessageBox(QMessageBox::Information, title, html, QMessageBox::Close, &parent);
    note->setAttribute(Qt::WA_DeleteOnClose);
    note->setModal(false);
    note->setTextFormat(Qt::RichText);
    placeOver(*note, parent);
    note->show();
    return note;
}

}  // namespace ktplace::gui
