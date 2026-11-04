#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage.h"

#include <QObject>

#include <memory>
#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QMainWindow;
class QToolBar;
class QWidget;

namespace usdcc::usd {

class StageManager;
class ToolOptionsBar;
class ToolRail;

// The Shortcut settings (usdcc::core::Settings) that pick each tool; the
// constructor below defines them (Q, W, E, R by default).
namespace toolShortcutKeys {
inline constexpr auto select = "Shortcuts/tools.select";
inline constexpr auto move = "Shortcuts/tools.move";
inline constexpr auto rotate = "Shortcuts/tools.rotate";
inline constexpr auto scale = "Shortcuts/tools.scale";
}  // namespace toolShortcutKeys

// The two toolbars that drive the editing tools, installed in the *main
// window's* layout rather than inside any viewport (see docs/PLAN.md §6): a
// vertical tool rail down the left edge and a tool-options bar under the menu
// bar. They control a ToolSet shared by every viewport, so they always act on
// all of them — there is no "current viewport" they follow.
//
// The options bar also carries the transform tools' **Target**
// (docs/ATTRIBUTE_EDITOR.md §7.1): Auto, or one op of the selected prim that
// the active tool can drive, kept in sync with the attribute editor's
// transform stack (both set the current stage's Stage::targetXformOp()). When
// the target changes to an op the active tool can't drive, the bar switches
// to the tool that can; if the user then picks an incompatible tool by hand, a
// hint explains why its gizmo is greyed out.
//
// Both toolbars can be dragged to any edge the main window allows (the options
// bar: top or bottom only), or floated. Parented to the window, which also owns
// the toolbars; just construct it.
class ViewportToolBars : public QObject {
    Q_OBJECT

public:
    ViewportToolBars(QMainWindow* window, std::shared_ptr<usdcc::tools::ToolSet> toolSet, StageManager* stageManager);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Shows the active tool's name and options in the options bar, and checks
    // its button on the rail.
    void syncToActiveTool();

    // Records which edge `bar` is docked to (or "floating") as its "area"
    // property, which the stylesheet uses to put the divider line on the side
    // facing the content.
    void updateArea(QToolBar* bar);

    // Target: follow the current stage, its selection and its target.
    void watchStage(const StageRefPtr& stage);
    void updateTarget();
    void onTargetChanged();

    QMainWindow* m_window;
    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    StageManager* m_stageManager;
    ToolRail* m_rail;
    ToolOptionsBar* m_optionsBar;
    QWidget* m_targetBox = nullptr;
    QAction* m_targetAction = nullptr;  // the box's slot in the toolbar, which owns its visibility
    QComboBox* m_targetCombo = nullptr;
    QLabel* m_targetHint = nullptr;
    StageRefPtr m_watchedStage;
    usdcc::core::Signal<>::Connection m_activeToolConnection;
    usdcc::core::Signal<StageRefPtr>::Connection m_currentStageConnection;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    usdcc::core::Signal<>::Connection m_targetConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

}  // namespace usdcc::usd
