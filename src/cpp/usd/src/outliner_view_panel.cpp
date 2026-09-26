#include "usdcc/usd/outliner_view_panel.h"

#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/stage.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace usdcc::usd {

OutlinerViewPanel::OutlinerViewPanel(StageManager* stageManager, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(title, parent), m_stageManager(stageManager) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 4, 4, 4);

    m_stageCombo = new QComboBox(toolbar);
    toolbarLayout->addWidget(new QLabel(tr("Stage:"), toolbar));
    toolbarLayout->addWidget(m_stageCombo, 1);

    m_model = new UsdPrimTreeModel(this);
    m_treeView = new QTreeView(container);
    m_treeView->setModel(m_model);
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    layout->addWidget(toolbar);
    layout->addWidget(m_treeView, 1);

    setContentWidget(container);

    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &OutlinerViewPanel::onStageComboChanged);
    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &OutlinerViewPanel::onTreeSelectionChanged);
    connect(m_model, &UsdPrimTreeModel::primRenamed, this, &OutlinerViewPanel::onPrimRenamed);

    if (m_stageManager) {
        m_stageOpenedConnection = m_stageManager->stageOpened.connect([this](StageRefPtr) { refreshStageCombo(); });
        m_stageClosedConnection = m_stageManager->stageClosed.connect([this](StageRefPtr) { refreshStageCombo(); });
        m_selectionChangedConnection = m_stageManager->selectionChanged.connect(
            [this](StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
                onStageSelectionChanged(stage, std::move(paths));
            });
    }

    refreshStageCombo();
}

usdcc::ui::ViewPanel* OutlinerViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new OutlinerViewPanel(m_stageManager, windowTitle(), parent);
    const int stageIndex = m_stageCombo->currentIndex();
    if (stageIndex >= 0) {
        copy->m_stageCombo->setCurrentIndex(stageIndex);
    }
    return copy;
}

void OutlinerViewPanel::populateContextMenu(QMenu* menu) {
    usdcc::ui::ViewPanel::populateContextMenu(menu);
    menu->addAction(tr("Move Up"), this, [this]() { moveSelectedPrim(-1); });
    menu->addAction(tr("Move Down"), this, [this]() { moveSelectedPrim(1); });
}

void OutlinerViewPanel::refreshStageCombo() {
    m_stageCombo->blockSignals(true);
    m_stageCombo->clear();

    if (m_stageManager) {
        for (const auto& stage : m_stageManager->stages()) {
            const QString identifier = QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier());
            m_stageCombo->addItem(identifier, static_cast<qlonglong>(stage->cacheId()));
        }
    }

    m_stageCombo->blockSignals(false);
    onStageComboChanged(m_stageCombo->currentIndex());
}

void OutlinerViewPanel::onStageComboChanged(int index) {
    if (!m_stageManager || index < 0) {
        m_stage = nullptr;
        m_model->setStage(nullptr);
        return;
    }
    const auto cacheId = static_cast<long>(m_stageCombo->itemData(index).toLongLong());
    m_stage = m_stageManager->findByCacheId(cacheId);
    m_model->setStage(m_stage ? m_stage->usdStage() : PXR_NS::UsdStageRefPtr());
    onStageSelectionChanged(m_stage, m_stageManager->selectedPaths(m_stage));
}

void OutlinerViewPanel::onTreeSelectionChanged() {
    if (m_updatingSelection || !m_stageManager || !m_stage) {
        return;
    }
    std::vector<PXR_NS::SdfPath> paths;
    for (const QModelIndex& index : m_treeView->selectionModel()->selectedIndexes()) {
        paths.push_back(m_model->pathForIndex(index));
    }
    m_stageManager->setSelectedPaths(m_stage, paths);
}

void OutlinerViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage != m_stage) {
        return;
    }
    m_updatingSelection = true;
    QItemSelection selection;
    for (const auto& path : paths) {
        const QModelIndex index = m_model->indexForPath(path);
        if (index.isValid()) {
            selection.select(index, index);
        }
    }
    m_treeView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    m_updatingSelection = false;
}

void OutlinerViewPanel::onPrimRenamed(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath) {
    if (!m_stageManager || !m_stage) {
        return;
    }
    auto paths = m_stageManager->selectedPaths(m_stage);
    bool changed = false;
    for (auto& path : paths) {
        if (path == oldPath) {
            path = newPath;
            changed = true;
        }
    }
    if (changed) {
        m_stageManager->setSelectedPaths(m_stage, paths);
    }
}

void OutlinerViewPanel::moveSelectedPrim(int direction) {
    if (!m_stage) {
        return;
    }
    const QModelIndexList selected = m_treeView->selectionModel()->selectedIndexes();
    if (selected.size() != 1) {
        return;
    }

    const PXR_NS::SdfPath path = m_model->pathForIndex(selected.first());
    const PXR_NS::UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(path);
    const PXR_NS::UsdPrim parent = prim ? prim.GetParent() : PXR_NS::UsdPrim();
    if (!prim || !parent) {
        return;
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

    if (!m_stage->usdStage()->GetEditTarget().GetPrimSpecForScenePath(parent.GetPath())) {
        // Reordering requires a prim spec for the parent on the edit target
        // layer to author the "reorder nameChildren" opinion onto.
        m_stage->usdStage()->OverridePrim(parent.GetPath());
    }
    if (PXR_NS::SdfPrimSpecHandle parentSpec =
            m_stage->usdStage()->GetEditTarget().GetPrimSpecForScenePath(parent.GetPath())) {
        parentSpec->SetNameChildrenOrder(order);
    }

    m_model->refresh();

    // refresh() reset the model, losing the view's selection; restore it so
    // repeated Move Up/Down on the same prim doesn't require re-selecting.
    const QModelIndex newIndex = m_model->indexForPath(path);
    if (newIndex.isValid()) {
        m_treeView->setCurrentIndex(newIndex);
    }
}

}  // namespace usdcc::usd
