#include "usdcc/usd/viewport_tool_bars.h"

#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/tool_options_bar.h"
#include "usdcc/usd/tool_rail.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/usd/usd/stage.h>

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStyle>
#include <QToolBar>

namespace usdcc::usd {

using usdcc::tools::ToolKind;

ViewportToolBars::ViewportToolBars(QMainWindow* window, std::shared_ptr<usdcc::tools::ToolSet> toolSet,
                                   StageManager* stageManager)
    : QObject(window), m_window(window), m_toolSet(std::move(toolSet)), m_stageManager(stageManager) {
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

    // Target: which transform op the tools act on.
    m_targetBox = new QWidget();
    m_targetBox->setObjectName(QStringLiteral("toolOptionsBar"));
    m_targetBox->setAttribute(Qt::WA_StyledBackground);
    m_targetBox->setFixedHeight(30);
    auto* targetLayout = new QHBoxLayout(m_targetBox);
    targetLayout->setContentsMargins(10, 0, 10, 0);
    targetLayout->setSpacing(6);
    m_targetHint = new QLabel(m_targetBox);
    m_targetHint->setObjectName(QStringLiteral("targetHint"));
    m_targetCombo = new QComboBox(m_targetBox);
    m_targetCombo->setMinimumWidth(160);
    m_targetCombo->setToolTip(tr("The transform op the tool acts on. Auto: the first op of the tool's own type."));
    targetLayout->addWidget(m_targetHint);
    targetLayout->addWidget(new QLabel(tr("Target"), m_targetBox));
    targetLayout->addWidget(m_targetCombo);
    // A widget in a QToolBar is shown and hidden through its action.
    m_targetAction = optionsToolBar->addWidget(m_targetBox);
    window->addToolBar(Qt::TopToolBarArea, optionsToolBar);

    connect(m_targetCombo, &QComboBox::activated, this, [this](int index) {
        const StageRefPtr stage = m_stageManager->currentStage();
        const std::vector<PXR_NS::SdfPath> selection = m_stageManager->selectedPaths(stage);
        if (!stage || selection.empty()) return;
        const PXR_NS::TfToken op(m_targetCombo->itemData(index).toString().toStdString());
        stage->setTargetXformOp(selection.front(), op);
    });
    m_currentStageConnection =
        m_stageManager->currentStageChanged.connect([this](const StageRefPtr& stage) { watchStage(stage); });
    m_selectionConnection = m_stageManager->selectionChanged.connect(
        [this](const StageRefPtr& stage, std::vector<PXR_NS::SdfPath>) {
            if (stage == m_watchedStage) updateTarget();
        });

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
    watchStage(m_stageManager->currentStage());
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
    updateTarget();
}

void ViewportToolBars::watchStage(const StageRefPtr& stage) {
    m_watchedStage = stage;
    m_targetConnection = {};
    m_stageChangedConnection = {};
    if (stage) {
        m_targetConnection = stage->targetXformOpChanged.connect([this]() { onTargetChanged(); });
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges& changes) {
            const std::vector<PXR_NS::SdfPath> selection = m_stageManager->selectedPaths(m_watchedStage);
            if (!selection.empty() && changes.resyncsPrim(selection.front())) updateTarget();  // ops added/removed
        });
    }
    updateTarget();
}

void ViewportToolBars::onTargetChanged() {
    // Picking an op the active tool can't drive (e.g. a scale op in the
    // transform stack while Move is active) switches to the tool that can.
    const StageRefPtr stage = m_watchedStage;
    if (stage && !stage->targetXformOp().IsEmpty()) {
        const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(stage->targetPrim());
        for (const PXR_NS::UsdGeomXformOp& op : XformStack(prim).ops()) {
            if (op.GetOpName() != stage->targetXformOp()) continue;
            const ToolKind kind = usdcc::tools::toolKindForOpType(op.GetOpType());
            if (kind != ToolKind::Select && !usdcc::tools::toolCanDrive(m_toolSet->activeKind(), op.GetOpType())) {
                m_toolSet->setActiveKind(kind);  // refreshes the target too, via syncToActiveTool()
                return;
            }
            break;
        }
    }
    updateTarget();
}

void ViewportToolBars::updateTarget() {
    if (!m_targetAction) return;  // during construction
    const ToolKind kind = m_toolSet->activeKind();
    const StageRefPtr stage = m_watchedStage;
    const std::vector<PXR_NS::SdfPath> selection =
        stage ? m_stageManager->selectedPaths(stage) : std::vector<PXR_NS::SdfPath>{};
    const PXR_NS::UsdPrim prim =
        stage && selection.size() == 1 ? stage->usdStage()->GetPrimAtPath(selection.front()) : PXR_NS::UsdPrim();
    const XformStack stack(prim);
    if (kind == ToolKind::Select || !stack.isValid()) {
        m_targetAction->setVisible(false);
        return;
    }
    m_targetAction->setVisible(true);

    const PXR_NS::TfToken target = stage->targetXformOpFor(prim.GetPath());
    m_targetCombo->clear();
    m_targetCombo->addItem(tr("Auto"), QString());
    int current = 0;
    QString hint;
    for (const PXR_NS::UsdGeomXformOp& op : stack.ops()) {
        if (op.IsInverseOp()) continue;
        const QString label = QString::fromStdString(XformStack::opLabel(op));
        const QString name = QString::fromStdString(op.GetOpName().GetString());
        if (usdcc::tools::toolCanDrive(kind, op.GetOpType())) {
            m_targetCombo->addItem(label, name);
            if (op.GetOpName() == target) current = m_targetCombo->count() - 1;
        } else if (op.GetOpName() == target) {
            // An incompatible target stays visible, and explains the grey gizmo.
            m_targetCombo->addItem(tr("%1 (not editable with this tool)").arg(label), name);
            current = m_targetCombo->count() - 1;
            const QString needs = kind == ToolKind::Move     ? tr("a Translate op")
                                  : kind == ToolKind::Rotate ? tr("a rotate op")
                                                             : tr("a Scale op");
            hint = tr("Target is %1; choose Auto or %2.").arg(label, needs);
        }
    }
    m_targetCombo->setCurrentIndex(current);
    m_targetHint->setText(hint);
    m_targetHint->setVisible(!hint.isEmpty());
}

}  // namespace usdcc::usd
