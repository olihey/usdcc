#include "usdcc/usd/layer_editor_view_panel.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QSet>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

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

    m_tree = new QTreeWidget(container);
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

    layout->addWidget(toolbar);
    layout->addWidget(m_tree, 1);
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
    menu->addAction(tr("Refresh"), this, &LayerEditorViewPanel::refresh);
}

void LayerEditorViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    m_editTargetConnection = {};
    if (stage) {
        // Any edit can make a layer dirty; muting and sublayer changes resync.
        m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) { m_refreshTimer->start(); });
        m_editTargetConnection = stage->editTargetChanged.connect([this]() { m_refreshTimer->start(); });
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
        if (id == selected) (*it)->setSelected(true);
    }
    m_filling = false;

    const LayerEntry* target = findTarget(m_layers);
    m_editing->setText(target ? tr("Editing: %1").arg(qs(target->displayName)) : QString());
    m_editing->setToolTip(target ? qs(target->identifier) : QString());
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
}

}  // namespace usdcc::usd
