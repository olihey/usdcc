// ViewPanels across a restart: a panel opened from the Stages menu (an
// Outliner for a stage) comes back where it was, and a startup panel the
// saved layout doesn't mention (one deleted by Close Stage, say) comes back
// open instead of staying hidden. Runs in usdcc_ui_tests (Qt's offscreen
// platform); the layout is saved in Qt's test-mode config folder, never in
// the user's usdcc.ini.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

#include <memory>

using usdcc::ui::MainWindow;
using usdcc::ui::ViewPanel;
using usdcc::ui::ViewPanelRegistry;
using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::UndoManager;

namespace {
void settle(MainWindow& window) {
    QElapsedTimer timer;
    timer.start();
    while (!window.isLayoutSettled() && timer.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents();
}

// One run of the app: the startup Outliner (as main.cpp makes it), and the
// registry's types.
struct Session {
    StageManager& stages;
    std::shared_ptr<UndoManager> undo;
    ViewPanelRegistry registry;
    MainWindow window;

    Session(StageManager& stageManager, std::shared_ptr<UndoManager> undoManager, bool startupOutliner = true)
        : stages(stageManager), undo(std::move(undoManager)) {
        registry.add(QStringLiteral("outliner"), QStringLiteral("Outliner"),
                     [this](const usdcc::usd::StageRefPtr& stage, const QString& title, QWidget* parent) -> ViewPanel* {
                         return new OutlinerViewPanel(&stages, stage, undo, title, parent);
                     });
        if (startupOutliner) {
            auto* outliner = new OutlinerViewPanel(&stages, nullptr, undo, QStringLiteral("Outliner"), &window);
            window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, outliner);
        }
        registry.restorePanels(window.dockManager(), &window, [this](const QString& identifier) {
            for (const usdcc::usd::StageRefPtr& stage : stages.stages()) {
                if (stage->usdStage()->GetRootLayer()->GetIdentifier() == identifier.toStdString()) return stage;
            }
            return usdcc::usd::StageRefPtr();
        });
        window.restoreLayout();
        window.resize(1000, 700);
        window.show();
        settle(window);
    }

    ads::CDockWidget* panel(const QString& name) { return window.dockManager()->findDockWidget(name); }

    void close() {
        registry.savePanels(window.dockManager());
        window.close();
    }
};
}  // namespace

USDCC_TEST(view_panel_opened_from_stages_menu_survives_restart) {
    TestSettings settings;
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const usdcc::usd::StageRefPtr stage = stages.createStage();
    QString name;
    {
        Session first(stages, undo);
        ViewPanel* opened = first.registry.open(QStringLiteral("outliner"), stage, first.window.dockManager(), &first.window);
        CHECK(opened && !opened->isClosed(), "an Outliner opened for the stage");
        if (!opened) return;
        name = opened->objectName();
        first.close();
    }
    {
        Session second(stages, undo);
        ads::CDockWidget* restored = second.panel(name);
        CHECK(restored && !restored->isClosed(), "it's back after a restart: %s", qPrintable(name));
        auto* outliner = qobject_cast<OutlinerViewPanel*>(restored);
        CHECK(outliner && outliner->currentStage() == stage, "showing its stage, which is still open");
        CHECK(second.panel("Outliner") && !second.panel("Outliner")->isClosed(), "the startup Outliner too");
        second.close();
    }
}

USDCC_TEST(view_panel_missing_from_layout_comes_back_open) {
    TestSettings settings;
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    {
        // A run whose startup Outliner was gone at exit (deleted by Close
        // Stage): the saved layout doesn't mention it.
        Session first(stages, undo, false);
        auto* other = new OutlinerViewPanel(&stages, nullptr, undo, QStringLiteral("Other"), &first.window);
        first.window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, other);
        first.close();
    }
    {
        Session second(stages, undo);
        CHECK(second.panel("Outliner") && !second.panel("Outliner")->isClosed(),
              "a startup panel the layout doesn't know comes up open");
        second.close();
    }
    {
        // One the user closed stays closed.
        Session third(stages, undo);
        third.panel("Outliner")->toggleView(false);
        QApplication::processEvents();
        third.close();
    }
    {
        Session fourth(stages, undo);
        CHECK(fourth.panel("Outliner") && fourth.panel("Outliner")->isClosed(), "a panel closed on purpose stays closed");
        fourth.close();
    }
}
