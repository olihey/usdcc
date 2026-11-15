// Workspaces (docs/WORKSPACES.md): the file format and its rules, the folders and
// which file wins, switching the window to one (reusing panels, one viewport per
// stage, views that follow the current stage), reading a layout back as one, the
// controller behind the title bar's button, and File > Open Stage with Options.
// Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/ui/workspace.h"
#include "usdcc/ui/workspace_applier.h"
#include "usdcc/ui/workspace_controller.h"
#include "usdcc/ui/workspace_library.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/open_stage_options_dialog.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_opener.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
#include <QPointer>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <memory>

using usdcc::ui::MainWindow;
using usdcc::ui::ViewPanel;
using usdcc::ui::ViewPanelRegistry;
using usdcc::ui::WorkspaceController;
using usdcc::ui::WorkspaceLibrary;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::ViewportViewPanel;
namespace workspace = usdcc::ui::workspace;

namespace {

QJsonObject parse(const char* text) { return QJsonDocument::fromJson(QByteArray(text)).object(); }

void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text);
}

QByteArray workspaceText(const char* id, const char* name, const char* group, int order) {
    return QString::fromUtf8(R"({"format": "usdcc-workspace", "version": 1, "id": "%1", "name": "%2", "group": "%3",
                             "order": %4, "layout": {"tabs": [{"panel": "viewport"}]}})")
        .arg(QLatin1String(id), QLatin1String(name), QLatin1String(group))
        .arg(order)
        .toUtf8();
}

void waitFor(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
}

// The view panels in `area`, open, in tab order.
QList<ViewPanel*> panelsIn(ads::CDockAreaWidget* area) {
    QList<ViewPanel*> list;
    if (!area) return list;
    for (ads::CDockWidget* dock : area->openedDockWidgets()) {
        if (auto* panel = qobject_cast<ViewPanel*>(dock)) list << panel;
    }
    return list;
}

QList<ViewPanel*> openPanels(ads::CDockManager* docks) {
    QList<ViewPanel*> list;
    for (ads::CDockWidget* dock : docks->dockWidgetsMap()) {
        auto* panel = qobject_cast<ViewPanel*>(dock);
        if (panel && !panel->isClosed()) list << panel;
    }
    return list;
}

QString typeOf(const ViewPanel* panel) { return panel->property(ViewPanelRegistry::kTypeProperty).toString(); }

// The app's panel types and its three fixed panels, as main.cpp makes them.
struct App {
    TestSettings settings;
    QTemporaryDir uiScaleFolder;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       uiScaleFolder.filePath(QStringLiteral("ui.ini"))};
    StageManager stages;
    std::shared_ptr<usdcc::usd::UndoManager> undo = std::make_shared<usdcc::usd::UndoManager>();
    std::shared_ptr<usdcc::tools::ToolSet> tools = std::make_shared<usdcc::tools::ToolSet>();
    ViewPanelRegistry registry;
    MainWindow window;

    App() {
        registry.add(QStringLiteral("viewport"), QStringLiteral("Viewport"),
                     [this](const StageRefPtr& stage, const QString&, QWidget* parent) -> ViewPanel* {
                         return new ViewportViewPanel(&stages, stage, tools, undo, QStringLiteral("Viewport"), parent);
                     });
        registry.add(QStringLiteral("outliner"), QStringLiteral("Outliner"),
                     [this](const StageRefPtr& stage, const QString& title, QWidget* parent) -> ViewPanel* {
                         return new usdcc::usd::OutlinerViewPanel(&stages, stage, undo, title, parent);
                     });
        registry.add(QStringLiteral("attributes"), QStringLiteral("Attributes"),
                     [this](const StageRefPtr& stage, const QString& title, QWidget* parent) -> ViewPanel* {
                         return new usdcc::usd::AttributesViewPanel(&stages, stage, undo, title, parent);
                     });
        auto* viewport = new ViewportViewPanel(&stages, nullptr, tools, undo, QStringLiteral("Viewport"), &window);
        viewport->setObjectName(QStringLiteral("Viewport"));
        auto* area = window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, viewport);
        auto* outliner = new usdcc::usd::OutlinerViewPanel(&stages, nullptr, undo, QStringLiteral("Outliner"), &window);
        outliner->setObjectName(QStringLiteral("Outliner"));
        window.dockManager()->addDockWidgetTabToArea(outliner, area);
        auto* attributes = new usdcc::usd::AttributesViewPanel(&stages, nullptr, undo, QStringLiteral("Attributes"), &window);
        attributes->setObjectName(QStringLiteral("Attributes"));
        window.dockManager()->addDockWidgetTabToArea(attributes, area);
        viewport->setProperty(ViewPanelRegistry::kTypeProperty, QStringLiteral("viewport"));
        outliner->setProperty(ViewPanelRegistry::kTypeProperty, QStringLiteral("outliner"));
        attributes->setProperty(ViewPanelRegistry::kTypeProperty, QStringLiteral("attributes"));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
    }
    // The panels a switch deleted (deleteLater(), out of the window) go now, while the
    // stages they were connected to still exist; the app's event loop does this at once.
    ~App() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

    workspace::ApplyContext context() {
        return {&window, &registry, &stages, {QStringLiteral("Viewport"), QStringLiteral("Outliner"), QStringLiteral("Attributes")}};
    }
};

