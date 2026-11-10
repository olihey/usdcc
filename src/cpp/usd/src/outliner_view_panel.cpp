#include "usdcc/usd/outliner_view_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/outliner_item_delegate.h"
#include "usdcc/usd/prim_tree_filter.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usd/stage.h>

#include <QJsonArray>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
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
constexpr auto kAskBeforeMovingKey = "Outliner/askBeforeMovingAcrossLayers";
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
    m_proxy = new PrimTreeFilterProxy(this);
    m_proxy->setPrimModel(m_model);
    m_treeView = new QTreeView(container);
    m_treeView->setModel(m_proxy);
    // Prim icons, and the active checkbox at the row's right edge.
    m_treeView->setItemDelegate(new OutlinerItemDelegate(m_treeView));
    usdcc::ui::scaled::iconSize(m_treeView, 16, 16);
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    // InternalMove: from this tree to itself. The drop only asks (dropRequested).
    m_treeView->setDragEnabled(true);
    m_treeView->setAcceptDrops(true);
    m_treeView->setDropIndicatorShown(true);
    m_treeView->setDragDropMode(QAbstractItemView::InternalMove);
    m_treeView->setAutoExpandDelay(600);
    m_treeView->viewport()->installEventFilter(this);

    m_dropNote = new QLabel(container);
    m_dropNote->setObjectName(QStringLiteral("propertyNote"));
    m_dropNote->setWordWrap(true);
    usdcc::ui::scaled::margins(m_dropNote, 8, 4, 8, 4);
    m_dropNote->hide();

    // A search and the types to show, on a row of their own below the toolbar.
    auto* filterRow = new QWidget(container);
    filterRow->setObjectName(QStringLiteral("panelToolbar"));
    filterRow->setAttribute(Qt::WA_StyledBackground);
    auto* filterLayout = new QHBoxLayout(filterRow);
    usdcc::ui::scaled::margins(filterLayout, 4, 0, 4, 4);
    m_filterBar = new PrimTreeFilterBar(m_treeView, m_proxy, filterRow);
    filterLayout->addWidget(m_filterBar);

    layout->addWidget(toolbar);
    layout->addWidget(filterRow);
    layout->addWidget(m_treeView, 1);
    layout->addWidget(m_dropNote);

    setContentWidget(container);

    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &OutlinerViewPanel::onTreeSelectionChanged);
    // The checkbox and the name editor ask; the edits are undo steps here.
    connect(m_model, &UsdPrimTreeModel::activeChangeRequested, this, [this](const PXR_NS::SdfPath& path, bool active) {
        if (currentStage()) prim_actions::setActive(*m_undoManager, currentStage(), {path}, active);
    });
    connect(m_model, &UsdPrimTreeModel::renameRequested, this, [this](const PXR_NS::SdfPath& path, const std::string& name) {
        if (currentStage()) prim_actions::rename(*m_undoManager, currentStage(), path, name, this->stageManager()->stages());
    });
    connect(m_model, &UsdPrimTreeModel::dropHint, this, [this](const QString& why) {
        m_dropNote->setText(why);
        m_dropNote->setVisible(!why.isEmpty());
    });
    connect(m_model, &UsdPrimTreeModel::dropRequested, this,
            [this](const std::vector<PXR_NS::SdfPath>& paths, const PXR_NS::SdfPath& parent, int row) {
                const bool keepWorld = !(QApplication::keyboardModifiers() & Qt::AltModifier);
                // Queued: it may ask a question, and it rebuilds the tree, from inside the view's drop.
                QMetaObject::invokeMethod(
                    this, [this, paths, parent, row, keepWorld]() { performDrop(paths, parent, row, keepWorld); }, Qt::QueuedConnection);
            });
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_treeView, &QTreeView::customContextMenuRequested, this, &OutlinerViewPanel::showPrimMenu);
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

