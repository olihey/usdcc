#include "usdcc/usd/outliner_view_panel.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/outliner_item_delegate.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

namespace {
constexpr auto kExpandToSelectionKey = "Outliner/expandToSelection";
}  // namespace

OutlinerViewPanel::OutlinerViewPanel(StageManager* stageManager, StageRefPtr stage,
                                      std::shared_ptr<UndoManager> undoManager, const QString& title,
                                      QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);

    addPanelControls(toolbarLayout);

    m_expandToSelection = new QAction(tr("Expand to Selection"), this);
    m_expandToSelection->setToolTip(
        tr("Expand the tree down to the selected prims whenever the selection changes, wherever it was made"));
    m_expandToSelection->setCheckable(true);
    m_expandToSelection->setChecked(usdcc::ui::appSettings().value(kExpandToSelectionKey, true).toBool());

    m_model = new UsdPrimTreeModel(this);
    m_treeView = new QTreeView(container);
    m_treeView->setModel(m_model);
    // Prim icons, and the active checkbox at the row's right edge.
    m_treeView->setItemDelegate(new OutlinerItemDelegate(m_treeView));
    usdcc::ui::scaled::iconSize(m_treeView, 16, 16);
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    layout->addWidget(toolbar);
    layout->addWidget(m_treeView, 1);

    setContentWidget(container);

    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &OutlinerViewPanel::onTreeSelectionChanged);
    connect(m_model, &UsdPrimTreeModel::primRenamed, this, &OutlinerViewPanel::onPrimRenamed);
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &OutlinerViewPanel::saveViewState);
    connect(m_model, &QAbstractItemModel::modelReset, this, &OutlinerViewPanel::restoreViewState);
    connect(m_expandToSelection, &QAction::toggled, this, [this](bool on) {
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kExpandToSelectionKey, on);
        if (on) {
            // this->: the constructor parameter `stageManager` shadows the accessor here.
            expandToSelection(this->stageManager()->selectedPaths(currentStage()));
        }
    });

    m_selectionChangedConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr changedStage, std::vector<PXR_NS::SdfPath> paths) {
            onStageSelectionChanged(changedStage, std::move(paths));
        });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* OutlinerViewPanel::duplicate(QWidget* parent) const {
    return new OutlinerViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

void OutlinerViewPanel::populateContextMenu(QMenu* menu) {
    usdcc::ui::ViewPanel::populateContextMenu(menu);
    menu->addAction(tr("Move Up"), this, [this]() { moveSelectedPrim(-1); });
    menu->addAction(tr("Move Down"), this, [this]() { moveSelectedPrim(1); });
}

void OutlinerViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    menu->addAction(m_expandToSelection);
}

void OutlinerViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_keepViewState = false;  // the expanded paths belong to the previous stage
    m_model->setStage(stage ? stage->usdStage() : PXR_NS::UsdStageRefPtr());
    m_keepViewState = true;
    // Edits, and undo/redo of them, arrive as change notices; only the ones
    // that touch the hierarchy rebuild the tree.
    m_stageChangedConnection = {};
    if (stage) {
        m_stageChangedConnection =
            stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
    }
    onStageSelectionChanged(stage, stageManager()->selectedPaths(stage));
}

void OutlinerViewPanel::onStageObjectsChanged(const StageChanges& changes) {
    const bool hierarchyChanged =
        std::any_of(changes.resyncedPaths.begin(), changes.resyncedPaths.end(), [](const PXR_NS::SdfPath& path) {
            return path.IsPrimPath() || path.IsAbsoluteRootPath();  // not a property's resync
        });
    if (!hierarchyChanged || m_refreshQueued) {
        return;
    }
    m_refreshQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_refreshQueued = false;
        m_model->refresh();
    });
}

void OutlinerViewPanel::saveViewState() {
    m_expandedPaths.clear();
    m_scrollPosition = m_treeView->verticalScrollBar()->value();
    if (!m_keepViewState) {
        return;
    }
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < m_model->rowCount(parent); ++row) {
            const QModelIndex index = m_model->index(row, 0, parent);
            if (m_treeView->isExpanded(index)) {
                m_expandedPaths.insert(m_model->pathForIndex(index));
                pending.push_back(index);  // collapsed subtrees keep their children's state hidden anyway
            }
        }
    }
}

void OutlinerViewPanel::restoreViewState() {
    if (!m_keepViewState) {
        return;
    }
    for (const PXR_NS::SdfPath& path : m_expandedPaths) {
        const QModelIndex index = m_model->fetchPath(path);  // loads instance proxies as needed
        if (index.isValid()) {
            m_treeView->setExpanded(index, true);
        }
    }
    onStageSelectionChanged(currentStage(), stageManager()->selectedPaths(currentStage()));
    // After the view has laid out the restored rows.
    const int scrollPosition = m_scrollPosition;
    QTimer::singleShot(0, this, [this, scrollPosition]() { m_treeView->verticalScrollBar()->setValue(scrollPosition); });
}