// Outliner | (per-stage viewports over Attributes) | a Top view following the current stage.
const char* kThreeColumns = R"({
    "format": "usdcc-workspace", "version": 1, "id": "three", "name": "Three", "group": "Test", "order": 5,
    "layout": {"split": "horizontal", "sizes": [1, 3, 2], "items": [
        {"tabs": [{"panel": "outliner"}]},
        {"split": "vertical", "sizes": [0.75, 0.25], "items": [
            {"tabs": [{"panel": "viewport"}]},
            {"tabs": [{"panel": "attributes"}]}]},
        {"tabs": [{"panel": "viewport", "stage": "current", "camera": {"view": "top"}}]}]}})";

}  // namespace

USDCC_TEST(workspace_format_rules) {
    QString error;
    std::optional<workspace::Definition> definition = workspace::fromJson(parse(kThreeColumns), QStringLiteral("file"), &error);
    CHECK(definition.has_value(), "a good one reads: %s", qPrintable(error));
    if (!definition) return;
    CHECK(definition->id == "three" && definition->name == "Three" && definition->group == "Test" && definition->order == 5,
          "its id, name, group and order");
    const workspace::Node& root = definition->layout;
    CHECK(root.isSplit() && root.orientation == Qt::Horizontal && root.items.size() == 3, "three columns");
    CHECK(std::abs(root.sizes[0] - 1.0 / 6.0) < 1e-9 && std::abs(root.sizes[1] - 0.5) < 1e-9, "sizes as shares: %f %f",
          root.sizes[0], root.sizes[1]);
    CHECK(root.items[1].isSplit() && root.items[1].orientation == Qt::Vertical, "the middle one stacked");
    CHECK(root.items[2].tabs[0].followsCurrentStage() && root.items[2].tabs[0].camera.value("view") == "top",
          "a Top view following the current stage");
    CHECK(root.containsPerStageViewport() && !root.items[0].containsPerStageViewport(), "the per-stage viewport is in the middle");

    std::optional<workspace::Definition> again = workspace::fromJson(definition->toJson(), QString());
    CHECK(again && again->toJson() == definition->toJson(), "written and read back the same");

    auto refused = [](const char* text, const char* why) {
        QString error;
        const bool read = workspace::fromJson(parse(text), QStringLiteral("x"), &error).has_value();
        CHECK(!read && error.contains(QLatin1String(why)), "refused (%s): %s", why, qPrintable(error));
    };
    refused(R"({"version": 1, "name": "A", "layout": {"tabs": [{"panel": "viewport"}]}})", "isn't a workspace");
    refused(R"({"format": "usdcc-workspace", "version": 2, "name": "A", "layout": {"tabs": [{"panel": "viewport"}]}})", "newer");
    refused(R"({"format": "usdcc-workspace", "version": 1, "layout": {"tabs": [{"panel": "viewport"}]}})", "no \"name\"");
    refused(R"({"format": "usdcc-workspace", "version": 1, "name": "A", "layout": {"tabs": [{"panel": "outliner"}]}})",
            "no viewport");
    refused(R"({"format": "usdcc-workspace", "version": 1, "name": "A", "layout": {"tabs": [{"panel": "viewport"}, {"panel": "viewport"}]}})",
            "2 per-stage viewports");
    refused(R"({"format": "usdcc-workspace", "version": 1, "name": "A", "layout": {"split": "diagonal", "items": []}})",
            "\"split\" is \"diagonal\"");
    refused(R"({"format": "usdcc-workspace", "version": 1, "id": "a b", "name": "A", "layout": {"tabs": [{"panel": "viewport"}]}})",
            "may only hold");
    refused(R"({"format": "usdcc-workspace", "version": 1, "name": "A", "sidePanels": {"Log": "folded"}, "layout": {"tabs": [{"panel": "viewport"}]}})",
            "\"folded\"");

    // Short forms, the id from the file, and a split nested in one the same way.
    definition = workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1, "name": "B", "fitOnSwitch": true,
        "layout": {"split": "horizontal", "sizes": [1, 1], "items": [
            {"tabs": [{"panel": "viewport", "camera": {"fit": false}}]},
            {"split": "horizontal", "items": [{"tabs": [{"panel": "outliner"}]}, {"tabs": [{"panel": "usda"}]}]}]}})"),
                                     QStringLiteral("from-file"));
    CHECK(definition && definition->id == "from-file", "the id from the file name");
    CHECK(definition && definition->fitOnSwitch == "selection", "fitOnSwitch true means the selection");
    CHECK(definition && definition->layout.items.size() == 3 && std::abs(definition->layout.sizes[2] - 0.25) < 1e-9,
          "a split in one the same way is part of it, its shares scaled");
    CHECK(definition && definition->layout.items[0].tabs[0].camera.value("fit") == "none", "fit false means none");
    CHECK(workspace::idFromName(QStringLiteral("My  Look Dev!")) == "my-look-dev", "an id from a name: %s",
          qPrintable(workspace::idFromName(QStringLiteral("My  Look Dev!"))));
}