bool OutlinerViewPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_treeView->viewport()) {
        switch (event->type()) {
        case QEvent::DragLeave:
        case QEvent::Drop:
            m_dropNote->hide();
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            if (handleSubtreeClick(static_cast<QMouseEvent*>(event))) {
                m_eatRelease = true;
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (std::exchange(m_eatRelease, false)) return true;
            break;
        default:
            break;
        }
    }
    return usdcc::ui::ViewPanel::eventFilter(watched, event);
}

bool OutlinerViewPanel::handleSubtreeClick(const QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return false;
    const Qt::KeyboardModifiers modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != Qt::ShiftModifier && modifiers != Qt::ControlModifier) return false;
    const QPoint pos = event->position().toPoint();
    const QModelIndex index = m_treeView->indexAt(pos).siblingAtColumn(0);
    if (!index.isValid() || !m_proxy->hasChildren(index)) return false;
    // The expand arrow sits in the indentation just left of the row's first column.
    const QRect rect = m_treeView->visualRect(index);
    if (pos.x() >= rect.left() || pos.x() < rect.left() - m_treeView->indentation()) return false;
    setSubtreeExpanded(index, modifiers == Qt::ShiftModifier);
    return true;
}

void OutlinerViewPanel::setSubtreeExpanded(const QModelIndex& index, bool expanded) {
    if (!index.isValid()) return;
    if (expanded) {
        // Children load on first expand; expandRecursively doesn't fetch them, so load the subtree first.
        std::vector<QModelIndex> pending{index};
        while (!pending.empty()) {
            const QModelIndex parent = pending.back();
            pending.pop_back();
            while (m_proxy->canFetchMore(parent)) m_proxy->fetchMore(parent);
            for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
                const QModelIndex child = m_proxy->index(row, 0, parent);
                if (m_proxy->hasChildren(child)) pending.push_back(child);
            }
        }
        m_treeView->expandRecursively(index);
        return;
    }
    // Collapse the loaded descendants too, so expanding the row again shows only its children.
    std::vector<QModelIndex> pending{index};
    std::vector<QModelIndex> toCollapse;
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        if (!m_treeView->isExpanded(parent) && parent != index) continue;
        toCollapse.push_back(parent);
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) pending.push_back(m_proxy->index(row, 0, parent));
    }
    // Deepest first, so no row is laid out again after its parent closed.
    for (auto it = toCollapse.rbegin(); it != toCollapse.rend(); ++it) m_treeView->collapse(*it);
}

void OutlinerViewPanel::performDrop(const std::vector<PXR_NS::SdfPath>& paths, const PXR_NS::SdfPath& parent, int row, bool keepWorldPosition) {
    const StageRefPtr stage = currentStage();
    if (!stage) return;
    QSettings settings = usdcc::ui::appSettings();
    if (settings.value(kAskBeforeMovingKey, true).toBool()) {
        const prim_actions::MoveNotes notes = prim_actions::moveNotes(stage->usdStage(), paths, parent);
        if (!notes.isEmpty()) {
            QStringList lines;
            if (!notes.otherLayers.empty()) {
                QStringList names;
                for (const std::string& name : notes.otherLayers) names << QString::fromStdString(name);
                lines << tr("This also changes other layers, which will need saving: %1.").arg(names.join(QStringLiteral(", ")));
            }
            for (const PXR_NS::SdfPath& path : notes.relocated) {
                lines << tr("%1 comes from a reference or similar, so its file can't change: this adds a relocates entry to %2 instead.")
                             .arg(QString::fromStdString(path.GetString()), QString::fromStdString(stage->editTargetLayer()->GetDisplayName()));
            }
            QMessageBox box(QMessageBox::Question, tr("Move Prim"), lines.join(QStringLiteral("\n\n")), QMessageBox::Yes | QMessageBox::Cancel, this);
            box.setDefaultButton(QMessageBox::Yes);
            auto* dontAsk = new QCheckBox(tr("Don't ask again"), &box);
            box.setCheckBox(dontAsk);
            if (box.exec() != QMessageBox::Yes) return;
            if (dontAsk->isChecked()) settings.setValue(kAskBeforeMovingKey, false);
        }
    }
    prim_actions::MoveOptions options;
    options.row = row;
    options.keepWorldPosition = keepWorldPosition;
    options.openStages = stageManager()->stages();
    const QString why = prim_actions::movePrims(*m_undoManager, stage, paths, parent, options);
    if (!why.isEmpty()) USDCC_LOG_WARNING("usdcc.stage", "{}", why.toStdString());
}

