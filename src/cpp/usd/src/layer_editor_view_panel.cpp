#include "usdcc/usd/layer_editor_view_panel.h"

#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/usda_preview_dialog.h"

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace usdcc::usd {

namespace {
const QColor kMissingColor(0xff, 0x5f, 0x56);   // as the log's errors
const QColor kMutedColor(255, 255, 255, 89);     // 35%, as a note
const QColor kTargetColor(0x3d, 0x8b, 0xfd);     // the accent
constexpr int kIdentifierRole = Qt::UserRole;

QString qs(const std::string& text) { return QString::fromStdString(text); }

const LayerEntry* findEntry(const std::vector<LayerEntry>& entries, const std::string& identifier) {
    for (const LayerEntry& entry : entries) {
        if (entry.identifier == identifier) return &entry;
        if (const LayerEntry* found = findEntry(entry.children, identifier)) return found;
    }
    return nullptr;
}

const LayerEntry* findTarget(const std::vector<LayerEntry>& entries) {
    for (const LayerEntry& entry : entries) {
        if (entry.editTarget) return &entry;
        if (const LayerEntry* found = findTarget(entry.children)) return found;
    }
    return nullptr;
}

QString realPath(const LayerEntry& entry) {
    return entry.layer && !entry.layer->GetRealPath().empty() ? qs(entry.layer->GetRealPath()) : QString();
}

constexpr auto kEditNewLayerKey = "Layers/editNewLayer";

// A drop above or below a sibling reorders the sublayers (through
// `onMove`); every other drop is refused. The tree itself never moves an
// item: the edit's change notice rebuilds it.
class LayerTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(QTreeWidgetItem* dragged, int index)> onMove;

protected:
    void dropEvent(QDropEvent* event) override {
        QTreeWidgetItem* dragged = currentItem();
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        const DropIndicatorPosition position = dropIndicatorPosition();
        event->ignore();
        setState(NoState);  // ends the drag without QTreeWidget's own move
        viewport()->update();
        if (!dragged || !target || target == dragged || !dragged->parent() || target->parent() != dragged->parent() ||
            (position != AboveItem && position != BelowItem)) {
            return;
        }
        QTreeWidgetItem* parent = dragged->parent();
        int to = parent->indexOfChild(target) + (position == BelowItem ? 1 : 0);
        if (to > parent->indexOfChild(dragged)) --to;
        if (onMove) onMove(dragged, to);
    }
};
}  // namespace

