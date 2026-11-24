// Workspaces (docs/WORKSPACES.md): the file format and its rules, the folders and
// which file wins, switching the window to one (reusing panels, one viewport per
// stage, views that follow the current stage), reading a layout back as one, the
// controller behind the title bar's button, and File > Open Stage with Options.
// Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/log_panel.h"
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

#include <AutoHideTab.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidgetTab.h>

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
#include <QWidgetAction>

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
    write(shipped.filePath("default.json"), workspaceText("default", "Default", "Scene", 10));
    write(shipped.filePath("lookdev.json"), workspaceText("lookdev", "Look Dev", "Shading", 40));
    write(shipped.filePath("four.json"), workspaceText("four", "Four View", "Scene", 20));
    write(shipped.filePath("loose.json"), workspaceText("loose", "Loose", "", 30));
    write(shipped.filePath("broken.json"), "{ not json");
    write(shipped.filePath("workspace.schema.json"), R"({"$schema": "x", "title": "not a workspace"})");
    // Your copy of Default from when its id was "layout": still a copy of it.
    write(user.filePath("layout.json"), workspaceText("layout", "My Layout", "Scene", 10));
    write(user.filePath("mine.json"), workspaceText("mine", "Mine", "", 900));

    WorkspaceLibrary library({user.path(), shipped.path(), false});
    CHECK(library.workspaces().size() == 5, "five, %d", int(library.workspaces().size()));
    const workspace::Definition* layout = library.find(QStringLiteral("default"));
    CHECK(layout && layout->name == "My Layout" && layout->source == "Your workspaces", "yours wins over the shipped one");
    CHECK(layout && layout->id == "default" && library.find(QStringLiteral("layout")) == layout,
          "read as Default's id, and found by the one it had");
    CHECK(library.isUserWorkspace(QStringLiteral("default")) && !library.isUserWorkspace(QStringLiteral("four")), "and is yours");
    const std::vector<workspace::Definition> hidden = library.shadowed(QStringLiteral("default"));
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
    // Yours without a group are under Personal; yours with one (a copy of a shipped one)
    // and another folder's without one stay where they were.
    CHECK(order.join(' ') == "Scene:My Layout/Four View :Loose Shading:Look Dev Personal:Mine",
          "groups by their lowest order, workspaces by order: %s", qPrintable(order.join(' ')));
    CHECK(library.find(QStringLiteral("mine"))->group.isEmpty(), "the heading isn't written into the workspace");

    // Saving over your copy writes the file it is in, not a default.json beside it.
    workspace::Definition copy = *library.find(QStringLiteral("default"));
    copy.name = QStringLiteral("My Default");
    QString saveError;
    CHECK(library.save(copy, &saveError) && !QFile::exists(user.filePath("default.json")) &&
              library.find(QStringLiteral("default"))->name == "My Default" &&
              library.shadowed(QStringLiteral("default")).size() == 1,
          "saved over layout.json: %s", qPrintable(saveError));

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
    for (const char* id : {"default", "side-by-side", "four-view", "viewport", "lookdev", "layering", "inspect", "scripting"}) {
        CHECK(real.find(QLatin1String(id)) != nullptr, "shipped: %s", id);
    }
    CHECK(real.problems().isEmpty(), "the shipped files all read: %s", qPrintable(real.problems().join(';')));
    // The one a first start gets is named Default. It is behind the user folder's
    // layout.json here.
    const std::vector<workspace::Definition> first = real.shadowed(QLatin1String(WorkspaceController::kDefaultId));
    CHECK(first.size() == 1 && first.front().name == "Default", "the default workspace's name: %s",
          first.empty() ? "none" : qPrintable(first.front().name));
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
    CHECK(app.window.logPanel()->isClosed(), "the Log hidden, as it says");
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
        // The Log, docked at the right, is a column of its own.
        if (columns->widget(i) != app.window.logPanel()->dockAreaWidget() && !columns->widget(i)->isHidden()) {
            sizes << columns->sizes()[i];
        }
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
            if (columns->widget(i) != app.window.logPanel()->dockAreaWidget() && !columns->widget(i)->isHidden()) {
                after << columns->sizes()[i];
            }
        }
        CHECK(after.size() == 4 && std::abs(after[0] - sizes[0]) <= 2 && std::abs(after[3] - sizes[3]) <= 2 &&
                  after[1] > sizes[1] && after[2] > sizes[2],
              "a wider window widens the viewports only: %d %d %d %d", after.value(0), after.value(1), after.value(2),
              after.value(3));
    }

    usdcc::ui::LogPanel* logPanel = app.window.logPanel();
    CHECK(!logPanel->isClosed() && !logPanel->isCollapsed() && logPanel->area() == Qt::RightDockWidgetArea,
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
    CHECK(logPanel->isClosed(), "the Log, not named, hidden");
    // Named by its title works too.
    std::optional<workspace::Definition> titled = workspace::fromJson(
        parse(R"({"format": "usdcc-workspace", "version": 1, "name": "T", "sidePanels": {"Log": "collapsed"},
                  "layout": {"tabs": [{"panel": "viewport"}]}})"),
        QStringLiteral("titled"));
    logPanel->setObjectName(QStringLiteral("logByAnotherName"));
    CHECK(titled && workspace::apply(*titled, app.context()).applied, "titled applied");
    CHECK(!logPanel->isClosed() && logPanel->isCollapsed(), "found by its title, collapsed");
    logPanel->setObjectName(QStringLiteral("Log"));
}

