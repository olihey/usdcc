#include "usdcc/ui/main_window.h"

namespace usdcc::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("usdcc");
    resize(1280, 800);
}

}  // namespace usdcc::ui
