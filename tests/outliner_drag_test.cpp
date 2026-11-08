// Dragging prims in the Outliner (docs/OUTLINER_REPARENT.md §3.9): what can be dropped
// where and why not, the request a drop makes, the question before changing other layers,
// and the selection and expanded rows following. Runs in usdcc_ui_tests (offscreen). A
// real mouse drag can't run here: Qt's drag has its own blocking loop, so these go in
// through what the view calls.
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QUndoStack>

#include <memory>

using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::UsdPrimTreeModel;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

struct Scene {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini"))};
    StageManager manager;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    StageRefPtr stage;
    Scene() {
        QStandardPaths::setTestModeEnabled(true);
        // The test config file is shared: "Don't ask again" mustn't carry over.
        usdcc::ui::appSettings().remove(QStringLiteral("Outliner/askBeforeMovingAcrossLayers"));
        stage = manager.createStage();
    }
    ~Scene() { QStandardPaths::setTestModeEnabled(false); }
    UsdStageRefPtr usd() const { return stage->usdStage(); }
    void define(const char* path, const char* type = "Xform") { usd()->DefinePrim(SdfPath(path), TfToken(type)); }
    bool has(const char* path) const { return static_cast<bool>(usd()->GetPrimAtPath(SdfPath(path))); }
};

// Drops these rows on `parent`, as the view does, through the panel's own signals.
bool drop(UsdPrimTreeModel* model, const std::vector<const char*>& paths, const char* parent, int row = -1) {
    QModelIndexList indexes;
    for (const char* path : paths) indexes << model->fetchPath(SdfPath(path));
    std::unique_ptr<QMimeData> data(model->mimeData(indexes));
    const QModelIndex on = std::string(parent) == "/" ? QModelIndex() : model->fetchPath(SdfPath(parent));
    return model->dropMimeData(data.get(), Qt::MoveAction, row, 0, on);
}

bool canDrop(UsdPrimTreeModel* model, const char* path, const char* parent) {
    std::unique_ptr<QMimeData> data(model->mimeData({model->fetchPath(SdfPath(path))}));
    const QModelIndex on = std::string(parent) == "/" ? QModelIndex() : model->fetchPath(SdfPath(parent));
    return model->canDropMimeData(data.get(), Qt::MoveAction, -1, 0, on);
}

void answerQuestion(QMessageBox::StandardButton answer, bool dontAskAgain, bool* asked, QString* text) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [=]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        poll->stop();
        poll->deleteLater();
        *asked = true;
        *text = box->text();
        if (dontAskAgain) box->checkBox()->setChecked(true);
        box->button(answer)->click();
    });
    poll->start();
}
}  // namespace

USDCC_TEST(outliner_drag_model) {
    Scene s;
    for (const char* path : {"/A", "/A/Kid", "/B", "/Off"}) s.define(path);
    s.usd()->GetPrimAtPath(SdfPath("/Off")).SetActive(false);
    UsdPrimTreeModel model;
    model.setStage(s.usd());
    const QModelIndex a = model.fetchPath(SdfPath("/A"));
    CHECK(model.flags(a) & Qt::ItemIsDragEnabled, "a row can be dragged");
    CHECK(model.flags(a) & Qt::ItemIsDropEnabled, "and dropped on");
    CHECK(model.flags(QModelIndex()) & Qt::ItemIsDropEnabled, "the empty space is the root");
    CHECK(model.supportedDropActions() == Qt::MoveAction, "moving, never copying");

    QString hint = QStringLiteral("unset");
    QObject::connect(&model, &UsdPrimTreeModel::dropHint, &model, [&](const QString& why) { hint = why; });
    CHECK(canDrop(&model, "/A/Kid", "/B") && hint.isEmpty(), "another parent");
    CHECK(canDrop(&model, "/A/Kid", "/"), "the root");
    CHECK(!canDrop(&model, "/A", "/A/Kid") && hint.contains(QStringLiteral("into itself")), "its own descendant, saying why");
    CHECK(!canDrop(&model, "/A/Kid", "/Off") && hint.contains(QStringLiteral("inactive")), "an inactive prim, saying why");

    QMimeData other;
    other.setText(QStringLiteral("/A"));
    CHECK(!model.canDropMimeData(&other, Qt::MoveAction, -1, 0, a), "not something from elsewhere");

    model.setReadOnly(true);
    CHECK(!(model.flags(a) & Qt::ItemIsDragEnabled) && !(model.flags(QModelIndex()) & Qt::ItemIsDropEnabled), "read-only: no dragging");
    std::unique_ptr<QMimeData> data(model.mimeData({a}));
    CHECK(!model.canDropMimeData(data.get(), Qt::MoveAction, -1, 0, QModelIndex()), "nor dropping");
}