USDCC_TEST(workspace_unknown_panels_are_left_out) {
    std::optional<workspace::Definition> definition = workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1,
        "name": "C", "layout": {"split": "horizontal", "items": [
            {"tabs": [{"panel": "vray"}]},
            {"split": "vertical", "items": [{"tabs": [{"panel": "viewport"}]}, {"tabs": [{"panel": "vray"}, {"panel": "rtx"}]}]},
            {"tabs": [{"panel": "outliner"}, {"panel": "vray"}]}]}})"),
                                                                          QStringLiteral("c"));
    CHECK(definition.has_value(), "reads");
    if (!definition) return;
    QStringList missing;
    const workspace::Node node =
        workspace::pruned(definition->layout, [](const QString& type) { return type == QLatin1String("outliner"); }, &missing);
    CHECK(missing == QStringList({QStringLiteral("vray"), QStringLiteral("rtx")}), "each missing type once: %s",
          qPrintable(missing.join(',')));
    CHECK(node.isSplit() && node.items.size() == 2, "the empty column dropped, %d left", int(node.items.size()));
    CHECK(node.items.size() == 2 && !node.items[0].isSplit() && node.items[0].tabs[0].isPerStageViewport(),
          "the stack left with one area became that area");
    CHECK(node.items.size() == 2 && node.items[1].tabs.size() == 1 && node.items[1].tabs[0].panel == "outliner",
          "an area keeps what's known");
}

