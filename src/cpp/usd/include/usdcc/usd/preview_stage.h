#pragma once

#include "usdcc/usd/stage.h"

#include <QApplication>
#include <QDialog>
#include <QString>

#include <functional>

class QObject;
class QWidget;

namespace usdcc::usd {

// What the preview dialogs (Show USDA, docs/USDA_VIEW.md §10, and Show Prims,
// docs/SHOW_PRIMS.md) share.

// Whether a preview can show `path`: an open layer, or a file USD has a format
// for (not a texture). A missing file is for its caller to check.
bool canPreviewUsda(const QString& path);

// Opens `path` on a worker thread, as a stage of its own that StageManager
// never hears of, without payloads. `done` runs on this thread with the stage,
// or null and why not, unless `context` is gone by then: closing a preview
// while it loads just drops the result, as USD can't stop an open midway.
using PreviewStageLoaded = std::function<void(const StageRefPtr& stage, const QString& error)>;
void loadPreviewStage(const QString& path, QObject* context, PreviewStageLoaded done);

// A preview's window: deleted on close, with minimize and maximize buttons,
// and known to previewOwner().
void setUpPreviewWindow(QDialog* dialog);
// What a new preview opened from `parent` goes over: its main window, even
// from another preview, so closing that one doesn't take this one with it.
QWidget* previewOwner(QWidget* parent);

// One spelling per file, to tell whether a dialog already shows it.
QString previewFileKey(const QString& path);
// The visible `Dialog` (with a path()) showing `path`, brought forward; null if
// there's none.
template <typename Dialog>
Dialog* raiseOpenPreview(const QString& path) {
    const QString key = previewFileKey(path);
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<Dialog*>(widget);
        if (dialog && dialog->isVisible() && previewFileKey(dialog->path()) == key) {
            dialog->raise();
            dialog->activateWindow();
            return dialog;
        }
    }
    return nullptr;
}

}  // namespace usdcc::usd