USDCC_TEST(outliner_drag_reparents_and_reorders) {
    Scene s;
    for (const char* path : {"/A", "/A/Kid", "/A/Kid/Deep", "/B", "/B/One", "/B/Two"}) s.define(path);
    s.manager.setCurrentStage(s.stage);
    OutlinerViewPanel panel(&s.manager, s.stage, s.undo, QStringLiteral("Outliner"));
    panel.resize(300, 400);
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeView*>();
    auto* proxy = static_cast<QSortFilterProxyModel*>(tree->model());  // the search and type filter
    auto* model = static_cast<UsdPrimTreeModel*>(proxy->sourceModel());
    [[maybe_unused]] const auto view = [proxy](const QModelIndex& index) { return proxy->mapFromSource(index); };
    CHECK(tree->dragEnabled() && tree->acceptDrops() && tree->dragDropMode() == QAbstractItemView::InternalMove, "the tree drags");

    s.manager.setSelectedPaths(s.stage, {SdfPath("/A/Kid")});
    tree->expand(view(model->fetchPath(SdfPath("/A/Kid"))));
    tree->collapse(view(model->fetchPath(SdfPath("/B"))));
    settle();

    CHECK(drop(model, {"/A/Kid"}, "/B"), "the drop's accepted");
    CHECK(!s.has("/B/Kid"), "and nothing's changed yet, inside the view's drop");
    settle();
    CHECK(s.has("/B/Kid/Deep") && !s.has("/A/Kid"), "then it moves");
    CHECK(s.manager.selectedPaths(s.stage) == std::vector<SdfPath>{SdfPath("/B/Kid")}, "the selection follows");
    QModelIndex moved = model->fetchPath(SdfPath("/B/Kid"));
    CHECK(moved.isValid() && tree->isExpanded(view(moved)), "it stays expanded");
    CHECK(tree->isExpanded(view(model->fetchPath(SdfPath("/B")))), "and shows, under a parent that was collapsed");
    CHECK(tree->selectionModel()->isSelected(view(moved)), "selected in the tree");

    s.undo->stack(s.stage)->undo();
    settle();
    CHECK(s.has("/A/Kid") && !s.has("/B/Kid"), "undo");
    CHECK(s.manager.selectedPaths(s.stage) == std::vector<SdfPath>{SdfPath("/A/Kid")}, "the selection goes back too");
    CHECK(tree->isExpanded(view(model->fetchPath(SdfPath("/A/Kid")))), "still expanded");

    // Between rows: the row is where among the parent's children.
    CHECK(drop(model, {"/B/Two"}, "/B", 0), "between rows");
    settle();
    const auto order = s.usd()->GetPrimAtPath(SdfPath("/B")).GetChildren();
    CHECK((*order.begin()).GetName() == TfToken("Two"), "reorders");

    CHECK(drop(model, {"/A/Kid"}, "/B", 1), "into a position");
    settle();
    std::vector<std::string> names;
    for (const UsdPrim& child : s.usd()->GetPrimAtPath(SdfPath("/B")).GetChildren()) names.push_back(child.GetName());
    CHECK(names == std::vector<std::string>({"Two", "Kid", "One"}), "in another parent");

    CHECK(drop(model, {"/B/Kid"}, "/"), "onto the empty space");
    settle();
    CHECK(s.has("/Kid"), "to the root");
}

