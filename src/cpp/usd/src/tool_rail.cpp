#include "usdcc/usd/tool_rail.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scaled_metrics.h"

#include <QAction>
#include <QActionGroup>
#include <QIcon>

namespace usdcc::usd {

namespace {
constexpr int kIconSize = 16;
}  // namespace

ToolRail::ToolRail(QWidget* parent) : QToolBar(parent), m_group(new QActionGroup(this)) {
    setObjectName(QStringLiteral("toolRail"));
    setWindowTitle(tr("Tools"));  // names it in the main window's toolbar context menu
    setOrientation(Qt::Vertical);  // initially; the main window flips it if the user docks it top/bottom
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    usdcc::ui::scaled::iconSize(this, kIconSize, kIconSize);
    m_group->setExclusive(true);
}

void ToolRail::addTool(int id, const QString& toolTip, const QString& iconName, const std::string& shortcutKey) {
    // Off = idle glyph, On = the white glyph drawn on the checked accent fill.
    QIcon icon;
    icon.addFile(QStringLiteral(":/usdcc/theme/%1.png").arg(iconName), QSize(), QIcon::Normal, QIcon::Off);
    icon.addFile(QStringLiteral(":/usdcc/theme/%1-active.png").arg(iconName), QSize(), QIcon::Normal, QIcon::On);

    QAction* action = QToolBar::addAction(icon, toolTip);
    action->setToolTip(toolTip);
    action->setCheckable(true);
    action->setData(id);
    m_group->addAction(action);
    // triggered() (not toggled()) so only a user pick reports, not setCurrentTool().
    connect(action, &QAction::triggered, this, [this, id]() { Q_EMIT toolSelected(id); });

    if (shortcutKey.empty()) return;
    // Application-wide, so the key also works in floating panels (a floated
    // viewport is a window of its own). Text fields still get letters typed
    // into them: they claim those keys first (ShortcutOverride).
    action->setShortcutContext(Qt::ApplicationShortcut);
    usdcc::ui::bindShortcut(action, shortcutKey);
    const auto updateToolTip = [action, toolTip, shortcutKey]() {
        const QKeySequence key = usdcc::ui::settingShortcut(shortcutKey);
        action->setToolTip(key.isEmpty() ? toolTip
                                         : QStringLiteral("%1 (%2)").arg(toolTip, key.toString(QKeySequence::NativeText)));
    };
    usdcc::ui::onSettingChanged(action, shortcutKey, updateToolTip);
    updateToolTip();
}

void ToolRail::setCurrentTool(int id) {
    for (QAction* action : m_group->actions()) {
        if (action->data().toInt() == id) {
            action->setChecked(true);
            return;
        }
    }
}

}  // namespace usdcc::usd
