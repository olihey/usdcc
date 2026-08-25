#pragma once

#include "usdcc/ui/view_panel.h"

class QComboBox;

namespace usdcc::usd {

class StageManager;
class HydraViewportWindow;

// ViewPanel subclass that renders a USD stage via Hydra and lets the user
// switch between loaded stages and available render delegates (see
// docs/PLAN.md milestone M3). Actual navigation (orbit/pan/zoom) and
// rendering live in HydraViewportWidget; this class is the ViewPanel/toolbar
// wrapper around it.
//
// Deliberately built as part of the usdcc_usd_viewport target (see
// src/cpp/usd/CMakeLists.txt) rather than usdcc_ui: it only exists when USD
// is available, and keeping it out of usdcc_usd itself means usdcc.usd's
// pybind11 module doesn't pull in Qt Widgets/ADS as a runtime dependency it
// doesn't otherwise need.
class ViewportViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit ViewportViewPanel(StageManager* stageManager, const QString& title = QStringLiteral("Viewport"),
                                QWidget* parent = nullptr);

    HydraViewportWindow* hydraWindow() const { return m_viewport; }

private slots:
    void refreshStageCombo();
    void refreshRendererCombo();
    void onStageComboChanged(int index);
    void onRendererComboChanged(int index);

private:
    StageManager* m_stageManager;
    HydraViewportWindow* m_viewport;
    QComboBox* m_stageCombo;
    QComboBox* m_rendererCombo;
};

}  // namespace usdcc::usd
