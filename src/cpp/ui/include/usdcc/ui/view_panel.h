#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <DockWidget.h>

class QComboBox;
class QMenu;

namespace usdcc::usd {
class StageManager;
}

namespace usdcc::ui {

// Base class for panels living inside the Qt Advanced Docking System
// container (ViewportViewPanel, OutlinerViewPanel, AttributesViewPanel —
// see docs/PLAN.md section 4). Every ViewPanel always has an associated
// usdcc::usd::Stage (never null — see the constructor) and a "Stage:"
// dropdown to switch which one it shows, both owned here rather than
// duplicated per-subclass — see docs/PLAN.md §6 "ViewPanel: mandatory Stage
// association" for the full design and its acknowledged consequences
// (usdcc_ui no longer builds without USD_INSTALL; ViewPanel is no longer
// exposed to Python).
//
// Uses ADS's deprecated (title, parent) CDockWidget constructor rather than
// the (manager, title, parent) one: the manager association still happens
// when the panel is added via CDockManager::addDockWidget(), and avoiding
// ads::CDockManager* in the public constructor keeps ViewPanel constructible
// from Python without generating Shiboken6 bindings for all of ADS (that
// base class is intentionally not exposed to Python — see setContentWidget
// below for why that's fine in practice).
//
// setContentWidget()/contentWidget() re-expose CDockWidget::setWidget()/
// widget() in terms of QWidget alone, which IS known to Shiboken6 (it's part
// of Qt's own typesystem), so Python code gets a working content API without
// needing bindings for ads::CDockWidget itself.
class ViewPanel : public ads::CDockWidget {
    Q_OBJECT

public:
    // `stageManager` and `stage` must both be non-null — a ViewPanel always
    // shows some real stage. If `stage` is later closed via
    // StageManager::closeStage(), this panel closes itself (see the .cpp).
    explicit ViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage, const QString& title,
                       QWidget* parent = nullptr);

    usdcc::usd::StageManager* stageManager() const { return m_stageManager; }
    const usdcc::usd::StageRefPtr& currentStage() const { return m_stage; }

    // Switches which stage this panel shows. No-op if `stage` is null (the
    // invariant forbids ever actually holding a null stage) or already the
    // current one. Calls onStageChanged() so subclasses can refresh their
    // displayed content.
    void setStage(usdcc::usd::StageRefPtr stage);

    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const;

    // Creates a copy of this panel, showing the same stage. The base
    // implementation just constructs a plain ViewPanel with the same title
    // and stage — it has no way to generically duplicate an arbitrary
    // content widget, and no way to invoke a subclass's constructor (which
    // typically needs extra arguments). Subclasses that carry their own
    // state should override this: construct their own type (passing
    // currentStage(), not stageManager()->currentStage() — a sibling panel
    // may be showing a different stage than whatever is globally current),
    // then copy whatever values make the duplicate meaningfully match the
    // original (e.g. the camera position, selection).
    virtual ViewPanel* duplicate(QWidget* parent = nullptr) const;

protected:
    // Extends the context menu shown when right-clicking this panel's tab —
    // ADS shows panels as tabs in the dock area's title bar rather than a
    // traditional per-window titlebar, and that tab is what "the titlebar"
    // means here. The base implementation adds a "Duplicate" action.
    // Subclasses that want their own entries should override this, call
    // ViewPanel::populateContextMenu(menu) first to keep the base entries,
    // then add their own — see ViewportViewPanel::populateContextMenu() for
    // an example.
    virtual void populateContextMenu(QMenu* menu);

    bool eventFilter(QObject* watched, QEvent* event) override;

    // Called whenever the stage this panel shows changes (via setStage(),
    // including the user picking a different entry in stageCombo()) — never
    // at construction time (a virtual call from ViewPanel's own constructor
    // would dispatch to this no-op base version, not a subclass override,
    // since the derived part of the object doesn't exist yet). Subclasses
    // instead perform their initial setup directly from currentStage() at
    // the end of their own constructor (see e.g. ViewportViewPanel), then
    // rely on this override for every later change.
    virtual void onStageChanged(const usdcc::usd::StageRefPtr& stage) {}

    // The "Stage:" combo — subclasses insert this into their own toolbar
    // layout (QLayout::addWidget() reparents it automatically, so being
    // parent-less here at construction is fine).
    QComboBox* stageCombo() const { return m_stageCombo; }

    // Repopulates stageCombo() from stageManager()->stages() and restores
    // its selection to match currentStage(). Subclasses call this once, as
    // the last line of their own constructor, to apply their initial stage
    // (see onStageChanged()'s comment for why the base constructor can't do
    // this itself).
    void refreshStageCombo();

private slots:
    void onStageComboChanged(int index);

private:
    usdcc::usd::StageManager* m_stageManager;
    usdcc::usd::StageRefPtr m_stage;
    QComboBox* m_stageCombo;

    // Kept alive for as long as this panel should keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<usdcc::usd::StageRefPtr>::Connection m_stageOpenedConnection;
    usdcc::core::Signal<usdcc::usd::StageRefPtr>::Connection m_stageClosedConnection;
};

}  // namespace usdcc::ui