LayerEditorViewPanel::LayerEditorViewPanel(StageManager* stageManager, StageRefPtr stage,
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
    toolbarLayout->addStretch(1);
    m_editing = new QLabel(toolbar);
    m_editing->setObjectName(QStringLiteral("propertyNote"));
    toolbarLayout->addWidget(m_editing);

    // + : a new or existing layer, under the selected one (§4).
    m_editNewLayer = new QAction(tr("Edit the New Layer"), this);
    m_editNewLayer->setToolTip(tr("Make a new layer the edit target when it's added"));
    m_editNewLayer->setCheckable(true);
    m_editNewLayer->setChecked(usdcc::ui::appSettings().value(kEditNewLayerKey, true).toBool());
    connect(m_editNewLayer, &QAction::toggled, this, [](bool on) {
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kEditNewLayerKey, on);
    });
    auto* add = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/plus.png"),
                                               tr("Add a layer under the selected one"), toolbar);
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add);
    connect(addMenu, &QMenu::aboutToShow, this, [this, addMenu]() {
        addMenu->clear();
        populateAddMenu(addMenu, parentForAdd());
    });
    add->setMenu(addMenu);
    toolbarLayout->addWidget(add);
    // Save: the selected layer, else the edit target (§5).
    auto* save = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/save.png"),
                                                tr("Save the selected layer (Ctrl+S)"), toolbar);
    connect(save, &QToolButton::clicked, this, [this]() { saveLayer(layerToSave()); });
    toolbarLayout->addWidget(save);

    auto* tree = new LayerTree(container);
    tree->onMove = [this](QTreeWidgetItem* dragged, int index) {
        const std::string identifier = dragged->data(Name, kIdentifierRole).toString().toStdString();
        QMetaObject::invokeMethod(
            this, [this, identifier, index]() { layer_actions::moveLayer(*m_undoManager, currentStage(), identifier, index); },
            Qt::QueuedConnection);
    };
    m_tree = tree;
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDragEnabled(true);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Layer"), tr("Target"), tr("Mute")});
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(Target, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(Mute, QHeaderView::ResizeToContents);
    m_tree->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    usdcc::ui::scaled::iconSize(m_tree, 12, 12);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &LayerEditorViewPanel::showMenu);
    connect(m_tree, &QTreeWidget::itemChanged, this, &LayerEditorViewPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::itemClicked, this, &LayerEditorViewPanel::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == Name) setEditTarget(item->data(Name, kIdentifierRole).toString().toStdString());
    });

    // Delete removes the selected layer, Ctrl+Up/Down moves it.
    auto shortcut = [this](const QKeySequence& keys, std::function<void(const std::string&)> action) {
        auto* sc = new QShortcut(keys, m_tree, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this, action]() {
            const std::string identifier = selectedIdentifier();
            if (!identifier.empty()) action(identifier);
        });
    };
    shortcut(QKeySequence::Delete, [this](const std::string& id) { removeLayer(id); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Up), [this](const std::string& id) { moveBy(id, -1); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Down), [this](const std::string& id) { moveBy(id, 1); });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, container, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, [this]() { saveLayer(layerToSave()); });

    // The selected layer's properties, under the tree (§6).
    auto* propertiesScroll = new QScrollArea(container);
    propertiesScroll->setWidgetResizable(true);
    propertiesScroll->setFrameShape(QFrame::NoFrame);
    m_properties = new QWidget(propertiesScroll);
    m_propertiesLayout = new QVBoxLayout(m_properties);
    usdcc::ui::scaled::margins(m_propertiesLayout, 10, 8, 10, 8);
    usdcc::ui::scaled::spacing(m_propertiesLayout, 6);
    propertiesScroll->setWidget(m_properties);
    // On the selection, not the current item: a click makes a row current
    // before it selects it, so the current row isn't selected yet then.
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        if (!m_filling) updateProperties();
    });

    auto* splitter = new QSplitter(Qt::Vertical, container);
    splitter->addWidget(m_tree);
    splitter->addWidget(propertiesScroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);

    layout->addWidget(toolbar);
    layout->addWidget(splitter, 1);
    setContentWidget(container);

    // An edit can send several notices, a drag many.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout, this, &LayerEditorViewPanel::refresh);

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* LayerEditorViewPanel::duplicate(QWidget* parent) const {
    return new LayerEditorViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

void LayerEditorViewPanel::populateOptionsMenu(QMenu* menu) {
    QAction* saveAll = menu->addAction(tr("Save All"), this, [this]() {
        QString error;
        if (!layer_actions::saveAll(currentStage(), &error)) QMessageBox::warning(this, tr("Save All"), error);
    });
    saveAll->setEnabled(!layer_actions::unsavedLayers(currentStage()).empty());
    menu->addSeparator();
    menu->addAction(tr("Refresh"), this, &LayerEditorViewPanel::refresh);
}

void LayerEditorViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    m_editTargetConnection = {};
    if (stage) {
        // Any edit can make a layer dirty; muting and sublayer changes resync.
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) { m_refreshTimer->start(); });
        m_editTargetConnection = stage->editTargetChanged.connect([this]() { m_refreshTimer->start(); });
        m_layersSavedConnection = stage->layersSaved.connect([this]() { m_refreshTimer->start(); });
    } else {
        m_layersSavedConnection = {};
    }
    refresh();
}

const LayerEntry* LayerEditorViewPanel::entryFor(const std::string& identifier) const {
    return findEntry(m_layers, identifier);
}

QTreeWidgetItem* LayerEditorViewPanel::itemFor(const std::string& identifier) const {
    const QString id = qs(identifier);
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(Name, kIdentifierRole).toString() == id) return *it;
    }
    return nullptr;
}

