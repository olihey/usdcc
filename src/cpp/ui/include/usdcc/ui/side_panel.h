#pragma once

#include <QDockWidget>

namespace usdcc::ui {

// Base class for panels docked at the edges of the QMainWindow, outside the
// Qt Advanced Docking System container (LogSidePanel, ScriptingSidePanel,
// USDASidePanel — see docs/PLAN.md section 4).
class SidePanel : public QDockWidget {
    Q_OBJECT

public:
    explicit SidePanel(const QString& title, QWidget* parent = nullptr);
};

}  // namespace usdcc::ui
