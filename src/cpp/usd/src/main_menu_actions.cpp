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

// Marks a menu made for an entry, which goes when its last entry does.
constexpr auto kMadeForEntries = "usdccMadeForEntries";

// The menu-bar entry a new top-level menu goes in before: the one after Create
// and the menus made for entries that follow it, so plugin menus sit after
// Create in the order they came. With no Create, before Help or else Debug;
// null (the end) with neither.
QAction* newMenuSlot(const QList<QAction*>& bar) {
    const auto named = [&bar](QLatin1String name) -> qsizetype {
        for (qsizetype i = 0; i < bar.size(); ++i) {
            if (withoutMnemonic(bar[i]->text()) == name) return i;
        }
        return -1;
    };
    qsizetype create = named(QLatin1String("Create"));
    if (create >= 0) {
        qsizetype i = create + 1;
        while (i < bar.size() && bar[i]->menu() && bar[i]->menu()->property(kMadeForEntries).toBool()) ++i;
        return bar.value(i);
    }
    for (QLatin1String name : {QLatin1String("Help"), QLatin1String("Debug")}) {
        if (const qsizetype i = named(name); i >= 0) return bar[i];
    }
    return nullptr;
}

QMenu* menuAt(usdcc::ui::MainWindow& window, const QStringList& parts, bool create = true) {
    QMenuBar* bar = window.mainMenuBar();
    QMenu* current = nullptr;
    for (const QString& part : parts) {
        const QList<QAction*> actions = current ? current->actions() : bar->actions();
        QMenu* found = nullptr;
        for (QAction* action : actions) {
            if (action->menu() && withoutMnemonic(action->text()) == withoutMnemonic(part)) found = action->menu();
        }
        if (!found) {
            if (!create) return nullptr;
            if (current) {
                found = current->addMenu(part);
            } else {
                found = new QMenu(part, bar);
                bar->insertMenu(newMenuSlot(actions), found);
            }
            found->setProperty(kMadeForEntries, true);
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
    QMenu* menu = menuAt(window, parts, false);
    QAction* action = menu ? entryIn(menu, label) : nullptr;
    if (!action) return false;
    menu->removeAction(action);
    action->deleteLater();
    // Up through the menus made for it that have nothing left. The app's own stay.
    while (menu && menu->actions().isEmpty() && menu->property(kMadeForEntries).toBool()) {
        QMenu* parent = qobject_cast<QMenu*>(menu->parentWidget());
        if (QWidget* owner = menu->parentWidget()) owner->removeAction(menu->menuAction());
        menu->deleteLater();
        menu = parent;
    }
    return true;
}

}  // namespace usdcc::usd
