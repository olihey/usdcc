#pragma once

#include <DockWidget.h>

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
};

}  // namespace usdcc::ui