USDCC_TEST(workspace_library_folders) {
    TestSettings settings;
    QTemporaryDir user;
    QTemporaryDir shipped;
    write(shipped.filePath("layout.json"), workspaceText("layout", "Layout", "Scene", 10));
    write(shipped.filePath("lookdev.json"), workspaceText("lookdev", "Look Dev", "Shading", 40));
    write(shipped.filePath("four.json"), workspaceText("four", "Four View", "Scene", 20));
    write(shipped.filePath("loose.json"), workspaceText("loose", "Loose", "", 30));
    write(shipped.filePath("broken.json"), "{ not json");
    write(shipped.filePath("workspace.schema.json"), R"({"$schema": "x", "title": "not a workspace"})");
    write(user.filePath("layout.json"), workspaceText("layout", "My Layout", "Scene", 10));

    WorkspaceLibrary library({user.path(), shipped.path(), false});
    CHECK(library.workspaces().size() == 4, "four, %d", int(library.workspaces().size()));
    const workspace::Definition* layout = library.find(QStringLiteral("layout"));
    CHECK(layout && layout->name == "My Layout" && layout->source == "Your workspaces", "yours wins over the shipped one");
    CHECK(library.isUserWorkspace(QStringLiteral("layout")) && !library.isUserWorkspace(QStringLiteral("four")), "and is yours");
    const std::vector<workspace::Definition> hidden = library.shadowed(QStringLiteral("layout"));
    CHECK(hidden.size() == 1 && hidden.front().source == "Shipped with usdcc", "it hides the shipped one");
    CHECK(library.problems().size() == 1 && library.problems().front().contains("broken.json"),
          "the broken file is named, the schema isn't: %s", qPrintable(library.problems().join(';')));

    const std::vector<WorkspaceLibrary::Group> groups = library.groups();
    QStringList order;
    for (const WorkspaceLibrary::Group& group : groups) {
        QStringList names;
        for (const workspace::Definition* definition : group.workspaces) names << definition->name;
        order << group.name + ":" + names.join('/');
    }
    CHECK(order.join(' ') == "Scene:My Layout/Four View :Loose Shading:Look Dev",
          "groups by their lowest order, workspaces by order: %s", qPrintable(order.join(' ')));

    workspace::Definition mine = *library.find(QStringLiteral("lookdev"));
    mine.name = QStringLiteral("My Look Dev");
    QString error;
    CHECK(library.save(mine, &error), "saved: %s", qPrintable(error));
    CHECK(QFile::exists(user.filePath("lookdev.json")) && library.find(QStringLiteral("lookdev"))->name == "My Look Dev",
          "in your folder, and found again");
    CHECK(library.removeUserCopy(QStringLiteral("lookdev"), &error), "removed: %s", qPrintable(error));
    CHECK(library.find(QStringLiteral("lookdev"))->name == "Look Dev", "the shipped one is back");
    CHECK(!library.removeUserCopy(QStringLiteral("four")), "a shipped one isn't yours to remove");

    // The real shipped files read, and each names its viewport.
    WorkspaceLibrary real({user.path(), QString(), false});
    for (const char* id : {"layout", "side-by-side", "four-view", "viewport", "lookdev", "layering", "inspect", "scripting"}) {
        CHECK(real.find(QLatin1String(id)) != nullptr, "shipped: %s", id);
    }
    CHECK(real.problems().isEmpty(), "the shipped files all read: %s", qPrintable(real.problems().join(';')));
}

