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
#include "usdcc/ui/side_panel.h"
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

    // Limits, and side panels with an edge and a size.
    definition = workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1, "name": "L",
        "sidePanels": {"Log": "collapsed", "Script Editor": {"state": "shown", "area": "right", "size": 0.3}},
        "layout": {"split": "horizontal", "items": [
            {"tabs": [{"panel": "outliner"}], "maxWidth": 300},
            {"split": "vertical", "items": [
                {"split": "horizontal", "items": [{"tabs": [{"panel": "viewport"}]}, {"tabs": [{"panel": "viewport", "stage": "current"}]}]},
                {"tabs": [{"panel": "viewport", "stage": "current"}]}]},
            {"split": "vertical", "maxWidth": 400, "items": [{"tabs": [{"panel": "attributes"}]}, {"tabs": [{"panel": "layers"}]}]}]}})"),
                                     QStringLiteral("limits"), &error);
    CHECK(definition.has_value(), "limits read: %s", qPrintable(error));
    if (!definition) return;
    const workspace::Node& limited = definition->layout;
    CHECK(limited.items[0].maxWidth == 300 && limited.items[2].maxWidth == 400 && limited.items[1].maxWidth == 0,
          "maxWidth on an area and a split");
    CHECK(limited.hasMaxSize() && !limited.items[1].hasMaxSize(), "the layout has limits, the viewports' column none");
    CHECK(limited.items[1].viewportCount(Qt::Horizontal) == 2 && limited.items[1].viewportCount(Qt::Vertical) == 2 &&
              limited.items[0].viewportCount(Qt::Horizontal) == 0 && limited.viewportCount(Qt::Horizontal) == 2,
          "viewports side by side and stacked");
    const workspace::SidePanelEntry log = definition->sidePanels.value(QStringLiteral("Log"));
    const workspace::SidePanelEntry editor = definition->sidePanels.value(QStringLiteral("Script Editor"));
    CHECK(log.state == "collapsed" && log.area.isEmpty() && log.size == 0.0, "a side panel's state alone");
    CHECK(editor.state == "shown" && editor.area == "right" && std::abs(editor.size - 0.3) < 1e-9,
          "a side panel's edge and size");
    again = workspace::fromJson(definition->toJson(), QString());
    CHECK(again && again->toJson() == definition->toJson() && again->layout.items[2].maxWidth == 400 &&
              again->sidePanels == definition->sidePanels,
          "limits and side panels written and read back the same");
    CHECK(definition->toJson().value("sidePanels").toObject().value("Log") == QJsonValue(QStringLiteral("collapsed")),
          "a side panel with only a state written as one");
    refused(R"({"format": "usdcc-workspace", "version": 1, "name": "A", "sidePanels": {"Log": {"area": "middle"}}, "layout": {"tabs": [{"panel": "viewport"}]}})",
            "\"middle\"");
    refused(R"({"format": "usdcc-workspace", "version": 1, "name": "A", "layout": {"tabs": [{"panel": "viewport"}], "maxWidth": "wide"}})",
            "\"maxWidth\" isn't");
    // A split of one keeps its limit on its child.
    definition = workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1, "name": "O",
        "layout": {"split": "horizontal", "items": [
            {"split": "vertical", "maxWidth": 250, "items": [{"tabs": [{"panel": "outliner"}]}]},
            {"tabs": [{"panel": "viewport"}]}]}})"),
                                     QStringLiteral("one"));
    CHECK(definition && definition->layout.items[0].maxWidth == 250 && !definition->layout.items[0].isSplit(),
          "a split of one gives its limit to its child");
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

