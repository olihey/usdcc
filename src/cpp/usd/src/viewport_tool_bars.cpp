#include "usdcc/usd/viewport_tool_bars.h"

#include "usdcc/usd/tool_options_bar.h"
#include "usdcc/usd/tool_rail.h"

#include <QEvent>
#include <QMainWindow>
#include <QStyle>
#include <QToolBar>

namespace usdcc::usd {

using usdcc::tools::ToolKind;

ViewportToolBars::ViewportToolBars(QMainWindow* window, std::shared_ptr<usdcc::tools::ToolSet> toolSet)
    : QObject(window), m_window(window), m_toolSet(std::move(toolSet)) {
    m_rail = new ToolRail(window);
    m_rail->addTool(static_cast<int>(ToolKind::Select), tr("Select"), QStringLiteral("tool-select"));
    m_rail->addTool(static_cast<int>(ToolKind::Move), tr("Move"), QStringLiteral("tool-move"));
    m_rail->addTool(static_cast<int>(ToolKind::Rotate), tr("Rotate"), QStringLiteral("tool-rotate"));
    m_rail->addTool(static_cast<int>(ToolKind::Scale), tr("Scale"), QStringLiteral("tool-scale"));
    window->addToolBar(Qt::LeftToolBarArea, m_rail);

    // The options bar is a plain widget; a QToolBar is what puts it in the main
    // window's layout.
    m_optionsBar = new ToolOptionsBar();
    auto* optionsToolBar = new QToolBar(tr("Tool Options"), window);
    optionsToolBar->setObjectName(QStringLiteral("toolOptionsToolBar"));
    // A horizontal strip of controls only makes sense along the top or bottom.
    optionsToolBar->setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);
    optionsToolBar->addWidget(m_optionsBar);
    window->addToolBar(Qt::TopToolBarArea, optionsToolBar);

    // Both bars are draggable and floatable (QToolBar's defaults). Track which
    // edge each one is docked to, so the stylesheet can draw its divider on the
    // side facing the content (see dark.qss, QToolBar[area=...]).
    for (QToolBar* bar : {static_cast<QToolBar*>(m_rail), optionsToolBar}) {
        bar->installEventFilter(this);
        updateArea(bar);
    }

    connect(m_rail, &ToolRail::toolSelected, this,
            [this](int id) { m_toolSet->setActiveKind(static_cast<ToolKind>(id)); });
    connect(m_optionsBar, &ToolOptionsBar::optionChanged, this,
            [this](const std::string& id, const usdcc::tools::OptionValue& value) {
                m_toolSet->setActiveOption(id, value);
            });

    // Not only for our own clicks: if anything else switches the active tool,
    // the rail and options bar follow.
    m_activeToolConnection = m_toolSet->activeToolChanged.connect([this]() { syncToActiveTool(); });
    syncToActiveTool();
}

bool ViewportToolBars::eventFilter(QObject* watched, QEvent* event) {
    if (auto* bar = qobject_cast<QToolBar*>(watched)) {
        switch (event->type()) {
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::Show:
            case QEvent::ParentChange:
                updateArea(bar);
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void ViewportToolBars::updateArea(QToolBar* bar) {
    const char* area = "floating";
    switch (m_window->toolBarArea(bar)) {
        case Qt::LeftToolBarArea:
            area = "left";
            break;
        case Qt::RightToolBarArea:
            area = "right";
            break;
        case Qt::TopToolBarArea:
            area = "top";
            break;
        case Qt::BottomToolBarArea:
            area = "bottom";
            break;
        default:
            break;
    }
    if (bar->property("area").toString() == QLatin1String(area)) {
        return;
    }
    bar->setProperty("area", QLatin1String(area));
    bar->style()->unpolish(bar);  // re-evaluate the stylesheet's [area="..."] rules
    bar->style()->polish(bar);
    bar->update();
}

void ViewportToolBars::syncToActiveTool() {
    m_rail->setCurrentTool(static_cast<int>(m_toolSet->activeKind()));
    const usdcc::tools::Tool& tool = m_toolSet->activeTool();
    m_optionsBar->setTool(QString::fromStdString(tool.name()), tool.options());
}

}  // namespace usdcc::usd
