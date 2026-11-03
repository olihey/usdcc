#include "usdcc/usd/dependencies_view_panel.h"

#include "usdcc/ui/settings.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/usda_preview_dialog.h"

#include <pxr/usd/sdf/layer.h>
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
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

namespace {
constexpr auto kShowAssetAttributesKey = "Dependencies/showAssetAttributes";
const QColor kMissingColor(0xff, 0x5f, 0x56);  // as the log's errors
// Top-level rows: the file's index into dependencies(). Their children: the prim path.
constexpr int kIndexRole = Qt::UserRole;
constexpr int kPrimRole = Qt::UserRole + 1;

enum Column { File, Type, UsedBy, State, Path, ColumnCount };

QString qs(const std::string& text) { return QString::fromStdString(text); }

// "1 prim", "3 prims" (tr()'s %n plurals need a translation loaded).
QString count(int n, const QString& one, const QString& many) { return (n == 1 ? one : many).arg(n); }

QString layerName(const std::string& identifier) {
    if (const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(identifier)) return qs(layer->GetDisplayName());
    return QFileInfo(qs(identifier)).fileName();
}
}  // namespace

DependenciesViewPanel::DependenciesViewPanel(StageManager* stageManager, StageRefPtr stage,
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
    m_summary = new QLabel(toolbar);
    toolbarLayout->addWidget(m_summary);
    toolbarLayout->addStretch(1);

    m_showAssetAttributes = new QAction(tr("Show Asset Attributes"), this);
    m_showAssetAttributes->setToolTip(tr("Also list the files asset attributes name, such as textures"));
    m_showAssetAttributes->setCheckable(true);
    m_showAssetAttributes->setChecked(usdcc::ui::appSettings().value(kShowAssetAttributesKey, false).toBool());
    connect(m_showAssetAttributes, &QAction::toggled, this, [this](bool on) {
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(kShowAssetAttributesKey, on);
        refresh();
    });

    m_tree = new QTreeWidget(container);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("File"), tr("Type"), tr("Used by"), tr("State"), tr("Path")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &DependenciesViewPanel::showMenu);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { onActivated(item); });

    layout->addWidget(toolbar);
    layout->addWidget(m_tree, 1);
    setContentWidget(container);

    // A single edit can send several notices, and a drag many.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout, this, &DependenciesViewPanel::refresh);

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* DependenciesViewPanel::duplicate(QWidget* parent) const {
    return new DependenciesViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

void DependenciesViewPanel::setShowAssetAttributes(bool on) { m_showAssetAttributes->setChecked(on); }

void DependenciesViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    menu->addAction(m_showAssetAttributes);
    menu->addAction(tr("Refresh"), this, &DependenciesViewPanel::refresh);
}

void DependenciesViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    if (stage) {
        m_stageChangedConnection =
            stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
    }
    refresh();
}

void DependenciesViewPanel::onStageObjectsChanged(const StageChanges& changes) {
    // Arcs and sublayers change by resync; an asset attribute's value doesn't.
    if (!changes.resyncedPaths.empty() || (m_showAssetAttributes->isChecked() && !changes.changedInfoOnlyPaths.empty())) {
        m_refreshTimer->start();
    }
}

void DependenciesViewPanel::refresh() {
    m_refreshTimer->stop();
    // Keep which files were expanded, by where they are.
    QSet<QString> expanded;
    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        if (QTreeWidgetItem* item = m_tree->topLevelItem(row); item->isExpanded()) expanded.insert(item->text(Path));
    }
    m_tree->clear();
    m_dependencies = currentStage() ? stageDependencies(currentStage()->usdStage(), m_showAssetAttributes->isChecked())
                                    : std::vector<Dependency>();

    int missing = 0;
    for (int index = 0; index < int(m_dependencies.size()); ++index) {
        const Dependency& file = m_dependencies[index];
        const QString path = QDir::toNativeSeparators(qs(file.resolvedPath));
        auto* item = new QTreeWidgetItem(m_tree);
        item->setData(File, kIndexRole, index);
        item->setText(File, QFileInfo(qs(file.resolvedPath)).fileName());
        item->setText(Type, qs(dependencyKindNames(file.kinds)));
        const bool sublayerOnly = file.kinds == Dependency::Sublayer;
        const int users = int(sublayerOnly ? file.layers.size() : file.prims.size());
        item->setText(UsedBy, sublayerOnly ? count(users, tr("%1 layer"), tr("%1 layers"))
                                            : count(users, tr("%1 prim"), tr("%1 prims")));
        QStringList state;
        if (file.missing) state << tr("missing");
        if (!file.unloaded.empty()) state << tr("%1 unloaded").arg(file.unloaded.size());
        item->setText(State, state.join(QStringLiteral(", ")));
        item->setText(Path, path);

        QStringList authoredIn;
        for (const std::string& layer : file.layers) authoredIn << layerName(layer);
        item->setToolTip(File, tr("%1\nAs authored: %2\nIn: %3")
                                   .arg(file.missing ? tr("Not found: %1").arg(path) : path, qs(file.assetPath),
                                        authoredIn.join(QStringLiteral(", "))));
        if (file.missing) {
            ++missing;
            for (int column = 0; column < ColumnCount; ++column) item->setForeground(column, kMissingColor);
        }

        if (sublayerOnly) {
            for (const std::string& layer : file.layers) {
                auto* child = new QTreeWidgetItem(item, {layerName(layer)});
                child->setToolTip(File, qs(layer));
            }
        } else {
            for (const PXR_NS::SdfPath& prim : file.prims) {
                auto* child = new QTreeWidgetItem(item, {qs(prim.GetString())});
                child->setData(File, kPrimRole, qs(prim.GetString()));
                if (std::binary_search(file.unloaded.begin(), file.unloaded.end(), prim)) child->setText(State, tr("unloaded"));
            }
        }
        item->setExpanded(expanded.contains(path));
    }
    for (int column = 0; column < Path; ++column) m_tree->resizeColumnToContents(column);

    const int files = int(m_dependencies.size());
    const QString summary = count(files, tr("%1 file"), tr("%1 files"));
    m_summary->setText(missing ? tr("%1, %2 missing").arg(summary).arg(missing) : summary);
}

