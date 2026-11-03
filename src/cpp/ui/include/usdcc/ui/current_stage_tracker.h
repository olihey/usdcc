#pragma once

#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"

#include <QObject>
#include <QPointer>

namespace ads {
class CDockManager;
class CDockWidget;
}  // namespace ads

namespace usdcc::ui {

class ViewPanel;

// Makes StageManager's current stage the stage of the view panel that last
// had focus (docs/CURRENT_STAGE.md §2): a panel gaining focus (ADS's
// focusedDockWidgetChanged) or the focused panel switching stages makes its
// stage current. Focus anywhere else, or on a panel with no stage, keeps the
// current stage. When the current stage is closed, the focused panel's stage
// (if it has another) takes over before StageManager falls back to the first
// open one.
class CurrentStageTracker : public QObject {
    Q_OBJECT

public:
    CurrentStageTracker(ads::CDockManager* dockManager, usdcc::usd::StageManager* stageManager,
                        QObject* parent = nullptr);

    ViewPanel* focusedPanel() const { return m_focused; }

private:
    void follow(ads::CDockWidget* focused);
    void makeCurrent(const usdcc::usd::StageRefPtr& stage);

    usdcc::usd::StageManager* m_stageManager;
    QPointer<ViewPanel> m_focused;
    QMetaObject::Connection m_focusedStageConnection;
    usdcc::core::Signal<usdcc::usd::StageRefPtr>::Connection m_stageClosedConnection;
};

}  // namespace usdcc::ui