USDCC_TEST(workspace_controller_and_button) {
    App app;
    QTemporaryDir user;
    QTemporaryDir shipped;
    write(shipped.filePath("default.json"), workspaceText("default", "Default", "Scene", 10));
    write(shipped.filePath("three.json"), kThreeColumns);
    WorkspaceLibrary library({user.path(), shipped.path(), false});
    WorkspaceController controller(app.context(), &library);
    QToolButton* button = controller.createButton(&app.window);
    app.window.addTitleBarWidget(button);
    CHECK(controller.currentId() == "default" && button->text() == "Default", "Default before any is chosen: %s",
          qPrintable(button->text()));
    CHECK(button->icon().isNull() && button->toolButtonStyle() == Qt::ToolButtonTextOnly && button->menu(),
          "a push button: its name and the menu's arrow, no icon");

    CHECK(!controller.isChosen(), "none chosen yet");
    QString error;
    CHECK(controller.switchTo(QStringLiteral("three"), &error), "switched: %s", qPrintable(error));
    CHECK(controller.isChosen(), "chosen now");
    CHECK(controller.currentId() == "three" && button->text() == "Three", "the button says so");
    CHECK(usdcc::ui::appSettings().value(WorkspaceController::kCurrentKey).toString() == "three", "and it's remembered");
    CHECK(!controller.switchTo(QStringLiteral("nope"), &error) && !error.isEmpty(), "no such workspace");

    QMenu menu;
    controller.populateMenu(&menu);
    QStringList texts;
    for (QAction* action : menu.actions()) {
        if (!action->text().isEmpty()) texts << action->text();
    }
    CHECK(texts.contains("Default") && texts.contains("Three") && texts.contains("Save Current Layout as Workspace..."),
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
    QAction* stageDefault = stageWorkspaces.findChild<QAction*>(QStringLiteral("workspace:default"));
    QStringList stageTexts;
    for (QAction* action : stageWorkspaces.actions()) {
        if (!action->text().isEmpty()) stageTexts << action->text();
    }
    CHECK(stageThree && !stageThree->isChecked() && !stageTexts.contains("Save Current Layout as Workspace..."),
          "a stage's: the workspaces only, unchecked: %s", qPrintable(stageTexts.join('|')));
    if (stageDefault) stageDefault->trigger();
    QApplication::processEvents();
    CHECK(before && controller.currentId() == "default", "choosing one runs what comes first, then lays it out");
    CHECK(controller.switchTo(QStringLiteral("three"), &error), "back to Three: %s", qPrintable(error));

    // Saving the layout as a new workspace makes it the active one.
    const QString id = controller.saveCurrentLayout(QStringLiteral("My Three"), false, &error);
    CHECK(id == "my-three" && QFile::exists(user.filePath("my-three.json")) && controller.currentId() == id,
          "saved as yours: %s %s", qPrintable(id), qPrintable(error));
    // It is listed under a Personal heading, the last of the workspaces.
    QMenu withMine;
    controller.populateMenu(&withMine);
    QString headingAbove;
    for (QAction* action : withMine.actions()) {
        if (auto* widgetAction = qobject_cast<QWidgetAction*>(action)) {
            if (auto* label = qobject_cast<QLabel*>(widgetAction->defaultWidget())) headingAbove = label->text();
        }
        if (action->objectName() == "workspace:my-three") break;
    }
    CHECK(headingAbove == "PERSONAL", "yours under Personal: %s", qPrintable(headingAbove));
    // By the id it had while it was named Layout, as a script or a session may still say.
    CHECK(controller.switchTo(QStringLiteral("layout"), &error) && controller.currentId() == "default",
          "back to Default: %s", qPrintable(error));
    controller.setCurrentId(id);
    controller.setCurrentId(QStringLiteral("layout"));
    CHECK(controller.currentId() == "default", "a session's \"layout\" is Default: %s", qPrintable(controller.currentId()));
    CHECK(!usdcc::ui::appSettings().contains(QStringLiteral("Workspaces/arranged/my-three")),
          "how My Three was left isn't kept");
    CHECK(usdcc::core::Settings::instance().contains("Shortcuts/workspace.three"), "a shortcut setting per workspace");
}

// Switching lays a workspace out as its file says every time: what was changed in the
// window since, a panel closed or a divider dragged, isn't kept. And the arrangements an
// earlier version remembered are removed.
USDCC_TEST(workspace_switching_always_follows_the_file) {
    App app;
    ads::CDockManager* docks = app.window.dockManager();
    app.stages.createStage();
    {
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(QStringLiteral("Workspaces/arranged/three"), QByteArray(R"({"format": "usdcc-workspace"})"));
        // A profile from when Default's id was "layout".
        settings.setValue(QLatin1String(WorkspaceController::kCurrentKey), QStringLiteral("layout"));
        settings.setValue(QStringLiteral("Shortcuts/workspace.layout"), QStringLiteral("Ctrl+Alt+1"));
    }
    QTemporaryDir user;
    QTemporaryDir shipped;
    write(shipped.filePath("default.json"), workspaceText("default", "Default", "Scene", 10));
    write(shipped.filePath("three.json"), kThreeColumns);
    WorkspaceLibrary library({user.path(), shipped.path(), false});
    WorkspaceController controller(app.context(), &library);
    CHECK(!usdcc::ui::appSettings().contains(QStringLiteral("Workspaces/arranged/three")),
          "the arrangements an earlier version kept are gone");
    CHECK(controller.currentId() == "default" && controller.isChosen() &&
              usdcc::ui::appSettings().value(WorkspaceController::kCurrentKey).toString() == "default",
          "the profile's \"layout\" is Default: %s", qPrintable(controller.currentId()));
    CHECK(usdcc::ui::appSettings().value(QStringLiteral("Shortcuts/workspace.default")).toString() == "Ctrl+Alt+1" &&
              !usdcc::ui::appSettings().contains(QStringLiteral("Shortcuts/workspace.layout")),
          "and its shortcut goes with it");

    // The Attributes open, and the columns 1 : 3 : 2.
    auto check = [&](const char* when) {
        waitFor([] { return false; }, 200);
        ViewPanel* outliner = nullptr;
        ViewPanel* attributes = nullptr;
        for (ViewPanel* panel : openPanels(docks)) {
            if (typeOf(panel) == "outliner") outliner = panel;
            if (typeOf(panel) == "attributes") attributes = panel;
        }
        CHECK(outliner && attributes, "%s: the Outliner and the Attributes open", when);
        QSplitter* columns = outliner ? qobject_cast<QSplitter*>(outliner->dockAreaWidget()->parentWidget()) : nullptr;
        QList<int> sizes;
        for (int i = 0; columns && i < columns->count(); ++i) {
            if (!columns->widget(i)->isHidden()) sizes << columns->sizes()[i];
        }
        const double total = sizes.size() == 3 ? sizes[0] + sizes[1] + sizes[2] : 0.0;
        CHECK(total > 0 && std::abs(sizes[0] / total - 1.0 / 6.0) < 0.05 && std::abs(sizes[2] / total - 1.0 / 3.0) < 0.05,
              "%s: the file's shares: %s", when, qPrintable([&] {
                  QStringList list;
                  for (int size : sizes) list << QString::number(size);
                  return list.join(' ');
              }()));
        return columns;
    };
    // Closes the Attributes and drags the first divider far right.
    auto tweak = [&](QSplitter* columns) {
        if (auto* attributes = qobject_cast<ViewPanel*>(docks->findDockWidget(QStringLiteral("Attributes")))) {
            attributes->toggleView(false);
        }
        if (columns && columns->count() >= 2) {
            QList<int> sizes = columns->sizes();
            sizes[0] += sizes[1] / 2;
            sizes[1] -= sizes[1] / 2;
            columns->setSizes(sizes);
        }
        QApplication::processEvents();
    };

    QString error;
    CHECK(controller.switchTo(QStringLiteral("three"), &error), "Three: %s", qPrintable(error));
    tweak(check("switched to"));
    CHECK(controller.switchTo(QStringLiteral("default"), &error) && controller.switchTo(QStringLiteral("three"), &error),
          "away and back: %s", qPrintable(error));
    tweak(check("back again"));
    CHECK(controller.switchTo(QStringLiteral("three"), &error), "chosen again: %s", qPrintable(error));
    check("the active one chosen again");
    CHECK(!usdcc::ui::appSettings().contains(QStringLiteral("Workspaces/arranged/three")) &&
              !usdcc::ui::appSettings().contains(QStringLiteral("Workspaces/arranged/default")),
          "nothing remembered");
}

USDCC_TEST(workspace_view_panel_follows_the_current_stage) {
    App app;
    const StageRefPtr one = app.stages.createStage();
    const StageRefPtr two = app.stages.createStage();
    app.stages.setCurrentStage(one);
    auto* panel = new usdcc::usd::OutlinerViewPanel(&app.stages, two, app.undo, QStringLiteral("F"), &app.window);
    app.window.dockManager()->addDockWidget(ads::RightDockWidgetArea, panel);
    // Its stage dropdown: Follow Current Stage, a separator, then the stages.
    QComboBox* combo = nullptr;
    for (QComboBox* candidate : panel->findChildren<QComboBox*>()) {
        if (candidate->findData(static_cast<qlonglong>(one->cacheId())) >= 0) combo = candidate;
    }
    CHECK(combo && combo->count() == 4 && combo->itemText(0).startsWith(QStringLiteral("Follow Current Stage")) &&
              combo->findData(static_cast<qlonglong>(one->cacheId())) == 2 &&
              combo->findData(static_cast<qlonglong>(two->cacheId())) == 3,
          "the dropdown: Follow Current Stage, then the stages");
    if (!combo) return;
    CHECK(combo->currentIndex() == 3, "it shows its own stage");

    combo->setCurrentIndex(0);
    CHECK(panel->followsCurrentStage() && panel->currentStage() == one, "picking Follow Current Stage shows the current one");
    CHECK(combo->currentText() == QStringLiteral("Follow Current Stage (%1)").arg(combo->itemText(2)),
          "and the entry names it: %s", qPrintable(combo->currentText()));
    app.stages.setCurrentStage(two);
    CHECK(panel->currentStage() == two && combo->currentIndex() == 0, "it switches with it, still following");
    CHECK(combo->currentText() == QStringLiteral("Follow Current Stage (%1)").arg(combo->itemText(3)),
          "the entry names the new one: %s", qPrintable(combo->currentText()));

    combo->setCurrentIndex(2);
    CHECK(panel->currentStage() == one && !panel->followsCurrentStage() && combo->currentIndex() == 2,
          "picking a stage stops it");
    app.stages.setCurrentStage(one);
    app.stages.setCurrentStage(two);
    CHECK(panel->currentStage() == one, "and it stays");

    panel->setFollowsCurrentStage(true);
    CHECK(combo->currentIndex() == 0 && panel->currentStage() == two, "turned on in code, the dropdown says so");
}

USDCC_TEST(workspace_follow_current_stage_setting) {
    App app;
    auto& settings = usdcc::core::Settings::instance();
    const std::string outlinerKey = ViewPanelRegistry::followSettingKey(QStringLiteral("outliner"));
    for (const char* type : {"viewport", "outliner", "attributes"}) {
        const usdcc::core::SettingDefinition* definition =
            settings.definition(ViewPanelRegistry::followSettingKey(QString::fromLatin1(type)));
        CHECK(definition && definition->category == "Appearance" && definition->group == "Follow Current Stage" &&
                  definition->type == usdcc::core::SettingType::Bool && !settings.get<bool>(definition->key),
              "a check box per registered type, off: %s", type);
    }
    const StageRefPtr one = app.stages.createStage();
    const StageRefPtr two = app.stages.createStage();
    app.stages.setCurrentStage(one);

    ViewPanel* first = app.registry.open(QStringLiteral("outliner"), two, app.window.dockManager(), &app.window);
    ViewPanel* attributes = app.registry.open(QStringLiteral("attributes"), two, app.window.dockManager(), &app.window);
    CHECK(first && attributes && !first->followsCurrentStage() && first->currentStage() == two, "off: as before");
    if (!first || !attributes) return;

    settings.setValue(outlinerKey, true);
    CHECK(first->followsCurrentStage() && first->currentStage() == one, "on: every open one of the type follows");
    CHECK(!attributes->followsCurrentStage() && attributes->currentStage() == two, "the other types don't");
    ViewPanel* second = app.registry.open(QStringLiteral("outliner"), two, app.window.dockManager(), &app.window);
    CHECK(second && second->followsCurrentStage() && second->currentStage() == one, "and a new one does");
    ViewPanel* copy = attributes->duplicateBeside();
    CHECK(copy && !copy->followsCurrentStage(), "a copy of another type doesn't");
    app.stages.setCurrentStage(two);
    CHECK(first->currentStage() == two && second && second->currentStage() == two, "they switch with it");

    settings.setValue(outlinerKey, false);
    CHECK(!first->followsCurrentStage() && second && !second->followsCurrentStage() && first->currentStage() == two,
          "off again: they stop, on the stage they show");

    settings.setValue(ViewPanelRegistry::followSettingKey(QStringLiteral("viewport")), true);
    ViewPanel* viewport = app.registry.create(QStringLiteral("viewport"), one, app.window.dockManager(), &app.window);
    CHECK(viewport && viewport->followsCurrentStage() && viewport->currentStage() == two, "a new viewport follows");
    delete viewport;
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
    write(shipped.filePath("default.json"), workspaceText("default", "Default", "Scene", 10));
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

// A viewport closed before a switch back to a workspace comes back with its tab: ADS 4.5
// moved it into place with the tab hidden and then called it open.
USDCC_TEST(workspace_brings_a_closed_viewport_back_with_its_tab) {
    App app;
    ads::CDockManager* docks = app.window.dockManager();
    const StageRefPtr stage = app.stages.createStage();
    QString error;
    const std::optional<workspace::Definition> three = workspace::fromJson(parse(kThreeColumns), QStringLiteral("file"), &error);
    CHECK(three && workspace::apply(*three, app.context()).applied, "applied");
    QApplication::processEvents();

    ViewPanel* viewport = nullptr;
    for (ViewPanel* panel : openPanels(docks)) {
        if (typeOf(panel) == "viewport" && panel->currentStage() == stage && !panel->followsCurrentStage()) viewport = panel;
    }
    CHECK(viewport, "the stage's viewport is open");
    if (!viewport) return;
    viewport->toggleView(false);  // Its tab's close button
    QApplication::processEvents();
    CHECK(viewport->isClosed() && viewport->tabWidget()->isHidden(), "closed, with its tab hidden");

    CHECK(workspace::apply(*three, app.context()).applied, "applied again");
    QApplication::processEvents();
    CHECK(!viewport->isClosed(), "the closed viewport isn't used and opened again");
    CHECK(!viewport->tabWidget()->isHidden(), "the viewport is back without its tab");
    CHECK(viewport->toggleViewAction()->isChecked(), "its View menu entry still says it's closed");
}

// The workspaces usdcc ships, each applied twice over the others (docs/WORKSPACES.md §3.2):
// the side panels where it says, and the layout's tree as the file has it, whatever docked
// side panels surround it.
USDCC_TEST(workspace_shipped_files_place_the_side_panels) {
    App app;
    // The types the files name that this test app lacks: any panel will do.
    for (const char* type : {"layers", "materialEditor", "usda", "schemas", "dependencies"}) {
        app.registry.add(QString::fromLatin1(type), QString::fromLatin1(type),
                         [&app](const StageRefPtr& stage, const QString& title, QWidget* parent) -> ViewPanel* {
                             return new usdcc::usd::OutlinerViewPanel(&app.stages, stage, app.undo, title, parent);
                         });
    }
    app.stages.createStage();
    QApplication::processEvents();
    usdcc::ui::LogPanel* log = app.window.logPanel();
    for (const char* name : {"Script Editor", "pluginManagerPanel", "Peers"}) {
        auto* panel = new usdcc::ui::SidePanel(QString::fromLatin1(name));
        panel->setWidget(new QLabel(QString::fromLatin1(name)));
        app.window.addSidePanel(panel, Qt::RightDockWidgetArea);
    }
    const QDir folder(QStringLiteral(USDCC_SOURCE_DIR "/resources/workspaces"));
    QStringList files = folder.entryList({QStringLiteral("*.json")}, QDir::Files);
    files.removeAll(QStringLiteral("workspace.schema.json"));
    CHECK(files.size() == 8, "eight shipped workspaces: %d", int(files.size()));
    for (int round = 0; round < 2; ++round) {
        for (const QString& file : files) {
            const QByteArray name = file.toUtf8();
            QFile json(folder.filePath(file));
            CHECK(json.open(QIODevice::ReadOnly), "%s opens", name.constData());
            std::optional<workspace::Definition> definition = workspace::fromJson(parse(json.readAll()), file);
            CHECK(definition.has_value(), "%s reads", name.constData());
            if (!definition) continue;
            const workspace::ApplyResult result = workspace::apply(*definition, app.context());
            CHECK(result.applied && result.missingTypes.isEmpty(), "%s applies (missing: %s)", name.constData(),
                  qPrintable(result.missingTypes.join(' ')));
            waitFor([] { return false; }, 100);

            const workspace::Definition captured = workspace::capture(&app.window, QStringLiteral("c"), QStringLiteral("C"), true);
            for (usdcc::ui::SidePanel* panel : app.window.sidePanels()) {
                const QByteArray who = panel->objectName().toUtf8();
                const auto entry = definition->sidePanels.find(panel->objectName());
                const bool named = entry != definition->sidePanels.end();
                const QString wanted = named ? entry->state : QStringLiteral("hidden");
                const workspace::SidePanelEntry got = captured.sidePanels.value(panel->objectName());
                CHECK(got.state == wanted, "%s: %s %s, not %s", name.constData(), who.constData(), qPrintable(wanted),
                      qPrintable(got.state));
                CHECK(panel->isVisible() == (wanted == QLatin1String("shown")), "%s: %s in view when shown only",
                      name.constData(), who.constData());
                if (!named) continue;
                if (!entry->area.isEmpty()) {
                    CHECK(got.area == entry->area, "%s: %s at the %s, not %s", name.constData(), who.constData(),
                          qPrintable(entry->area), qPrintable(got.area));
                }
                if (entry->size > 0.0) {
                    CHECK(std::abs(got.size - entry->size) < 0.03, "%s: %s with its share %.2f: %.3f", name.constData(),
                          who.constData(), entry->size, got.size);
                }
            }

            // The same tree, and the same shares where no maxWidth or maxHeight says otherwise.
            std::function<void(const workspace::Node&, const workspace::Node&)> same =
                [&](const workspace::Node& want, const workspace::Node& have) {
                    CHECK(want.isSplit() == have.isSplit(), "%s: a split where it says", name.constData());
                    if (want.isSplit() != have.isSplit()) return;
                    if (!want.isSplit()) {
                        CHECK(want.tabs.size() == have.tabs.size(), "%s: %d tabs, not %d", name.constData(),
                              int(want.tabs.size()), int(have.tabs.size()));
                        for (std::size_t i = 0; i < std::min(want.tabs.size(), have.tabs.size()); ++i) {
                            CHECK(want.tabs[i].panel == have.tabs[i].panel, "%s: %s in place", name.constData(),
                                  qPrintable(want.tabs[i].panel));
                        }
                        return;
                    }
                    CHECK(want.orientation == have.orientation && want.items.size() == have.items.size(),
                          "%s: split the same way: %d, not %d", name.constData(), int(want.items.size()),
                          int(have.items.size()));
                    for (std::size_t i = 0; i < std::min(want.items.size(), have.items.size()); ++i) {
                        same(want.items[i], have.items[i]);
                    }
                };
            same(definition->layout, captured.layout);
        }
    }

    // Saved and restored with the window's layout: docked by the last ones, then collapsed.
    for (const bool collapsed : {false, true}) {
        collapsed ? log->collapseAt(Qt::LeftDockWidgetArea) : log->dockAt(Qt::BottomDockWidgetArea);
        QApplication::processEvents();
        const QJsonObject state = app.window.layoutState();
        log->toggleView(false);
        app.window.restoreLayoutState(state, false);
        QApplication::processEvents();
        CHECK(!log->isClosed() && log->isCollapsed() == collapsed &&
                  log->area() == (collapsed ? Qt::LeftDockWidgetArea : Qt::BottomDockWidgetArea),
              "%s again after a restore", collapsed ? "collapsed" : "docked");
    }
    // Slid out, and tucked away again by collapsing.
    log->reveal();
    QApplication::processEvents();
    CHECK(log->isVisible() && log->isCollapsed(), "a collapsed Log slides out");
    log->setCollapsed(true);
    QApplication::processEvents();
    CHECK(!log->isVisible() && !log->isClosed(), "and goes back to its tab");
}

// A layout saved before the Log was in the dock manager doesn't name it: it comes as a tab at the
// window's edge, not floating and not lost.
USDCC_TEST(workspace_layout_without_the_log) {
    App app;
    usdcc::ui::LogPanel* log = app.window.logPanel();
    QJsonObject state = app.window.layoutState();
    QString layout = state.value(QStringLiteral("dockLayout")).toString();
    layout.replace(QStringLiteral("\"Log\""), QStringLiteral("\"Gone\""));
    state.insert(QStringLiteral("dockLayout"), layout);
    for (const bool docked : {false, true}) {
        if (docked) log->dockAt(Qt::BottomDockWidgetArea);
        app.window.restoreLayoutState(state, false);
        QApplication::processEvents();
        CHECK(!log->isClosed() && !log->isFloating() && log->isCollapsed() && log->area() == Qt::BottomDockWidgetArea,
              "a tab at the bottom: closed %d floating %d collapsed %d", log->isClosed(), log->isFloating(),
              log->isCollapsed());
        CHECK(log->sideTabWidget() && log->sideTabWidget()->isVisible(), "which shows");
    }
}

// View panels collapsed at an edge: a workspace has them there, one without takes them back
// into its layout or closes them, and the layout saved has them.
USDCC_TEST(workspace_collapsed_view_panels) {
    App app;
    ads::CDockManager* docks = app.window.dockManager();
    const StageRefPtr stage = app.stages.createStage();
    const auto apply = [&app](const char* json) {
        std::optional<workspace::Definition> definition = workspace::fromJson(parse(json), QStringLiteral("w"));
        CHECK(definition.has_value(), "reads");
        if (!definition) return;
        CHECK(workspace::apply(*definition, app.context()).applied, "applies");
        waitFor([] { return false; }, 100);
    };
    const char* kCollapsed = R"({"format": "usdcc-workspace", "version": 1, "name": "C",
        "collapsed": [{"panel": "outliner", "area": "left"}, {"panel": "attributes", "area": "right"}],
        "layout": {"tabs": [{"panel": "viewport"}]}})";
    const char* kDocked = R"({"format": "usdcc-workspace", "version": 1, "name": "D",
        "layout": {"split": "horizontal", "items": [{"tabs": [{"panel": "outliner"}]}, {"tabs": [{"panel": "viewport"}]}]}})";
    auto* outliner = qobject_cast<ViewPanel*>(docks->findDockWidget(QStringLiteral("Outliner")));
    auto* attributes = qobject_cast<ViewPanel*>(docks->findDockWidget(QStringLiteral("Attributes")));
    for (int round = 0; round < 2; ++round) {
        apply(kCollapsed);
        CHECK(docks->findDockWidget(QStringLiteral("Outliner")) == outliner, "the same Outliner");
        CHECK(outliner->collapsedArea() == Qt::LeftDockWidgetArea && attributes->collapsedArea() == Qt::RightDockWidgetArea,
              "both collapsed, each at its edge");
        CHECK(outliner->sideTabWidget()->isVisible() && !outliner->isVisible(), "as a tab");
        CHECK(outliner->currentStage() == stage, "showing the current stage");
        const workspace::Definition captured = workspace::capture(&app.window, QStringLiteral("c"), QStringLiteral("C"), false);
        CHECK(captured.collapsed.size() == 2 && !captured.layout.isSplit(), "read back collapsed, not in the layout: %d",
              int(captured.collapsed.size()));
        const auto again = workspace::fromJson(captured.toJson(), QString());
        CHECK(again && again->collapsed.size() == 2 && again->collapsed[0].area == captured.collapsed[0].area, "and written");

        apply(kDocked);
        CHECK(!outliner->isCollapsed() && outliner->isVisible(), "docked where the next workspace has it");
        CHECK(attributes->isClosed() && !attributes->isCollapsed(), "the other closed, off the bar");
    }

    // Saved and restored, and closing its stage's panels.
    apply(kCollapsed);
    const QJsonObject state = app.window.layoutState();
    apply(kDocked);
    app.window.restoreLayoutState(state, false);
    QApplication::processEvents();
    CHECK(outliner->collapsedArea() == Qt::LeftDockWidgetArea && !outliner->isClosed(), "a restored layout collapses it again");
    outliner->raise();
    waitFor([&] { return outliner->isVisible(); }, 500);
    CHECK(outliner->isVisible(), "raise() slides it out");
    ViewPanel* copy = outliner->duplicateBeside();
    waitFor([] { return false; }, 50);
    CHECK(copy->collapsedArea() == Qt::LeftDockWidgetArea && copy->isVisible(), "a copy is collapsed beside it, slid out");
    ViewPanel* opened = app.registry.open(QStringLiteral("outliner"), stage, docks, &app.window);
    waitFor([] { return false; }, 50);
    CHECK(opened && !opened->isCollapsed() && opened->isVisible(), "one opened from the Stages menu is docked");
    copy->deleteDockWidget();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    app.stages.closeStage(stage);
    CHECK(!outliner->currentStage() && outliner->isCollapsed(), "closing the stage empties it where it is");

    for (ViewPanel* panel : openPanels(docks)) {
        CHECK(panel->features().testFlag(ads::CDockWidget::DockWidgetPinnable) != (typeOf(panel) == "viewport"),
              "all but viewports can be collapsed: %s", qPrintable(typeOf(panel)));
    }
    QString why;
    CHECK(!workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1, "id": "v", "name": "V", "collapsed": [{"panel": "viewport"}],
              "layout": {"tabs": [{"panel": "viewport"}]}})"), QString(), &why) && why.contains("viewport"),
          "a collapsed viewport is refused: %s", qPrintable(why));
    CHECK(!workspace::fromJson(parse(R"({"format": "usdcc-workspace", "version": 1, "id": "v", "name": "V", "collapsed": [{"panel": "outliner",
              "area": "middle"}], "layout": {"tabs": [{"panel": "viewport"}]}})"), QString(), &why),
          "and so is an unknown edge");
}