void DependenciesViewPanel::selectPrimsUsing(int index) {
    if (!currentStage() || index < 0 || index >= int(m_dependencies.size())) return;
    if (!m_dependencies[index].prims.empty()) stageManager()->setSelectedPaths(currentStage(), m_dependencies[index].prims);
}

void DependenciesViewPanel::onActivated(QTreeWidgetItem* item) {
    if (!item || !currentStage()) return;
    if (!item->parent()) {
        selectPrimsUsing(item->data(File, kIndexRole).toInt());
    } else if (const QString prim = item->data(File, kPrimRole).toString(); !prim.isEmpty()) {
        stageManager()->setSelectedPaths(currentStage(), {PXR_NS::SdfPath(prim.toStdString())});
    }
}

void DependenciesViewPanel::showMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item) return;
    QMenu menu(this);
    if (item->parent() && !item->data(File, kPrimRole).toString().isEmpty()) {
        menu.addAction(tr("Select Prim"), this, [this, item]() { onActivated(item); });
        menu.addSeparator();
        item = item->parent();
    } else if (item->parent()) {
        item = item->parent();
    }
    populateFileMenu(&menu, item->data(File, kIndexRole).toInt());
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void DependenciesViewPanel::populateFileMenu(QMenu* menu, int index) {
    if (index < 0 || index >= int(m_dependencies.size())) return;
    const Dependency& file = m_dependencies[index];
    const QString path = qs(file.resolvedPath);
    const StageRefPtr stage = currentStage();

    QAction* select = menu->addAction(tr("Select Prims Using It"), this, [this, index]() { selectPrimsUsing(index); });
    select->setEnabled(!file.prims.empty());

    QAction* open = menu->addAction(tr("Open as Stage"), this, [path]() { openStageFile(path); });
    // An asset attribute's file (a texture, ...) is rarely a layer.
    open->setEnabled(!file.missing && (file.kinds & ~unsigned(Dependency::Asset)));
    QAction* show = menu->addAction(tr("Show USDA"), this, [this, path]() { showUsdaPreview(path, this); });
    show->setEnabled(open->isEnabled());
    QAction* prims = menu->addAction(tr("Show Prims"), this, [this, path]() { showPrimTree(path, this); });
    prims->setEnabled(open->isEnabled());
    QAction* reveal = menu->addAction(tr("Reveal in Explorer"), this, [path]() { revealFile(path); });
    reveal->setEnabled(!file.missing && QFileInfo::exists(path));

    menu->addSeparator();
    menu->addAction(tr("Copy Path"), this,
                    [path]() { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    const QString assetPath = qs(file.assetPath);
    menu->addAction(tr("Copy Asset Path"), this, [assetPath]() { QApplication::clipboard()->setText(assetPath); });

    if (!(file.kinds & Dependency::Payload) || !stage) return;
    std::vector<PXR_NS::SdfPath> loaded;
    for (const PXR_NS::SdfPath& prim : file.prims) {
        if (prim_actions::canUnload(stage, prim)) loaded.push_back(prim);
    }
    menu->addSeparator();
    const std::vector<PXR_NS::SdfPath> unloaded = file.unloaded;
    QAction* load = menu->addAction(tr("Load Everywhere"), this, [this, stage, unloaded]() {
        prim_actions::load(*m_undoManager, stage, unloaded);
    });
    load->setEnabled(!unloaded.empty());
    QAction* unload = menu->addAction(tr("Unload Everywhere"), this, [this, stage, loaded]() {
        prim_actions::unload(*m_undoManager, stage, loaded);
    });
    unload->setEnabled(!loaded.empty());
}

}  // namespace usdcc::usd