USDCC_TEST(workspace_switching) {
    App app;
    ads::CDockManager* docks = app.window.dockManager();
    const StageRefPtr one = app.stages.createStage();
    const StageRefPtr two = app.stages.createStage();
    app.stages.setCurrentStage(two);
    // Extra panels the registry made: an Outliner, on screen, which is reused before the
    // fixed one behind it, and an Attributes, which goes as the fixed one is reused.
    ViewPanel* extra = app.registry.open(QStringLiteral("outliner"), one, docks, &app.window);
    QPointer<ViewPanel> extraAttributes(app.registry.open(QStringLiteral("attributes"), one, docks, &app.window));
    // Behind the fixed Attributes, so that one wins.
    if (auto* fixedAttributes = docks->findDockWidget(QStringLiteral("Attributes"))) fixedAttributes->setAsCurrentTab();
    extra->setAsCurrentTab();
    QApplication::processEvents();

    std::optional<workspace::Definition> three = workspace::fromJson(parse(kThreeColumns), QString());
    workspace::ApplyResult result = workspace::apply(*three, app.context());
    QApplication::processEvents();
    CHECK(result.applied, "applied");

    ads::CDockAreaWidget* viewportArea = workspace::viewportArea(docks);
    const QList<ViewPanel*> viewports = panelsIn(viewportArea);
    CHECK(viewports.size() == 2 && viewports[0]->currentStage() == one && viewports[1]->currentStage() == two,
          "a viewport per stage, in the viewport area, in open order");
    CHECK(viewportArea && viewportArea->currentDockWidget() == viewports.value(1), "the current stage's in front");
    CHECK(docks->findDockWidget(QStringLiteral("Viewport")) == viewports.value(0), "the fixed Viewport reused for the first stage");

    ViewPanel* outliner = extra;
    ViewPanel* fixedOutliner = qobject_cast<ViewPanel*>(docks->findDockWidget(QStringLiteral("Outliner")));
    ViewPanel* attributes = qobject_cast<ViewPanel*>(docks->findDockWidget(QStringLiteral("Attributes")));
    CHECK(!outliner->isClosed() && outliner->currentStage() == two, "the Outliner on screen reused, showing the current stage");
    CHECK(fixedOutliner && fixedOutliner->isClosed(), "the fixed one behind it closed, kept");
    CHECK(attributes && attributes->dockAreaWidget() != viewportArea && !attributes->isClosed(), "Attributes under the viewports");
    CHECK(!extraAttributes || !docks->dockWidgetsMap().values().contains(extraAttributes.data()), "the extra Attributes is gone");

    ViewportViewPanel* top = nullptr;
    for (ViewPanel* panel : openPanels(docks)) {
        if (panel->followsCurrentStage()) top = qobject_cast<ViewportViewPanel*>(panel);
    }
    CHECK(top && top->hydraWindow()->view() == usdcc::usd::CameraView::Top && top->hydraWindow()->isOrthographic(),
          "the third column: an orthographic Top view");
    CHECK(top && top->windowTitle() == "Top" && top->currentStage() == two, "titled Top, showing the current stage");

    // The columns' shares (1 : 3 : 2).
    QApplication::processEvents();
    QSplitter* columns = outliner ? qobject_cast<QSplitter*>(outliner->dockAreaWidget()->parentWidget()) : nullptr;
    QList<int> sizes;
    for (int i = 0; columns && i < columns->count(); ++i) {
        if (!columns->widget(i)->isHidden()) sizes << columns->sizes()[i];
    }
    if (sizes.size() == 3) {
        const double total = sizes[0] + sizes[1] + sizes[2];
        CHECK(std::abs(sizes[0] / total - 1.0 / 6.0) < 0.05 && std::abs(sizes[2] / total - 1.0 / 3.0) < 0.05,
              "the columns' shares: %d %d %d", sizes[0], sizes[1], sizes[2]);
    } else {
        CHECK(false, "the outliner in a three-way splitter: %d", int(sizes.size()));
    }

    // Following: making the other stage current switches the Top view, not the per-stage ones.
    app.stages.setCurrentStage(one);
    CHECK(top && top->currentStage() == one, "the Top view follows");
    CHECK(viewports[1]->currentStage() == two, "the per-stage viewport stays on its stage");
    app.stages.setCurrentStage(two);

    // Switching again reuses the same panels.
    result = workspace::apply(*three, app.context());
    QApplication::processEvents();
    const QList<ViewPanel*> again = panelsIn(workspace::viewportArea(docks));
    CHECK(again == viewports, "the same viewports again");
    bool sameTop = false;
    for (ViewPanel* panel : openPanels(docks)) sameTop = sameTop || panel == top;
    CHECK(sameTop, "and the same Top view");

    // A workspace without Attributes or the Top view: the fixed one closes, the made one goes.
    QPointer<ViewPanel> topGuard(top);
    std::optional<workspace::Definition> simple = workspace::fromJson(
        parse(R"({"format": "usdcc-workspace", "version": 1, "name": "S", "sidePanels": {"Log": "hidden"},
                  "layout": {"split": "horizontal", "items": [{"tabs": [{"panel": "outliner"}]}, {"tabs": [{"panel": "viewport"}]}]}})"),
        QStringLiteral("simple"));
    result = workspace::apply(*simple, app.context());
    QApplication::processEvents();
    CHECK(attributes && attributes->isClosed() && docks->findDockWidget(QStringLiteral("Attributes")) == attributes,
          "the fixed Attributes is closed, not deleted");
    CHECK(!topGuard || !docks->dockWidgetsMap().values().contains(topGuard.data()), "the made Top view is gone");
    CHECK(app.window.logPanel()->isHidden(), "the Log hidden, as it says");
    CHECK(openPanels(docks).size() == 3 && !outliner->isClosed(), "the Outliner and two viewports open: %d",
          int(openPanels(docks).size()));

    // Read back as a workspace: one per-stage entry, beside the Outliner.
    const workspace::Definition captured = workspace::capture(&app.window, QStringLiteral("cap"), QStringLiteral("Captured"), false);
    int perStage = 0;
    captured.layout.forEachEntry([&perStage](const workspace::PanelEntry& entry) { perStage += entry.isPerStageViewport(); });
    CHECK(perStage == 1, "the two stages' viewports as one entry");
    CHECK(captured.layout.isSplit() && captured.layout.items.size() == 2 && captured.layout.items[0].tabs[0].panel == "outliner",
          "the Outliner beside them");
    CHECK(workspace::fromJson(captured.toJson(), QString()).has_value(), "a workspace that reads");
}