// The Outliner and the Attributes kept to their limits, the two viewport columns sharing
// the rest evenly; side panels as the workspace says, the others hidden.
USDCC_TEST(workspace_limits_and_side_panels) {
    App app;
    ads::CDockManager* docks = app.window.dockManager();
    app.stages.createStage();
    QApplication::processEvents();
    std::optional<workspace::Definition> limited = workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1,
        "name": "Limited", "sidePanels": {"Log": {"state": "shown", "area": "right"}},
        "layout": {"split": "horizontal", "sizes": [3, 1, 1, 3], "items": [
            {"tabs": [{"panel": "outliner"}], "maxWidth": 200},
            {"tabs": [{"panel": "viewport"}]},
            {"tabs": [{"panel": "viewport", "stage": "current", "camera": {"view": "top"}}]},
            {"tabs": [{"panel": "attributes"}], "maxWidth": 250}]}})"),
                                                                       QStringLiteral("limited"));
    CHECK(limited.has_value(), "reads");
    if (!limited) return;
    CHECK(workspace::apply(*limited, app.context()).applied, "applied");
    waitFor([] { return false; }, 200);  // the deferred second pass, once laid out

    ViewPanel* outliner = nullptr;
    ViewPanel* attributes = nullptr;
    for (ViewPanel* panel : openPanels(docks)) {
        if (typeOf(panel) == "outliner") outliner = panel;
        if (typeOf(panel) == "attributes") attributes = panel;
    }
    QSplitter* columns = outliner ? qobject_cast<QSplitter*>(outliner->dockAreaWidget()->parentWidget()) : nullptr;
    QList<int> sizes;
    for (int i = 0; columns && i < columns->count(); ++i) {
        if (!columns->widget(i)->isHidden()) sizes << columns->sizes()[i];
    }
    CHECK(sizes.size() == 4, "four columns: %d", int(sizes.size()));
    if (sizes.size() == 4) {
        const int scaled200 = usdcc::ui::UiScaleController::instance().pixels(200);
        const int scaled250 = usdcc::ui::UiScaleController::instance().pixels(250);
        // Its own minimum may hold a panel above its limit; within a few pixels otherwise.
        CHECK(sizes[0] <= std::max(scaled200, outliner->dockAreaWidget()->minimumSizeHint().width()) + 4,
              "the Outliner kept to its limit despite its share: %d", sizes[0]);
        CHECK(sizes[3] <= std::max(scaled250, attributes->dockAreaWidget()->minimumSizeHint().width()) + 4,
              "the Attributes kept to its limit: %d", sizes[3]);
        CHECK(std::abs(sizes[1] - sizes[2]) <= 2, "the viewports share the rest evenly: %d %d", sizes[1], sizes[2]);
        CHECK(sizes[1] > sizes[0] && sizes[1] > sizes[3], "and get the most: %d %d %d %d", sizes[0], sizes[1], sizes[2],
              sizes[3]);
    }
    // Only the viewports grow with the window.
    if (columns && sizes.size() == 4) {
        const QList<int> before = columns->sizes();
        app.window.resize(app.window.width() + 300, app.window.height());
        waitFor([&] { return columns->sizes() != before; }, 1000);
        QList<int> after;
        for (int i = 0; i < columns->count(); ++i) {
            if (!columns->widget(i)->isHidden()) after << columns->sizes()[i];
        }
        CHECK(after.size() == 4 && std::abs(after[0] - sizes[0]) <= 2 && std::abs(after[3] - sizes[3]) <= 2 &&
                  after[1] > sizes[1] && after[2] > sizes[2],
              "a wider window widens the viewports only: %d %d %d %d", after.value(0), after.value(1), after.value(2),
              after.value(3));
    }

    usdcc::ui::SidePanel* logPanel = app.window.logPanel();
    CHECK(!logPanel->isHidden() && !logPanel->isCollapsed() && app.window.dockWidgetArea(logPanel) == Qt::RightDockWidgetArea,
          "the Log shown, moved to the right as it says");
    const workspace::Definition captured = workspace::capture(&app.window, QStringLiteral("c"), QStringLiteral("C"), true);
    const workspace::SidePanelEntry log = captured.sidePanels.value(QStringLiteral("Log"));
    CHECK(log.state == "shown" && log.area == "right" && log.size > 0.0 && log.size < 1.0,
          "captured with its edge and share: %s %s %f", qPrintable(log.state), qPrintable(log.area), log.size);

    // One that names no side panels has none.
    std::optional<workspace::Definition> bare = workspace::fromJson(
        parse(R"({"format": "usdcc-workspace", "version": 1, "name": "Bare", "layout": {"tabs": [{"panel": "viewport"}]}})"),
        QStringLiteral("bare"));
    CHECK(bare && workspace::apply(*bare, app.context()).applied, "bare applied");
    CHECK(logPanel->isHidden(), "the Log, not named, hidden");
    // Named by its title works too.
    std::optional<workspace::Definition> titled = workspace::fromJson(
        parse(R"({"format": "usdcc-workspace", "version": 1, "name": "T", "sidePanels": {"Log": "collapsed"},
                  "layout": {"tabs": [{"panel": "viewport"}]}})"),
        QStringLiteral("titled"));
    logPanel->setObjectName(QStringLiteral("logByAnotherName"));
    CHECK(titled && workspace::apply(*titled, app.context()).applied, "titled applied");
    CHECK(!logPanel->isHidden() && logPanel->isCollapsed(), "found by its title, collapsed");
    logPanel->setObjectName(QStringLiteral("Log"));
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
    CHECK(button->icon().isNull() && button->toolButtonStyle() == Qt::ToolButtonTextOnly && button->menu(),
          "a push button: its name and the menu's arrow, no icon");

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
    CHECK(texts.contains("Open Personal Workspace Folder") && !texts.contains("Open Workspaces Folder"),
          "the personal folder's command");

    // View > Workspace is the same menu; a stage's has the workspaces alone, unchecked.
    QMenu* viewWorkspaces = controller.createMenu(QStringLiteral("Workspace"), &app.window);
    Q_EMIT viewWorkspaces->aboutToShow();
    CHECK(viewWorkspaces->actions().size() == menu.actions().size(), "View > Workspace is the same menu");
    QMenu stageWorkspaces;
    bool before = false;
    controller.addWorkspaceActions(&stageWorkspaces, [&before] { before = true; }, false);
    QAction* stageThree = stageWorkspaces.findChild<QAction*>(QStringLiteral("workspace:three"));
    QAction* stageLayout = stageWorkspaces.findChild<QAction*>(QStringLiteral("workspace:layout"));
    QStringList stageTexts;
    for (QAction* action : stageWorkspaces.actions()) {
        if (!action->text().isEmpty()) stageTexts << action->text();
    }
    CHECK(stageThree && !stageThree->isChecked() && !stageTexts.contains("Save Current Layout as Workspace..."),
          "a stage's: the workspaces only, unchecked: %s", qPrintable(stageTexts.join('|')));
    if (stageLayout) stageLayout->trigger();
    QApplication::processEvents();
    CHECK(before && controller.currentId() == "layout", "choosing one runs what comes first, then lays it out");
    CHECK(controller.switchTo(QStringLiteral("three"), &error), "back to Three: %s", qPrintable(error));

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