void OutlinerViewPanel::showPrimMenu(const QPoint& pos) {
    const QModelIndex index = m_treeView->indexAt(pos);
    if (!index.isValid()) {
        // Empty space: creating at the root. The selection stays as it is.
        QMenu menu(this);
        populateEmptySpaceMenu(&menu, stageManager(), currentStage(), m_undoManager.get());
        menu.exec(m_treeView->viewport()->mapToGlobal(pos));
        return;
    }
    if (!currentStage()) {
        return;
    }
    if (!m_treeView->selectionModel()->isSelected(index)) {
        m_treeView->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    }
    PrimMenuContext context;
    context.undo = m_undoManager.get();
    context.stageManager = stageManager();
    context.stage = currentStage();
    context.paths = stageManager()->selectedPaths(currentStage());
    context.clicked = m_model->pathForIndex(m_proxy->mapToSource(index));
    context.dialogParent = this;
    context.move = [this](int direction) { moveSelectedPrim(direction); };
    context.loadChanged = [this]() { dropMissingSelection(); };
    QMenu menu(this);
    populatePrimMenu(&menu, context);
    menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

void OutlinerViewPanel::dropMissingSelection() {
    std::vector<PXR_NS::SdfPath> paths = stageManager()->selectedPaths(currentStage());
    const auto gone = std::remove_if(paths.begin(), paths.end(), [this](const PXR_NS::SdfPath& path) {
        return !currentStage()->usdStage()->GetPrimAtPath(path);
    });
    if (gone == paths.end()) {
        return;
    }
    paths.erase(gone, paths.end());
    stageManager()->setSelectedPaths(currentStage(), paths);
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
    m_primCreatedConnection = {};
    m_primsMovedConnection = {};
    m_revealAfterRefresh = PXR_NS::SdfPath();
    m_movesToRestore.clear();
    if (stage) {
        m_stageChangedConnection =
            stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
        m_primCreatedConnection = stage->primCreated.connect([this](const PXR_NS::SdfPath& path) { m_revealAfterRefresh = path; });
        // Before the tree rebuilds, which is queued by the edit's change notice.
        m_primsMovedConnection = stage->primsMoved.connect([this](const PrimMoves& moves) {
            m_movesToRestore.insert(m_movesToRestore.end(), moves.begin(), moves.end());
            if (!moves.empty()) m_revealAfterRefresh = moves.front().second;
        });
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
        if (m_revealAfterRefresh.IsEmpty()) return;
        // After the scroll position the refresh restores, also on the next pass.
        QTimer::singleShot(0, this, [this, path = m_revealAfterRefresh]() { expandToSelection({path}); });
        m_revealAfterRefresh = PXR_NS::SdfPath();
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
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (m_treeView->isExpanded(index)) {
                m_expandedPaths.insert(m_model->pathForIndex(m_proxy->mapToSource(index)));
                pending.push_back(index);  // collapsed subtrees keep their children's state hidden anyway
            }
        }
    }
}

void OutlinerViewPanel::restoreViewState() {
    if (!m_keepViewState) {
        return;
    }
    for (const PXR_NS::SdfPath& saved : m_expandedPaths) {
        const PXR_NS::SdfPath path = pathAfterMoves(saved, m_movesToRestore);
        const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(path));  // loads instance proxies as needed
        if (index.isValid()) {
            m_treeView->setExpanded(index, true);
        }
    }
    m_movesToRestore.clear();
    m_filterBar->reapply();  // while filtering, everything that shows is open
    onStageSelectionChanged(currentStage(), stageManager()->selectedPaths(currentStage()));
    // After the view has laid out the restored rows.
    const int scrollPosition = m_scrollPosition;
    QTimer::singleShot(0, this, [this, scrollPosition]() { m_treeView->verticalScrollBar()->setValue(scrollPosition); });
}

