#pragma once

#include <QMainWindow>

namespace ads {
class CDockManager;
}

namespace usdcc::ui {

// QMainWindow shell hosting the Qt Advanced Docking System. Panel
// registration currently instantiates a stand-in SidePanel/ViewPanel to
// prove the docking wiring end to end; real subclasses land across later
// milestones (see docs/PLAN.md section 4 and section 7).
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void restoreLayout();
    void saveLayout() const;

    ads::CDockManager* m_dockManager = nullptr;
};

}  // namespace usdcc::ui