USDCC_TEST(workspace_capture_keeps_views) {
    App app;
    const StageRefPtr stage = app.stages.createStage();
    std::optional<workspace::Definition> three = workspace::fromJson(parse(kThreeColumns), QString());
    workspace::apply(*three, app.context());
    QApplication::processEvents();
    const workspace::Definition captured = workspace::capture(&app.window, QStringLiteral("c"), QStringLiteral("C"), true);
    QStringList entries;
    captured.layout.forEachEntry([&entries](const workspace::PanelEntry& entry) {
        entries << entry.panel + (entry.followsCurrentStage() ? QStringLiteral("(current,") + entry.camera.value("view").toString() + ")"
                                                              : QString());
    });
    CHECK(entries.join(' ') == "outliner viewport attributes viewport(current,top)", "the same panels and views: %s",
          qPrintable(entries.join(' ')));
    CHECK(captured.sidePanels.contains(QStringLiteral("Log")), "with the side panels");
    // Laid out from what was read back, it's the same.
    workspace::apply(captured, app.context());
    QApplication::processEvents();
    const workspace::Definition twice = workspace::capture(&app.window, QStringLiteral("c"), QStringLiteral("C"), true);
    QStringList again;
    twice.layout.forEachEntry([&again](const workspace::PanelEntry& entry) {
        again << entry.panel + (entry.followsCurrentStage() ? QStringLiteral("(current,") + entry.camera.value("view").toString() + ")"
                                                            : QString());
    });
    CHECK(again == entries, "the same panels again: %s", qPrintable(again.join(' ')));
}

USDCC_TEST(workspace_controller_and_button) {
    App app;
    QTemporaryDir user;
    QTemporaryDir shipped;
    write(shipped.filePath("layout.json"), workspaceText("layout", "Layout", "Scene", 10));
    write(shipped.filePath("three.json"), kThreeColumns);
    WorkspaceLibrary library({user.path(), shipped.path(), false});
    WorkspaceController controller(app.context(), &library);
    QToolButton* button = controller.createButton(&app.window);
    app.window.addTitleBarWidget(button);
    CHECK(controller.currentId() == "layout" && button->text() == "Layout", "Layout before any is chosen: %s",
          qPrintable(button->text()));

    CHECK(!controller.isChosen(), "none chosen yet");
    QString error;
    CHECK(controller.switchTo(QStringLiteral("three"), &error), "switched: %s", qPrintable(error));
    CHECK(controller.isChosen() && !usdcc::ui::appSettings().contains(QStringLiteral("Workspaces/arranged/layout")),
          "the layout before any was chosen isn't remembered as Layout's");
    CHECK(controller.currentId() == "three" && button->text() == "Three", "the button says so");
    CHECK(usdcc::ui::appSettings().value(WorkspaceController::kCurrentKey).toString() == "three", "and it's remembered");
    CHECK(!controller.switchTo(QStringLiteral("nope"), &error) && !error.isEmpty(), "no such workspace");

    QMenu menu;
    controller.populateMenu(&menu);
    QStringList texts;
    for (QAction* action : menu.actions()) {
        if (!action->text().isEmpty()) texts << action->text();
    }
    CHECK(texts.contains("Layout") && texts.contains("Three") && texts.contains("Save Current Layout as Workspace..."),
          "the workspaces and the commands: %s", qPrintable(texts.join('|')));
    QAction* checked = menu.findChild<QAction*>(QStringLiteral("workspace:three"));
    CHECK(checked && checked->isChecked(), "the active one checked");
    bool heading = false;
    for (QObject* child : menu.findChildren<QLabel*>(QStringLiteral("workspaceMenuHeading"))) {
        heading = heading || static_cast<QLabel*>(child)->text() == "TEST";
    }
    CHECK(heading, "headed by its group");

    // Saving the layout as a new workspace makes it the active one.
    const QString id = controller.saveCurrentLayout(QStringLiteral("My Three"), false, &error);
    CHECK(id == "my-three" && QFile::exists(user.filePath("my-three.json")) && controller.currentId() == id,
          "saved as yours: %s %s", qPrintable(id), qPrintable(error));
    CHECK(controller.switchTo(QStringLiteral("layout"), &error), "back to Layout: %s", qPrintable(error));
    CHECK(usdcc::ui::appSettings().contains(QStringLiteral("Workspaces/arranged/my-three")),
          "how My Three was left is remembered");
    CHECK(usdcc::core::Settings::instance().contains("Shortcuts/workspace.three"), "a shortcut setting per workspace");
}