QJsonObject OutlinerViewPanel::saveSessionState() const {
    QJsonObject state;
    QJsonArray expanded;
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (!m_treeView->isExpanded(index)) continue;
            expanded << QString::fromStdString(m_model->pathForIndex(m_proxy->mapToSource(index)).GetString());
            pending.push_back(index);
        }
    }
    state.insert(QStringLiteral("expanded"), expanded);
    if (!m_proxy->nameFilter().isEmpty()) state.insert(QStringLiteral("search"), m_proxy->nameFilter());
    if (const auto& types = m_proxy->typeFilter()) {
        QJsonArray list;
        for (const std::string& type : *types) list << QString::fromStdString(type);
        state.insert(QStringLiteral("types"), list);
    }
    state.insert(QStringLiteral("scroll"), m_treeView->verticalScrollBar()->value());
    return state;
}

void OutlinerViewPanel::restoreSessionState(const QJsonObject& state) {
    if (!currentStage()) return;
    // Expanded first, unfiltered: the filter remembers them to go back to.
    m_filterBar->clear();
    for (const QJsonValue& value : state.value(QStringLiteral("expanded")).toArray()) {
        const std::string text = value.toString().toStdString();
        if (!PXR_NS::SdfPath::IsValidPathString(text)) continue;
        const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(PXR_NS::SdfPath(text)));
        if (index.isValid()) m_treeView->setExpanded(index, true);  // a prim that's gone is skipped
    }
    if (state.contains(QStringLiteral("types"))) {
        std::set<std::string> types;
        for (const QJsonValue& value : state.value(QStringLiteral("types")).toArray()) types.insert(value.toString().toStdString());
        m_filterBar->setTypeFilter(types);
    }
    if (state.contains(QStringLiteral("search"))) m_filterBar->setNameFilter(state.value(QStringLiteral("search")).toString());
    // After the view has laid out the rows.
    const int scroll = state.value(QStringLiteral("scroll")).toInt();
    QTimer::singleShot(0, this, [this, scroll]() { m_treeView->verticalScrollBar()->setValue(scroll); });
}

void OutlinerViewPanel::onTreeSelectionChanged() {
    if (m_updatingSelection || !currentStage()) {
        return;
    }
    std::vector<PXR_NS::SdfPath> paths;
    for (const QModelIndex& index : m_treeView->selectionModel()->selectedIndexes()) {
        paths.push_back(m_model->pathForIndex(m_proxy->mapToSource(index)));
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
        const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(path));  // invalid when filtered out
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
        const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(path));
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

void OutlinerViewPanel::moveSelectedPrim(int direction) {
    const QModelIndexList selected = m_treeView->selectionModel()->selectedIndexes();
    if (selected.size() != 1 || !currentStage()) {
        return;
    }

    // In the model, not as filtered: a move is among all its siblings.
    const QModelIndex source = m_proxy->mapToSource(selected.first());
    const PXR_NS::SdfPath path = m_model->pathForIndex(source);
    const PXR_NS::UsdPrim prim = currentStage()->usdStage()->GetPrimAtPath(path);
    const PXR_NS::UsdPrim parent = prim ? prim.GetParent() : PXR_NS::UsdPrim();
    if (!prim || !parent) {
        return;
    }
    const int row = selected.first().row();
    const int siblings = m_model->rowCount(source.parent());
    if ((direction < 0 && row == 0) || (direction > 0 && row + 1 >= siblings)) {
        return;  // already first/last
    }
    // No refresh here: the edit's change notice rebuilds the tree, keeping
    // its expansion and the selection (see onStageObjectsChanged()).
    prim_actions::MoveOptions options;
    options.row = direction < 0 ? row - 1 : row + 2;  // before that row, as a drop says it
    const QString why = prim_actions::movePrims(*m_undoManager, currentStage(), {path}, parent.GetPath(), options);
    if (!why.isEmpty()) USDCC_LOG_WARNING("usdcc.stage", "{}", why.toStdString());
}

}  // namespace usdcc::usd
