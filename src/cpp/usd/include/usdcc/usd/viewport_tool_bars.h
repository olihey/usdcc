#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"

#include <QObject>

#include <memory>

class QMainWindow;
class QToolBar;

namespace usdcc::usd {

class ToolOptionsBar;
class ToolRail;

// The two toolbars that drive the editing tools, installed in the *main
// window's* layout rather than inside any viewport (see docs/PLAN.md §6): a
// vertical tool rail down the left edge and a tool-options bar under the menu
// bar. They control a ToolSet shared by every viewport, so they always act on
// all of them — there is no "current viewport" they follow.
//
// Both toolbars can be dragged to any edge the main window allows (the options
// bar: top or bottom only), or floated. Parented to the window, which also owns
// the toolbars; just construct it.
class ViewportToolBars : public QObject {
    Q_OBJECT

public:
    ViewportToolBars(QMainWindow* window, std::shared_ptr<usdcc::tools::ToolSet> toolSet);

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

    QMainWindow* m_window;
    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    ToolRail* m_rail;
    ToolOptionsBar* m_optionsBar;
    usdcc::core::Signal<>::Connection m_activeToolConnection;
};

}  // namespace usdcc::usd