void LayerEditorViewPanel::refresh() {
    m_refreshTimer->stop();
    // Keep what was collapsed and selected, by layer.
    QSet<QString> collapsed;
    QString selected;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->childCount() && !(*it)->isExpanded()) collapsed.insert((*it)->data(Name, kIdentifierRole).toString());
        if ((*it)->isSelected()) selected = (*it)->data(Name, kIdentifierRole).toString();
    }
    const bool first = m_tree->topLevelItemCount() == 0;

    m_filling = true;
    m_tree->clear();
    m_layers = currentStage() ? layerTree(currentStage()->usdStage()) : std::vector<LayerEntry>();
    for (const LayerEntry& entry : m_layers) addItems(entry, nullptr);
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QString id = (*it)->data(Name, kIdentifierRole).toString();
        const LayerEntry* entry = entryFor(id.toStdString());
        // The session layer starts collapsed; the rest open.
        const bool closed = first ? (entry && entry->session) : collapsed.contains(id);
        (*it)->setExpanded(!closed);
    }
    // The layer just added, else the one selected before.
    if (!m_selectAfterRefresh.empty()) {
        selected = qs(m_selectAfterRefresh);
        m_selectAfterRefresh.clear();
    }
    if (QTreeWidgetItem* item = selected.isEmpty() ? nullptr : itemFor(selected.toStdString())) {
        m_tree->setCurrentItem(item);
    }
    m_filling = false;

    const LayerEntry* target = findTarget(m_layers);
    m_editing->setText(target ? tr("Editing: %1").arg(qs(target->displayName)) : QString());
    m_editing->setToolTip(target ? qs(target->identifier) : QString());
    updateProperties();
}

void LayerEditorViewPanel::addItems(const LayerEntry& entry, QTreeWidgetItem* parent) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    item->setData(Name, kIdentifierRole, qs(entry.identifier));

    QString name = entry.session ? tr("Session Layer") : qs(entry.displayName);
    if (entry.dirty) name += QStringLiteral(" *");
    if (entry.anonymous && !entry.session) name += QStringLiteral("  ") + tr("(in memory)");
    if (entry.missing) name += QStringLiteral("  ") + tr("(not found)");
    if (entry.muted) name += QStringLiteral("  ") + tr("(muted)");
    item->setText(Name, name);

    QStringList tip{qs(entry.identifier)};
    if (const QString path = realPath(entry); !path.isEmpty() && path != tip.front()) tip << QDir::toNativeSeparators(path);
    if (!entry.authoredPath.empty()) tip << tr("As authored: %1").arg(qs(entry.authoredPath));
    if (entry.layer) tip << tr("Format: %1").arg(qs(entry.layer->GetFileFormat()->GetFormatId().GetString()));
    if (!entry.offset.IsIdentity()) {
        tip << tr("Time offset %1, scale %2").arg(entry.offset.GetOffset()).arg(entry.offset.GetScale());
    }
    if (entry.dirty) tip << tr("Has unsaved changes");
    item->setToolTip(Name, tip.join(QLatin1Char('\n')));

    QFont font = item->font(Name);
    font.setItalic(entry.anonymous);
    font.setBold(entry.editTarget);
    item->setFont(Name, font);
    if (entry.missing) {
        item->setForeground(Name, kMissingColor);
    } else if (entry.muted) {
        item->setForeground(Name, kMutedColor);
    } else if (entry.editTarget) {
        item->setForeground(Name, kTargetColor);
    }

    if (entry.editTarget) {
        item->setIcon(Target, QIcon(QStringLiteral(":/usdcc/theme/pencil.png")));
        item->setToolTip(Target, tr("Edits go to this layer"));
    } else if (layer_actions::canTarget(entry)) {
        item->setToolTip(Target, tr("Click to edit this layer"));
    }

    // Only a sublayer can be dragged, to reorder it among its siblings.
    item->setFlags(layer_actions::canRemove(entry) ? item->flags() | Qt::ItemIsDragEnabled
                                                   : item->flags() & ~Qt::ItemIsDragEnabled);
    if (layer_actions::canMute(entry)) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Mute, entry.muted ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(Mute, entry.muted ? tr("Unmute: compose this layer again")
                                           : tr("Mute: leave this layer (and its sublayers) out of the stage"));
    }

    for (const LayerEntry& child : entry.children) addItems(child, item);
}

void LayerEditorViewPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (m_filling || column != Mute || !currentStage() || !m_undoManager) return;
    const std::string identifier = item->data(Name, kIdentifierRole).toString().toStdString();
    const bool muted = item->checkState(Mute) == Qt::Checked;
    // Queued: the change notice rebuilds this tree, item included.
    QMetaObject::invokeMethod(
        this, [this, identifier, muted]() { layer_actions::setMuted(*m_undoManager, currentStage(), identifier, muted); },
        Qt::QueuedConnection);
}

