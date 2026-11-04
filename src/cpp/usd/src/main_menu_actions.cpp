#include "usdcc/usd/main_menu_actions.h"

#include "usdcc/ui/main_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStringList>

namespace usdcc::usd {

namespace {
QString withoutMnemonic(QString text) { return text.remove(QLatin1Char('&')); }

QMenu* menuAt(usdcc::ui::MainWindow& window, const QStringList& parts) {
    QMenuBar* bar = window.mainMenuBar();
    QMenu* current = nullptr;
    for (const QString& part : parts) {
        const QList<QAction*> actions = current ? current->actions() : bar->actions();
        QMenu* found = nullptr;
        QAction* debug = nullptr;
        for (QAction* action : actions) {
            if (action->menu() && withoutMnemonic(action->text()) == withoutMnemonic(part)) found = action->menu();
            if (!current && withoutMnemonic(action->text()) == QLatin1String("Debug")) debug = action;
        }
        if (!found) {
            if (current) {
                found = current->addMenu(part);
            } else {
                found = new QMenu(part, bar);
                bar->insertMenu(debug, found);
            }
        }
        current = found;
    }
    return current;
}

QAction* entryIn(QMenu* menu, const QString& label) {
    for (QAction* action : menu->actions()) {
        if (!action->menu() && withoutMnemonic(action->text()) == withoutMnemonic(label)) return action;
    }
    return nullptr;
}
}  // namespace

void runAsOneUndoStep(UndoManager* undo, StageManager* stages, const std::string& label,
                      const std::function<bool()>& run) {
    // A global edit doesn't nest.
    const bool ownsEdit = undo && stages && !undo->isGlobalEditing();
    if (ownsEdit) undo->beginGlobalEdit(*stages, QString::fromStdString(label));
    const bool ok = run();
    if (ownsEdit) undo->endGlobalEdit(ok ? QString() : QStringLiteral("%1 (failed)").arg(QString::fromStdString(label)));
}

bool addMainMenuAction(usdcc::ui::MainWindow& window, const QString& path, std::function<void()> callback,
                       const QString& shortcut, const QString& tip) {
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2) return false;
    const QString label = parts.takeLast();
    QMenu* menu = menuAt(window, parts);
    QAction* action = entryIn(menu, label);
    if (action) {
        QObject::disconnect(action, &QAction::triggered, nullptr, nullptr);  // registered again
    } else {
        action = menu->addAction(label);
    }
    action->setShortcut(QKeySequence(shortcut));
    action->setStatusTip(tip);
    action->setToolTip(tip);
    QObject::connect(action, &QAction::triggered, &window, [callback = std::move(callback)]() { callback(); });
    return true;
}

bool removeMainMenuAction(usdcc::ui::MainWindow& window, const QString& path) {
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2) return false;
    const QString label = parts.takeLast();
    QMenu* menu = menuAt(window, parts);
    QAction* action = entryIn(menu, label);
    if (!action) return false;
    menu->removeAction(action);
    action->deleteLater();
    return true;
}

}  // namespace usdcc::usd
