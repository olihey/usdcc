// Panels made in Python (docs/SCRIPTING.md §8). Side panels (phase 5): a
// usdcc.ui.SidePanel subclass docked with usdcc.app.add_side_panel() is in the
// window and its View menu, belongs to the window, and goes back where the
// saved layout had it when a later session adds it again. View panels (M1): a
// usdcc.ui.ViewPanel subclass registered with register_view_panel() opens as
// a ScriptedViewPanel that follows its stage, fills its options menu, floats,
// and duplicates. Needs PySide6 (tools/build-pyside.ps1); without it this
// file isn't built.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/scripted_view_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"

#include <DockManager.h>

#include <QAction>
#include <QCoreApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>

#include <filesystem>
#include <fstream>
#include <memory>

using usdcc::scripting::AppContextScope;
using usdcc::scripting::ScriptedViewPanel;
using usdcc::test::pythonTestApp;
using usdcc::ui::MainWindow;

namespace {
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

namespace {
QString labelText(QWidget* panel) {
    const auto labels = panel->findChildren<QLabel*>();
    return labels.isEmpty() ? QString() : labels.back()->text();
}

std::vector<ScriptedViewPanel*> scriptedPanels(MainWindow& window) {
    std::vector<ScriptedViewPanel*> panels;
    for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
        if (auto* panel = qobject_cast<ScriptedViewPanel*>(dock)) panels.push_back(panel);
    }
    return panels;
}
}  // namespace

USDCC_TEST(python_view_panel) {
    TestSettings settings;
    auto& app = pythonTestApp();
    const auto file = std::filesystem::temp_directory_path() /
                      ("usdcc_view_panel_" + std::to_string(QCoreApplication::applicationPid()) + ".usda");
    std::ofstream(file) << "#usda 1.0\ndef Xform \"World\"\n{\n    def Cube \"Box\"\n    {\n    }\n}\n";
    const usdcc::usd::StageRefPtr stage = app.stages.openStage(file.string());
    {
        MainWindow window;
        usdcc::ui::ViewPanelRegistry registry;
        registry.add(QStringLiteral("builtin"), QStringLiteral("Built-in"),
                     [](const usdcc::usd::StageRefPtr&, const QString&, QWidget*) { return nullptr; });
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry});

        CHECK(app.python.runString("stage_id = " + std::to_string(stage->cacheId()) + R"(
import usdcc.app, usdcc.ui, usdcc.usd
from PySide6 import QtWidgets

class PrimCount(usdcc.ui.ViewPanel):
    def create_widget(self):
        self.label = QtWidgets.QLabel("not yet")
        return self.label
    def stage_changed(self, stage):
        self.label.setText("no stage" if stage is None else "%d prims" % len(list(stage.Traverse())))
    def populate_options_menu(self, menu):
        menu.addAction("Count Inactive Prims")

usdcc.app.register_view_panel("prim_count", "Prim Count", PrimCount)
stage = [s for s in usdcc.usd.stages() if usdcc.usd.stage_id(s) == stage_id][0]
panel = usdcc.app.open_view_panel("prim_count", stage)
assert isinstance(panel, PrimCount) and panel.stage == stage, panel
assert ("prim_count", "Prim Count") in usdcc.app.view_panel_types()
)", "register_view_panel.py"),
              "a Python view panel type is registered and opened");
        const auto* entry = registry.find(QStringLiteral("prim_count"));
        CHECK(entry && entry->displayName == "Prim Count", "it's a type like the built-in ones (the Stages menu's)");

        const auto panels = scriptedPanels(window);
        CHECK(panels.size() == 1, "one panel docked: %d", int(panels.size()));
        if (panels.size() != 1) return;
        ScriptedViewPanel* panel = panels.front();
        CHECK(panel->currentStage() == stage && labelText(panel) == "2 prims",
              "it shows the stage, and its Python widget saw it: [%s]", qPrintable(labelText(panel)));

        QMenu menu;
        panel->populateOptionsMenu(&menu);
        CHECK(!menu.actions().isEmpty() && menu.actions().back()->text() == "Count Inactive Prims",
              "Python fills the options menu");

        panel->setStage(nullptr);
        CHECK(labelText(panel) == "no stage", "a stage change reaches Python: [%s]", qPrintable(labelText(panel)));
        panel->setStage(stage);

        panel->setFloating();
        CHECK(panel->isFloating(), "it undocks into a floating window, like any view panel");

        std::unique_ptr<usdcc::ui::ViewPanel> copy(panel->duplicate());
        CHECK(copy && copy->currentStage() == stage && labelText(copy.get()) == "2 prims",
              "Duplicate makes another instance on the same stage: [%s]",
              copy ? qPrintable(labelText(copy.get())) : "?");
        copy.reset();

        CHECK(!app.python.runString("import usdcc.app, usdcc.ui\n"
                                    "class X(usdcc.ui.ViewPanel): pass\n"
                                    "usdcc.app.register_view_panel('builtin', 'Mine', X)\n"),
              "a built-in type can't be replaced");
        CHECK(!app.python.runString("import usdcc.app\nusdcc.app.register_view_panel('plain', 'Plain', object)\n"),
              "only usdcc.ui.ViewPanel subclasses are accepted");

        CHECK(app.python.runString("stage_id = " + std::to_string(stage->cacheId()) + R"(
import usdcc.app, usdcc.ui, usdcc.usd
class Broken(usdcc.ui.ViewPanel):
    def create_widget(self):
        raise RuntimeError("broken panel")
usdcc.app.register_view_panel("broken", "Broken", Broken)
usdcc.app.open_view_panel("broken", [s for s in usdcc.usd.stages() if usdcc.usd.stage_id(s) == stage_id][0])
)", "broken_panel.py"),
              "a panel whose Python code raises still opens");
        bool saysWhy = false;
        for (ScriptedViewPanel* scripted : scriptedPanels(window)) {
            if (!scripted->pythonPanel()) saysWhy = labelText(scripted).contains("failed");
        }
        CHECK(saysWhy, "and says its code failed instead of showing content");
    }
    app.stages.closeStage(stage);
    std::filesystem::remove(file);
}