USDCC_TEST(workspace_view_panel_follows_the_current_stage) {
    App app;
    const StageRefPtr one = app.stages.createStage();
    const StageRefPtr two = app.stages.createStage();
    app.stages.setCurrentStage(one);
    auto* panel = new usdcc::usd::OutlinerViewPanel(&app.stages, two, app.undo, QStringLiteral("F"), &app.window);
    app.window.dockManager()->addDockWidget(ads::RightDockWidgetArea, panel);
    panel->setFollowsCurrentStage(true);
    CHECK(panel->currentStage() == one, "turning it on shows the current stage");
    app.stages.setCurrentStage(two);
    CHECK(panel->currentStage() == two, "and it switches with it");
    // Its stage dropdown: the combo box listing the stages.
    for (QComboBox* combo : panel->findChildren<QComboBox*>()) {
        const int index = combo->findData(static_cast<qlonglong>(one->cacheId()));
        if (index >= 0 && combo->count() == 2) {
            combo->setCurrentIndex(index);
            break;
        }
    }
    CHECK(panel->currentStage() == one && !panel->followsCurrentStage(), "picking a stage stops it");
    app.stages.setCurrentStage(two);
    CHECK(panel->currentStage() == one, "and it stays");
}

USDCC_TEST(workspace_open_stage_with_options) {
    App app;
    QTemporaryDir files;
    write(files.filePath("payload.usda"), R"(#usda 1.0
(
    defaultPrim = "Thing"
)

def Xform "Thing"
{
    def Sphere "Ball"
    {
    }
}
)");
    write(files.filePath("main.usda"), R"(#usda 1.0

def Xform "World"
{
    def Xform "Thing" (
        payload = @./payload.usda@
    )
    {
    }
}
)");

    QTemporaryDir user;
    QTemporaryDir shipped;
    write(shipped.filePath("layout.json"), workspaceText("layout", "Layout", "Scene", 10));
    write(shipped.filePath("three.json"), kThreeColumns);
    WorkspaceLibrary library({user.path(), shipped.path(), false});
    usdcc::usd::OpenStageOptionsDialog dialog(files.filePath("main.usda"), &library);
    usdcc::usd::StageOpener::Options options = dialog.options();
    CHECK(options.loadPayloads && options.makeCurrent == usdcc::usd::StageOpener::Options::MakeCurrent::Everywhere &&
              options.workspace.isEmpty(),
          "by default: payloads, made current, the layout kept");
    QComboBox* box = dialog.workspaceBox();
    CHECK(box->itemText(0) == "Use existing" && box->findData(QStringLiteral("three")) > 0, "the workspaces to pick from");
    box->setCurrentIndex(box->findData(QStringLiteral("three")));
    dialog.loadPayloadsBox()->setChecked(false);
    dialog.makeCurrentBox()->setChecked(false);
    options = dialog.options();
    CHECK(!options.loadPayloads && options.makeCurrent == usdcc::usd::StageOpener::Options::MakeCurrent::No &&
              options.workspace == "three",
          "as picked");

    usdcc::usd::StageOpener opener(&app.stages, &app.window);
    StageRefPtr opened;
    QObject::connect(&opener, &usdcc::usd::StageOpener::opened, &app.window,
                     [&opened](const StageRefPtr& stage, const usdcc::usd::StageOpener::Options&, bool) { opened = stage; });
    // A failure would show a message box, which nobody here would close: say what it
    // says, and close it.
    QString failure;
    QTimer watcher;
    QObject::connect(&watcher, &QTimer::timeout, &app.window, [&failure]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            failure = box->text() + QLatin1Char(' ') + box->informativeText();
            box->done(QMessageBox::Ok);
        }
    });
    watcher.start(50);
    usdcc::usd::StageOpener::Options unloaded;
    unloaded.loadPayloads = false;
    opener.open(files.filePath("main.usda"), unloaded);
    waitFor([&]() { return !opener.busy(); }, 15000);
    watcher.stop();
    CHECK(failure.isEmpty(), "no error: %s", qPrintable(failure));
    CHECK(opened != nullptr, "opened");
    if (opened) {
        const PXR_NS::UsdPrim thing = opened->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Thing"));
        CHECK(thing && thing.HasAuthoredPayloads() && !thing.IsLoaded(), "its payload isn't loaded");
        CHECK(!opened->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Thing/Ball")), "so what's in it isn't there");
    }
}