void OutlinerViewPanel::onTreeSelectionChanged() {
    if (m_updatingSelection || !currentStage()) {
        return;
    }
    std::vector<PXR_NS::SdfPath> paths;
    for (const QModelIndex& index : m_treeView->selectionModel()->selectedIndexes()) {
        paths.push_back(m_model->pathForIndex(index));
    }
    stageManager()->setSelectedPaths(currentStage(), paths);
}

void OutlinerViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage != currentStage()) {
        return;
    }
    m_updatingSelection = true;
    QItemSelection selection;
    for (const auto& path : paths) {
        // A prim inside an instance (picked in a viewport) is an instance
        // proxy, which the tree loads on demand.
        const QModelIndex index = m_model->fetchPath(path);
        if (index.isValid()) {
            selection.select(index, index);
        }
    }
    m_treeView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    m_updatingSelection = false;
    m_model->setSelectedPaths(paths);
    if (m_expandToSelection->isChecked()) {
        expandToSelection(paths);
    }
}

void OutlinerViewPanel::expandToSelection(const std::vector<PXR_NS::SdfPath>& paths) {
    QModelIndex first;
    for (const PXR_NS::SdfPath& path : paths) {
        const QModelIndex index = m_model->fetchPath(path);
        if (!index.isValid()) {
            continue;
        }
        for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
            m_treeView->expand(parent);
        }
        if (!first.isValid()) {
            first = index;
        }
    }
    if (first.isValid()) {
        m_treeView->scrollTo(first);  // EnsureVisible: no jump if it already shows
    }
}

void OutlinerViewPanel::onPrimRenamed(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath) {
    auto paths = stageManager()->selectedPaths(currentStage());
    bool changed = false;
    for (auto& path : paths) {
        if (path == oldPath) {
            path = newPath;
            changed = true;
        }
    }
    if (changed) {
        stageManager()->setSelectedPaths(currentStage(), paths);
    }
}

void OutlinerViewPanel::moveSelectedPrim(int direction) {
    const QModelIndexList selected = m_treeView->selectionModel()->selectedIndexes();
    if (selected.size() != 1 || !currentStage()) {
        return;
    }

    const PXR_NS::SdfPath path = m_model->pathForIndex(selected.first());
    const PXR_NS::UsdPrim prim = currentStage()->usdStage()->GetPrimAtPath(path);
    const PXR_NS::UsdPrim parent = prim ? prim.GetParent() : PXR_NS::UsdPrim();
    if (!prim || !parent || prim.IsInstanceProxy()) {
        return;  // an instance proxy can't be reordered
    }

    // "reorder nameChildren" on the parent's prim spec controls the composed
    // child order regardless of which layer(s) actually define each child —
    // the correct, composition-aware way to reorder siblings (as opposed to
    // moving specs between layers, which SetNameChildrenOrder does not do).
    std::vector<PXR_NS::TfToken> order;
    for (const auto& sibling : parent.GetFilteredChildren(PXR_NS::UsdPrimAllPrimsPredicate)) {
        order.push_back(sibling.GetName());
    }
    auto it = std::find(order.begin(), order.end(), prim.GetName());
    if (it == order.end()) {
        return;
    }
    const auto swapWith = direction < 0 ? it - 1 : it + 1;
    if (swapWith < order.begin() || swapWith >= order.end()) {
        return;  // already first/last
    }
    std::iter_swap(it, swapWith);

    const PXR_NS::UsdStageRefPtr& usdStage = currentStage()->usdStage();
    // No refresh here: the edit's change notice rebuilds the tree, keeping
    // its expansion and the selection (see onStageObjectsChanged()).
    m_undoManager->apply(currentStage(), tr("Reorder %1").arg(QString::fromStdString(prim.GetName().GetString())),
                                               [&]() {
                                                   if (!usdStage->GetEditTarget().GetPrimSpecForScenePath(parent.GetPath())) {
                                                       usdStage->OverridePrim(parent.GetPath());
                                                   }
                                                   if (PXR_NS::SdfPrimSpecHandle parentSpec =
                                                           usdStage->GetEditTarget().GetPrimSpecForScenePath(parent.GetPath())) {
                                                       parentSpec->SetNameChildrenOrder(order);
                                                       return true;
                                                   }
                                                   return false;
                                               });
}

}  // namespace usdcc::usd
