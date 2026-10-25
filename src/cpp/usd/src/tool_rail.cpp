#include "usdcc/usd/tool_rail.h"

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
    setIconSize(QSize(kIconSize, kIconSize));
    m_group->setExclusive(true);
}

void ToolRail::addTool(int id, const QString& toolTip, const QString& iconName) {
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
    connect(action, &QAction::triggered, this, [this, id]() { emit toolSelected(id); });
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
