#pragma once

#include <QString>

class QMessageBox;
class QWidget;

namespace ktplace::gui {

void placeOver(QWidget &dialog, const QWidget &anchor);
QMessageBox *showNote(QWidget &parent, const QString &title, const QString &html);

}  // namespace ktplace::gui