USDCC_TEST(outliner_drag_shows_why_not) {
    Scene s;
    for (const char* path : {"/A", "/A/Kid"}) s.define(path);
    OutlinerViewPanel panel(&s.manager, s.stage, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeView*>();
    auto* proxy = static_cast<QSortFilterProxyModel*>(tree->model());  // the search and type filter
    auto* model = static_cast<UsdPrimTreeModel*>(proxy->sourceModel());
    [[maybe_unused]] const auto view = [proxy](const QModelIndex& index) { return proxy->mapFromSource(index); };
    QLabel* note = nullptr;
    for (auto* label : panel.findChildren<QLabel*>(QStringLiteral("propertyNote"))) note = label;
    CHECK(note && !note->isVisible(), "no note at first");
    if (!note) return;
    canDrop(model, "/A", "/A/Kid");
    CHECK(note->isVisible() && note->text().contains(QStringLiteral("into itself")), "a refused hover says why");
    canDrop(model, "/A/Kid", "/");
    CHECK(!note->isVisible(), "an allowed one hides it");
    canDrop(model, "/A", "/A/Kid");
    QEvent leave(QEvent::DragLeave);
    QApplication::sendEvent(tree->viewport(), &leave);
    CHECK(!note->isVisible(), "leaving hides it");
}

USDCC_TEST(outliner_drag_asks_before_changing_other_layers) {
    Scene s;
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    s.usd()->GetRootLayer()->GetSubLayerPaths().push_back(weak->GetIdentifier());
    s.usd()->SetEditTarget(weak);
    s.define("/G");
    s.define("/G/K", "Cube");
    s.define("/G/L", "Cube");
    s.define("/G/M", "Cube");
    s.usd()->SetEditTarget(s.usd()->GetRootLayer());
    s.define("/N");
    s.define("/Mine");
    OutlinerViewPanel panel(&s.manager, s.stage, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    auto* model = static_cast<UsdPrimTreeModel*>(static_cast<QSortFilterProxyModel*>(panel.findChild<QTreeView*>()->model())->sourceModel());

    bool asked = false;
    QString text;
    answerQuestion(QMessageBox::Cancel, false, &asked, &text);
    drop(model, {"/G/K"}, "/N");
    settle();
    CHECK(asked && text.contains(QString::fromStdString(weak->GetDisplayName())), "asks, naming the layer");
    CHECK(s.has("/G/K"), "and Cancel leaves it");

    asked = false;
    answerQuestion(QMessageBox::Yes, false, &asked, &text);
    drop(model, {"/G/K"}, "/N");
    settle();
    CHECK(asked && s.has("/N/K"), "Yes moves it");

    // A prim only in the edit target has nothing to ask.
    QTimer* poll = new QTimer(qApp);
    bool asked2 = false;
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [&asked2]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            asked2 = true;
            box->button(QMessageBox::Cancel)->click();
        }
    });
    poll->start();
    drop(model, {"/Mine"}, "/N");
    settle();
    delete poll;
    CHECK(!asked2 && s.has("/N/Mine"), "a prim only here isn't asked about");

    // Don't ask again.
    asked = false;
    answerQuestion(QMessageBox::Yes, true, &asked, &text);
    drop(model, {"/G/L"}, "/N");
    settle();
    CHECK(asked && s.has("/N/L"), "asked once more");
    panel.performDrop({SdfPath("/G/M")}, SdfPath("/N"), -1, true);
    CHECK(s.has("/N/M"), "and then not again");
}

USDCC_TEST(outliner_rename_follows_on_undo) {
    Scene s;
    s.define("/A");
    s.define("/A/Kid");
    OutlinerViewPanel panel(&s.manager, s.stage, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeView*>();
    auto* proxy = static_cast<QSortFilterProxyModel*>(tree->model());  // the search and type filter
    auto* model = static_cast<UsdPrimTreeModel*>(proxy->sourceModel());
    [[maybe_unused]] const auto view = [proxy](const QModelIndex& index) { return proxy->mapFromSource(index); };
    s.manager.setSelectedPaths(s.stage, {SdfPath("/A")});
    tree->expand(view(model->fetchPath(SdfPath("/A"))));
    settle();
    CHECK(model->setData(model->fetchPath(SdfPath("/A")), QStringLiteral("Z"), Qt::EditRole), "renames");
    settle();
    CHECK(s.manager.selectedPaths(s.stage) == std::vector<SdfPath>{SdfPath("/Z")}, "the selection follows");
    CHECK(tree->isExpanded(view(model->fetchPath(SdfPath("/Z")))), "stays expanded");
    s.undo->stack(s.stage)->undo();
    settle();
    CHECK(s.manager.selectedPaths(s.stage) == std::vector<SdfPath>{SdfPath("/A")}, "undo: the selection comes back");
    CHECK(tree->isExpanded(view(model->fetchPath(SdfPath("/A")))), "still expanded");
}
