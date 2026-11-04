#include "usdcc/usd/tool_rail.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QAction>
#include <QActionGroup>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace usdcc::usd {

namespace {
constexpr int kIconSize = 16;

// For a tool that came without an icon. Twice the size, for high-DPI screens.
QPixmap letterIcon(const QString& text, const QColor& color) {
    QPixmap pixmap(kIconSize * 2, kIconSize * 2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(kIconSize * 3 / 2);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, text.left(1).toUpper());
    return pixmap;
}
}  // namespace

ToolRail::ToolRail(QWidget* parent) : QToolBar(parent), m_group(new QActionGroup(this)) {
    setObjectName(QStringLiteral("toolRail"));
    setWindowTitle(tr("Tools"));  // names it in the main window's toolbar context menu
    setOrientation(Qt::Vertical);  // initially; the main window flips it if the user docks it top/bottom
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    usdcc::ui::scaled::iconSize(this, kIconSize, kIconSize);
    m_group->setExclusive(true);
}

void ToolRail::addTool(const QString& id, const QString& toolTip, const QString& iconName) {
    QIcon icon;
    if (iconName.isEmpty()) {
        icon.addPixmap(letterIcon(toolTip, QColor(0xc8, 0xc8, 0xcc)), QIcon::Normal, QIcon::Off);
        icon.addPixmap(letterIcon(toolTip, Qt::white), QIcon::Normal, QIcon::On);
    } else if (iconName.contains(QLatin1Char('/')) || iconName.contains(QLatin1Char('\\'))) {
        icon.addFile(iconName);
    } else {
        // Off = idle glyph, On = the white glyph drawn on the checked accent fill.
        icon.addFile(QStringLiteral(":/usdcc/theme/%1.png").arg(iconName), QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(QStringLiteral(":/usdcc/theme/%1-active.png").arg(iconName), QSize(), QIcon::Normal, QIcon::On);
    }

    QAction* action = QToolBar::addAction(icon, toolTip);
    action->setToolTip(toolTip);
    action->setCheckable(true);
    action->setData(id);
    m_group->addAction(action);
    // triggered() (not toggled()) so only a user pick reports, not setCurrentTool().
    connect(action, &QAction::triggered, this, [this, id]() { Q_EMIT toolSelected(id); });
}

void ToolRail::clearTools() {
    const QList<QAction*> actions = m_group->actions();
    for (QAction* action : actions) {
        m_group->removeAction(action);
        removeAction(action);
        action->deleteLater();
    }
}

void ToolRail::setCurrentTool(const QString& id) {
    for (QAction* action : m_group->actions()) {
        if (action->data().toString() == id) {
            action->setChecked(true);
            return;
        }
    }
}

}  // namespace usdcc::usd
