#pragma once

#include <QMainWindow>

namespace usdcc::ui {

// Bare application shell for M0. Qt Advanced Docking System integration and
// the SidePanel/ViewPanel base classes land in milestone M1.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
};

}  // namespace usdcc::ui
