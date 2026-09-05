#pragma once

#include <DockWidget.h>

class QMenu;

namespace usdcc::ui {

// Base class for panels living inside the Qt Advanced Docking System
// container (ViewportViewPanel, OutlinerViewPanel, AttributesViewPanel —
// see docs/PLAN.md section 4). Every ViewPanel will expose a dropdown to
// switch between the currently loaded USD stages once milestone M2
// introduces stage management; that selector isn't wired up yet.
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
    explicit ViewPanel(const QString& title, QWidget* parent = nullptr);

    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const;

    // Creates a copy of this panel. The base implementation just constructs
    // a plain ViewPanel with the same title — it has no way to generically
    // duplicate an arbitrary content widget, and no way to invoke a
    // subclass's constructor (which typically needs extra arguments, e.g.
    // ViewportViewPanel's StageManager*). Subclasses that carry their own
    // state should override this: construct their own type, then copy
    // whatever values make the duplicate meaningfully match the original
    // (e.g. the currently-viewed stage, camera position, selection).
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
};

}  // namespace usdcc::ui
