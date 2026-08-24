#include <QApplication>

#include "usdcc/ui/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    usdcc::ui::MainWindow window;
    window.show();

    return app.exec();
}
