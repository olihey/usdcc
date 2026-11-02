// Side panels made in Python (docs/SCRIPTING.md §8, phase 5): a
// usdcc.ui.SidePanel subclass docked with usdcc.app.add_side_panel() is in the
// window and its View menu, belongs to the window, and goes back where the
// saved layout had it when a later session adds it again. Needs PySide6
// (tools/build-pyside.ps1); without it this file isn't built.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/main_window.h"

#include <QAction>
#include <QCoreApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>

using usdcc::scripting::AppContextScope;
using usdcc::test::pythonTestApp;
using usdcc::ui::MainWindow;

namespace {
// The window saves its layout to usdcc.ini in the app's config folder; Qt's
// test mode moves that folder away from the user's.
struct TestSettings {
    TestSettings() {
        QStandardPaths::setTestModeEnabled(true);
        remove();
    }
    ~TestSettings() {
        remove();
        QStandardPaths::setTestModeEnabled(false);
    }
    static void remove() {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini");
    }
};

QMenu* viewMenu(MainWindow& window) {
    for (QAction* action : window.mainMenuBar()->actions()) {
        if (action->menu() && action->text() == QObject::tr("&View")) return action->menu();
    }
    return nullptr;
}

bool inViewMenu(MainWindow& window, const QString& text) {
    QMenu* menu = viewMenu(window);
    if (!menu) return false;
    for (const QAction* action : menu->actions()) {
        if (action->text() == text) return true;
    }
    return false;
}

// Lets the shown window settle (MainWindow::layoutSettled()), as the app
// waits for it before running startup scripts.
bool waitUntilSettled(MainWindow& window) {
    QElapsedTimer timer;
    timer.start();
    while (!window.isLayoutSettled() && timer.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return window.isLayoutSettled();
}

// Defines the panel class and docks one; the script's namespace (and with it
// Python's reference to the panel) is gone once this returns.
bool addNotesPanel(const char* area) {
    return pythonTestApp().python.runString(std::string(R"(
import usdcc.app, usdcc.ui
from PySide6 import QtWidgets

class Notes(usdcc.ui.SidePanel):
    def __init__(self):
        super().__init__("Notes")
        self.setWidget(QtWidgets.QLabel("Notes from Python"))

usdcc.app.add_side_panel(Notes(), ")") + area + "\")\n",
                                                "add_notes.py");
}
}  // namespace

USDCC_TEST(python_side_panel) {
    TestSettings settings;
    auto& app = pythonTestApp();
    {
        MainWindow window;
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window});
        window.buildViewMenu();
        window.restoreLayout();
        window.show();
        CHECK(waitUntilSettled(window), "the shown window's layout settles");

        const auto title = app.python.evaluate("__import__('usdcc.app').app.main_window().windowTitle()");
        CHECK(title && *title == "usdcc", "main_window() is the window as a QMainWindow: %s",
              title ? title->c_str() : "?");

        CHECK(addNotesPanel("left"), "a Python panel is added");
        auto* notes = window.findChild<QDockWidget*>(QStringLiteral("Notes"));
        CHECK(notes, "it's in the window, and still there after the script's namespace went");
        if (!notes) return;
        QCoreApplication::processEvents();
        CHECK(window.dockWidgetArea(notes) == Qt::LeftDockWidgetArea && notes->isVisible(),
              "docked on the left and shown, also once the window has handled its events");
        CHECK(inViewMenu(window, QStringLiteral("Notes")), "listed in the View menu");

        CHECK(!app.python.runString("import usdcc.app\nfrom PySide6 import QtWidgets\n"
                                    "usdcc.app.add_side_panel(QtWidgets.QDockWidget('Plain'))\n"),
              "anything but a usdcc.ui.SidePanel is refused");
        CHECK(!app.python.runString("import usdcc.app, usdcc.ui\n"
                                    "usdcc.app.add_side_panel(usdcc.ui.SidePanel('Lost'), 'middle')\n"),
              "and so is an unknown area");

        window.close();  // saves the layout, with Notes on the left
    }
    {
        // The next session: the layout is restored before the script runs,
        // and the panel goes back to the left although it asks for the bottom.
        MainWindow window;
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window});
        window.buildViewMenu();
        window.restoreLayout();
        CHECK(addNotesPanel("bottom"), "added again in the next session");
        auto* notes = window.findChild<QDockWidget*>(QStringLiteral("Notes"));
        CHECK(notes && window.dockWidgetArea(notes) == Qt::LeftDockWidgetArea,
              "back where the saved layout had it: area %d", notes ? int(window.dockWidgetArea(notes)) : -1);

        delete notes;
        CHECK(!inViewMenu(window, QStringLiteral("Notes")), "a deleted panel leaves the View menu");
    }
    const auto noWindow = app.python.evaluate("__import__('usdcc.app').app.is_embedded()");
    CHECK(noWindow && *noWindow == "True", "the test app's own context is back");
}