void LayerEditorViewPanel::onItemClicked(QTreeWidgetItem* item, int column) {
    if (column == Target) setEditTarget(item->data(Name, kIdentifierRole).toString().toStdString());
}

void LayerEditorViewPanel::setEditTarget(const std::string& identifier) {
    const LayerEntry* entry = entryFor(identifier);
    if (entry && !entry->editTarget) layer_actions::setEditTarget(currentStage(), *entry);
}

void LayerEditorViewPanel::showMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item) return;
    QMenu menu(this);
    populateLayerMenu(&menu, item->data(Name, kIdentifierRole).toString().toStdString());
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void LayerEditorViewPanel::populateLayerMenu(QMenu* menu, const std::string& identifier) {
    const LayerEntry* entry = entryFor(identifier);
    if (!entry) return;
    menu->setToolTipsVisible(true);

    QAction* target = menu->addAction(tr("Set as Edit Target"), this, [this, identifier]() { setEditTarget(identifier); });
    target->setEnabled(layer_actions::canTarget(*entry) && !entry->editTarget);
    if (layer_actions::canMute(*entry)) {
        const bool muted = entry->muted;
        menu->addAction(muted ? tr("Unmute") : tr("Mute"), this, [this, identifier, muted]() {
            QMetaObject::invokeMethod(
                this,
                [this, identifier, muted]() { layer_actions::setMuted(*m_undoManager, currentStage(), identifier, !muted); },
                Qt::QueuedConnection);
        });
    }

    menu->addSeparator();
    if (layer_actions::canAddTo(*entry)) {
        QMenu* add = menu->addMenu(tr("Add Sublayer"));
        populateAddMenu(add, identifier);
    }
    if (layer_actions::canRemove(*entry)) {
        int index = 0;
        int count = 0;
        layer_actions::sublayerPosition(currentStage(), identifier, &index, &count);
        QAction* up = menu->addAction(tr("Move Up"), this, [this, identifier]() { moveBy(identifier, -1); });
        up->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
        up->setEnabled(index > 0);
        QAction* down = menu->addAction(tr("Move Down"), this, [this, identifier]() { moveBy(identifier, 1); });
        down->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
        down->setEnabled(index + 1 < count);
        QAction* remove = menu->addAction(tr("Remove"), this, [this, identifier]() { removeLayer(identifier); });
        remove->setShortcut(QKeySequence::Delete);
        remove->setToolTip(tr("Take it out of the layer stack; the file stays"));
    }
    QAction* saveAction = menu->addAction(tr("Save"), this, [this, identifier]() { saveLayer(identifier); });
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setEnabled(entry->layer && entry->dirty && !entry->anonymous);
    if (entry->layer) {
        menu->addAction(tr("Save As..."), this, [this, identifier]() { saveLayerAs(identifier); });
    }
    if (layer_actions::canReload(*entry)) {
        QAction* reload = menu->addAction(tr("Reload"), this, [this, identifier]() { reloadLayer(identifier); });
        reload->setToolTip(tr("Discard its unsaved changes and load it fresh from its file"));
    }

    menu->addSeparator();
    const QString path = realPath(*entry);
    const QString id = qs(entry->identifier);
    menu->addAction(tr("Copy Identifier"), this, [id]() { QApplication::clipboard()->setText(id); });
    QAction* copy = menu->addAction(tr("Copy Path"), this,
                                    [path]() { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    copy->setEnabled(!path.isEmpty());
    QAction* reveal = menu->addAction(tr("Reveal in Explorer"), this, [path]() { revealFile(path); });
    reveal->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
    QAction* open = menu->addAction(tr("Open as Stage"), this, [path]() { openStageFile(path); });
    open->setEnabled(!path.isEmpty() && !entry->root);
    // By identifier, so an in-memory layer can show too.
    QAction* show = menu->addAction(tr("Show USDA"), this, [this, id]() { showUsdaPreview(id, this); });
    show->setEnabled(bool(entry->layer));
    QAction* prims = menu->addAction(tr("Show Prims"), this, [this, id]() { showPrimTree(id, this); });
    prims->setEnabled(bool(entry->layer));
}

std::string LayerEditorViewPanel::selectedIdentifier() const {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    return selected.isEmpty() ? std::string() : selected.front()->data(Name, kIdentifierRole).toString().toStdString();
}

std::string LayerEditorViewPanel::parentForAdd() const {
    const std::string selected = selectedIdentifier();
    if (!selected.empty()) return selected;
    return currentStage() ? currentStage()->usdStage()->GetRootLayer()->GetIdentifier() : std::string();
}

void LayerEditorViewPanel::populateAddMenu(QMenu* menu, const std::string& parent) {
    menu->setToolTipsVisible(true);
    const LayerEntry* entry = entryFor(parent);
    const bool can = entry && layer_actions::canAddTo(*entry);
    const QString under = entry ? (entry->session ? tr("the session layer") : qs(entry->displayName)) : QString();
    for (const auto& [kind, text] : {std::pair{AddKind::InMemory, tr("New In-Memory Layer")},
                                     std::pair{AddKind::NewFile, tr("New Layer File...")},
                                     std::pair{AddKind::Existing, tr("Add Existing Layer...")}}) {
        const AddKind which = kind;
        QAction* action = menu->addAction(text, this, [this, which, parent]() { addLayer(which, parent); });
        action->setEnabled(can);
        if (can) action->setToolTip(tr("As the strongest sublayer of %1").arg(under));
    }
    menu->addSeparator();
    menu->addAction(m_editNewLayer);
}

std::string LayerEditorViewPanel::addLayer(AddKind kind, const std::string& parent, const QString& file) {
    if (!currentStage() || !m_undoManager) return {};
    const LayerEntry* entry = entryFor(parent);
    QString path = file;
    if (kind != AddKind::InMemory && path.isEmpty()) {
        // Start in the parent's folder, where a relative path is shortest.
        const QString dir = entry ? QFileInfo(realPath(*entry)).absolutePath() : QString();
        const QString filter = tr("USD Layers (*.usda *.usd *.usdc);;All Files (*)");
        path = kind == AddKind::NewFile ? QFileDialog::getSaveFileName(this, tr("New Layer File"), dir, filter)
                                        : QFileDialog::getOpenFileName(this, tr("Add Existing Layer"), dir, filter);
        if (path.isEmpty()) return {};
        if (kind == AddKind::NewFile && QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".usda");
    }
    QString error;
    std::string added;
    switch (kind) {
        case AddKind::InMemory:
            added = layer_actions::addInMemoryLayer(*m_undoManager, currentStage(), parent, &error);
            break;
        case AddKind::NewFile:
            added = layer_actions::addNewLayerFile(*m_undoManager, currentStage(), parent, path.toStdString(), &error);
            break;
        case AddKind::Existing:
            added = layer_actions::addExistingLayer(*m_undoManager, currentStage(), parent, path.toStdString(), &error);
            break;
    }
    if (added.empty()) {
        if (file.isEmpty()) QMessageBox::warning(this, tr("Add Layer"), error);  // not from a test or script
        return {};
    }
    // An existing layer is often added to look at; a new one to edit in.
    if (kind != AddKind::Existing && m_editNewLayer->isChecked()) {
        currentStage()->setEditTargetLayer(PXR_NS::SdfLayer::Find(added));
    }
    m_selectAfterRefresh = added;
    return added;
}

void LayerEditorViewPanel::removeLayer(const std::string& identifier) {
    if (!currentStage() || !m_undoManager) return;
    // Queued: the change notice rebuilds this tree, from inside a menu or a shortcut.
    QMetaObject::invokeMethod(
        this, [this, identifier]() { layer_actions::removeLayer(*m_undoManager, currentStage(), identifier); },
        Qt::QueuedConnection);
}

void LayerEditorViewPanel::reloadLayer(const std::string& identifier) {
    const LayerEntry* entry = entryFor(identifier);
    if (!entry || !currentStage() || !m_undoManager) return;
    const QString name = qs(entry->displayName);
    const bool undoable = layer_actions::reloadIsUndoable(*entry);
    QUndoStack* stack = m_undoManager->stack(currentStage());
    const bool losesHistory = !undoable && stack && stack->count() > 0;
    // Asked only when something would be lost: unsaved changes, or undo history.
    if (entry->dirty || losesHistory) {
        QString text = entry->dirty ? tr("%1 has unsaved changes. Reloading discards them and loads the layer as "
                                         "it is in its file.")
                                          .arg(name)
                                    : tr("Reloading %1 loads it as it is in its file.").arg(name);
        text += QStringLiteral("\n\n");
        text += undoable ? tr("You can undo it to get the changes back.")
                         : tr("This layer can't be reloaded as an undo step, so it can't be undone, and the "
                              "stage's undo history is cleared.");
        QMessageBox box(QMessageBox::Warning, tr("Reload Layer"), text, QMessageBox::Cancel, this);
        QPushButton* reload = box.addButton(tr("Reload"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != reload) return;
    }
    QString error;
    if (!layer_actions::reloadLayer(*m_undoManager, currentStage(), identifier, &error)) {
        QMessageBox::warning(this, tr("Reload Layer"), error);
    }
}

std::string LayerEditorViewPanel::layerToSave() const {
    const std::string selected = selectedIdentifier();
    if (!selected.empty()) return selected;
    const LayerEntry* target = findTarget(m_layers);
    return target ? target->identifier : std::string();
}

// By the layer itself rather than this panel's last refresh, which may not
// have seen it yet (a layer added a moment ago).
bool LayerEditorViewPanel::saveLayer(const std::string& identifier) {
    const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(identifier);
    if (!layer || !currentStage()) return false;
    if (layer->IsAnonymous()) return !saveLayerAs(identifier).empty();  // nowhere to save it yet
    QString error;
    if (!layer_actions::saveLayer(currentStage(), layer, &error)) {
        QMessageBox::warning(this, tr("Save Layer"), error);
        return false;
    }
    return true;
}

std::string LayerEditorViewPanel::saveLayerAs(const std::string& identifier, const QString& file) {
    const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(identifier);
    if (!layer || !currentStage() || !m_undoManager) return {};
    const QString name = qs(layer->GetDisplayName());
    QString path = file;
    if (path.isEmpty()) {
        const QString current = qs(layer->GetRealPath());
        const QString start = current.isEmpty() ? name + QStringLiteral(".usda") : current;
        path = QFileDialog::getSaveFileName(this, tr("Save %1 As").arg(name), start,
                                            tr("USD Layers (*.usda *.usd *.usdc);;All Files (*)"));
        if (path.isEmpty()) return {};
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".usda");
    }
    const bool inMemory = layer->IsAnonymous();
    QString error;
    const std::string saved =
        layer_actions::saveLayerAs(*m_undoManager, currentStage(), identifier, path.toStdString(), &error);
    if (saved.empty()) {
        if (file.isEmpty()) QMessageBox::warning(this, tr("Save Layer As"), error);
        return {};
    }
    if (inMemory) m_selectAfterRefresh = saved;  // the file took its place
    return saved;
}

void LayerEditorViewPanel::updateProperties() {
    const std::string identifier = selectedIdentifier();
    const LayerEntry* entry = identifier.empty() ? nullptr : entryFor(identifier);
    const bool sublayer = entry && !entry->root && !entry->session;
    // Rebuilt for another layer; for the same one only its values change, so
    // a field being typed in keeps its text.
    const std::string key = entry ? identifier + (sublayer ? "|sublayer" : "") + (entry->layer ? "" : "|missing")
                                  : std::string();
    if (key != m_propertiesFor || m_properties->findChildren<QWidget*>().isEmpty()) {
        m_propertiesFor = key;
        rebuildProperties(entry, sublayer);
    }
    if (!entry) return;
    for (const PropertyField& field : m_fields) {
        if (field.editor->isAncestorOf(QApplication::focusWidget())) continue;
        field.editor->setValue(field.value(*entry));
        if (field.authored) field.row->setUnauthored(!field.authored(*entry));
    }
}

void LayerEditorViewPanel::rebuildProperties(const LayerEntry* entry, bool sublayer) {
    m_fields.clear();
    while (QLayoutItem* item = m_propertiesLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto note = [this](const QString& text) {
        auto* label = new QLabel(text, m_properties);
        label->setObjectName(QStringLiteral("propertyNote"));
        label->setWordWrap(true);
        m_propertiesLayout->addWidget(label);
    };
    if (!entry) {
        note(tr("Select a layer to see its properties."));
        m_propertiesLayout->addStretch(1);
        return;
    }
    if (!entry->layer) {
        note(entry->muted ? tr("Muted, and not open: unmute it to see its properties.")
                          : tr("Not found: %1").arg(QDir::toNativeSeparators(qs(entry->identifier))));
        m_propertiesLayout->addStretch(1);
        return;
    }

    using namespace usdcc::usd::properties;
    const std::string identifier = entry->identifier;
    const QString name = entry->session ? tr("the session layer") : qs(entry->displayName);
    auto addField = [this](const QString& label, PropertyEditor* editor, std::function<PXR_NS::VtValue(const LayerEntry&)> value,
                           std::function<bool(const LayerEntry&)> authored = {}) {
        auto* row = new PropertyRow(label, editor, m_properties);
        row->setLabelWidth(130);
        m_propertiesLayout->addWidget(row);
        m_fields.push_back({row, editor, std::move(value), std::move(authored)});
        return editor;
    };
    auto readOnly = [&](const QString& label, std::function<PXR_NS::VtValue(const LayerEntry&)> value) {
        PropertyEditor* editor = createPropertyEditor(PXR_NS::SdfValueTypeNames->String);
        editor->setReadOnly(true);
        addField(label, editor, std::move(value));
    };
    // An edit of the layer itself, one undo step; muted layers send no change
    // notice, so the tree is refreshed here too.
    auto edit = [this, identifier](const QString& description, std::function<bool(const PXR_NS::SdfLayerHandle&)> change) {
        layer_actions::editLayer(*m_undoManager, currentStage(), identifier, description, change);
        m_refreshTimer->start();
    };
    auto text = [](const std::string& s) { return PXR_NS::VtValue(s); };
    auto stringOf = [](const PXR_NS::VtValue& v) { return v.IsHolding<std::string>() ? v.UncheckedGet<std::string>() : std::string(); };

    readOnly(tr("Identifier"), [text](const LayerEntry& e) { return text(e.identifier); });
    readOnly(tr("File"), [text](const LayerEntry& e) {
        const QString path = realPath(e);
        return text(path.isEmpty() ? tr("(in memory)").toStdString() : QDir::toNativeSeparators(path).toStdString());
    });
    readOnly(tr("Format"), [text](const LayerEntry& e) { return text(e.layer->GetFileFormat()->GetFormatId().GetString()); });

    if (sublayer) {
        // Its offset and scale in the parent's subLayerOffsets.
        auto offsetField = [&](const QString& label, bool scale) {
            PropertyEditor* editor = addField(
                label, createPropertyEditor(PXR_NS::SdfValueTypeNames->Double),
                [scale](const LayerEntry& e) { return PXR_NS::VtValue(scale ? e.offset.GetScale() : e.offset.GetOffset()); },
                [](const LayerEntry& e) { return !e.offset.IsIdentity(); });
            connect(editor, &PropertyEditor::editFinished, this, [this, editor, identifier, scale]() {
                const PXR_NS::VtValue v = editor->value();
                const LayerEntry* e = entryFor(identifier);
                if (!e || !v.IsHolding<double>()) return;
                PXR_NS::SdfLayerOffset offset = e->offset;
                scale ? offset.SetScale(v.UncheckedGet<double>()) : offset.SetOffset(v.UncheckedGet<double>());
                layer_actions::setSublayerOffset(*m_undoManager, currentStage(), identifier, offset);
            });
        };
        offsetField(tr("Time Offset"), false);
        offsetField(tr("Time Scale"), true);
    }

    PropertyEditor* defaultPrim = addField(
        tr("Default Prim"), createPropertyEditor(PXR_NS::SdfValueTypeNames->String),
        [text](const LayerEntry& e) { return text(e.layer->GetDefaultPrim().GetString()); },
        [](const LayerEntry& e) { return e.layer->HasDefaultPrim(); });
    connect(defaultPrim, &PropertyEditor::valueEdited, this, [edit, stringOf, name](const PXR_NS::VtValue& v) {
        const std::string prim = stringOf(v);
        edit(tr("Set Default Prim of %1").arg(name), [prim](const PXR_NS::SdfLayerHandle& layer) {
            prim.empty() ? layer->ClearDefaultPrim() : layer->SetDefaultPrim(PXR_NS::TfToken(prim));
            return true;
        });
    });

    // The layer's time metadata: shown unauthored (with USD's fallback) until set.
    struct TimeField {
        QString label;
        double (PXR_NS::SdfLayer::*get)() const;
        bool (PXR_NS::SdfLayer::*has)() const;
        void (PXR_NS::SdfLayer::*set)(double);
    };
    for (const TimeField& field : {TimeField{tr("Start Time Code"), &PXR_NS::SdfLayer::GetStartTimeCode,
                                             &PXR_NS::SdfLayer::HasStartTimeCode, &PXR_NS::SdfLayer::SetStartTimeCode},
                                   TimeField{tr("End Time Code"), &PXR_NS::SdfLayer::GetEndTimeCode,
                                             &PXR_NS::SdfLayer::HasEndTimeCode, &PXR_NS::SdfLayer::SetEndTimeCode},
                                   TimeField{tr("Time Codes per Second"), &PXR_NS::SdfLayer::GetTimeCodesPerSecond,
                                             &PXR_NS::SdfLayer::HasTimeCodesPerSecond,
                                             &PXR_NS::SdfLayer::SetTimeCodesPerSecond},
                                   TimeField{tr("Frames per Second"), &PXR_NS::SdfLayer::GetFramesPerSecond,
                                             &PXR_NS::SdfLayer::HasFramesPerSecond, &PXR_NS::SdfLayer::SetFramesPerSecond}}) {
        const TimeField f = field;
        PropertyEditor* editor = addField(
            f.label, createPropertyEditor(PXR_NS::SdfValueTypeNames->Double),
            [f](const LayerEntry& e) { return PXR_NS::VtValue(((*e.layer).*f.get)()); },
            [f](const LayerEntry& e) { return ((*e.layer).*f.has)(); });
        connect(editor, &PropertyEditor::editFinished, this, [edit, editor, f, name]() {
            const PXR_NS::VtValue v = editor->value();
            if (!v.IsHolding<double>()) return;
            const double value = v.UncheckedGet<double>();
            edit(tr("Set %1 of %2").arg(f.label, name), [f, value](const PXR_NS::SdfLayerHandle& layer) {
                if (((*layer).*f.has)() && ((*layer).*f.get)() == value) return true;  // nothing to change
                ((*layer).*f.set)(value);
                return true;
            });
        });
    }

    struct TextField {
        QString label;
        std::string (PXR_NS::SdfLayer::*get)() const;
        void (PXR_NS::SdfLayer::*set)(const std::string&);
    };
    for (const TextField& field : {TextField{tr("Comment"), &PXR_NS::SdfLayer::GetComment, &PXR_NS::SdfLayer::SetComment},
                                   TextField{tr("Documentation"), &PXR_NS::SdfLayer::GetDocumentation,
                                             &PXR_NS::SdfLayer::SetDocumentation}}) {
        const TextField f = field;
        PropertyEditor* editor =
            addField(f.label, createPropertyEditor(PXR_NS::SdfValueTypeNames->String),
                     [f, text](const LayerEntry& e) { return text(((*e.layer).*f.get)()); },
                     [f](const LayerEntry& e) { return !((*e.layer).*f.get)().empty(); });
        connect(editor, &PropertyEditor::valueEdited, this, [edit, stringOf, f, name](const PXR_NS::VtValue& v) {
            const std::string value = stringOf(v);
            edit(tr("Set %1 of %2").arg(f.label, name), [f, value](const PXR_NS::SdfLayerHandle& layer) {
                ((*layer).*f.set)(value);
                return true;
            });
        });
    }
    m_propertiesLayout->addStretch(1);
}

void LayerEditorViewPanel::moveBy(const std::string& identifier, int delta) {
    int index = 0;
    if (!m_undoManager || !layer_actions::sublayerPosition(currentStage(), identifier, &index, nullptr)) return;
    m_selectAfterRefresh = identifier;
    QMetaObject::invokeMethod(
        this,
        [this, identifier, index, delta]() {
            layer_actions::moveLayer(*m_undoManager, currentStage(), identifier, index + delta);
        },
        Qt::QueuedConnection);
}

}  // namespace usdcc::usd
