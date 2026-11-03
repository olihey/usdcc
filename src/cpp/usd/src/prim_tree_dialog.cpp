#include "usdcc/usd/prim_tree_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/outliner_item_delegate.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace usdcc::usd {

namespace {
constexpr auto kSizeKey = "PrimTreeDialog/size";
}  // namespace

PrimTreeDialog* showPrimTree(const QString& path, QWidget* parent) {
    if (path.isEmpty() || !canPreviewUsda(path)) return nullptr;
    if (PrimTreeDialog* open = raiseOpenPreview<PrimTreeDialog>(path)) return open;
    auto* dialog = new PrimTreeDialog(path, previewOwner(parent));
    dialog->show();
    return dialog;
}

PrimTreeDialog::PrimTreeDialog(const QString& path, QWidget* parent) : QDialog(parent), m_path(path) {
    setUpPreviewWindow(this);
    const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(path.toStdString());
    const QString name = open ? QString::fromStdString(open->GetDisplayName()) : QFileInfo(path).fileName();
    setWindowTitle(tr("Show Prims - %1").arg(name));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* filterBar = new QWidget(this);
    filterBar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    filterBar->setAttribute(Qt::WA_StyledBackground);
    auto* filterLayout = new QHBoxLayout(filterBar);
    usdcc::ui::scaled::margins(filterLayout, 4, 4, 4, 4);
    m_filter = new QLineEdit(filterBar);
    m_filter->setPlaceholderText(tr("Filter by name"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setEnabled(false);  // until there's something to filter
    filterLayout->addWidget(m_filter);
    layout->addWidget(filterBar);

    m_model = new UsdPrimTreeModel(this);
    m_model->setReadOnly(true);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setRecursiveFilteringEnabled(true);  // a match keeps its ancestors
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setItemDelegate(new OutlinerItemDelegate(m_tree));
    usdcc::ui::scaled::iconSize(m_tree, 16, 16);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);  // keeps your place; nothing reacts
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(tr("Loading stage..."), this);
    m_status->setObjectName(QStringLiteral("propertyNote"));
    m_status->setContentsMargins(8, 2, 8, 2);
    layout->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(this);
    usdcc::ui::scaled::margins(buttons->layout(), 8, 6, 8, 8);
    m_loadPayloads = buttons->addButton(tr("Load Payloads"), QDialogButtonBox::ActionRole);
    QPushButton* closeButton = buttons->addButton(QDialogButtonBox::Close);
    m_loadPayloads->setAutoDefault(false);
    closeButton->setAutoDefault(false);
    m_loadPayloads->setEnabled(false);
    connect(m_loadPayloads, &QPushButton::clicked, this, [this]() {
        if (!m_stage) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_stage->usdStage()->Load();  // its change notice rebuilds the tree
        QApplication::restoreOverrideCursor();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);
    layout->addWidget(buttons);

    // A rebuild keeps what was open.
    connect(m_proxy, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        if (!m_filtering) m_expanded = expandedPaths();
    });
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this]() {
        if (m_filtering) {
            m_tree->expandAll();
        } else {
            expandPaths(m_expanded);
        }
    });

    // After typing pauses.
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(200);
    connect(m_filter, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_filterTimer, &QTimer::timeout, this, &PrimTreeDialog::applyFilter);

    const QSize saved = usdcc::ui::appSettings().value(kSizeKey).toSize();
    resize(saved.isValid() ? saved : usdcc::ui::scaled::size(480, 640));

    loadPreviewStage(path, this,
                     [this](const StageRefPtr& stage, const QString& error) { onStageLoaded(stage, error); });
}

void PrimTreeDialog::onStageLoaded(const StageRefPtr& stage, const QString& error) {
    m_loaded = true;
    if (!stage) {
        m_status->setText(error.isEmpty() ? tr("Couldn't load it as a stage")
                                          : tr("Couldn't load it as a stage: %1").arg(error));
        m_status->setWordWrap(true);
        return;
    }
    m_stage = stage;
    m_model->setStage(stage->usdStage());
    expandRoots();
    m_filter->setEnabled(true);
    // Layers are shared with the app's stages, so its edits can recompose this
    // one; only the ones that touch the hierarchy rebuild the tree.
    m_stageChangedConnection = stage->changed.connect([this](const StageChanges& changes) {
        const bool hierarchy = std::any_of(changes.resyncedPaths.begin(), changes.resyncedPaths.end(),
                                           [](const PXR_NS::SdfPath& p) { return p.IsPrimPath() || p.IsAbsoluteRootPath(); });
        if (!hierarchy || m_refreshQueued) return;
        m_refreshQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_refreshQueued = false;
            m_model->refresh();
            updateStatus();
        });
    });
    updateStatus();
}

void PrimTreeDialog::setFilter(const QString& text) {
    m_filter->setText(text);
    m_filterTimer->stop();
    applyFilter();
}

void PrimTreeDialog::applyFilter() {
    const QString text = m_filter->text();
    const bool filtering = !text.isEmpty();
    if (filtering && !m_filtering) m_expandedBeforeFilter = expandedPaths();
    const bool wasFiltering = m_filtering;
    m_filtering = filtering;
    m_proxy->setFilterFixedString(text);
    if (filtering) {
        m_tree->expandAll();  // every match shows
    } else if (wasFiltering) {
        m_tree->collapseAll();
        expandPaths(m_expandedBeforeFilter);
    }
    updateStatus();
}

int PrimTreeDialog::countMatches() const {
    // Not the rows shown: those include the matches' ancestors.
    const QString text = m_filter->text();
    int matches = 0;
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (index.data().toString().contains(text, Qt::CaseInsensitive)) ++matches;
            pending.push_back(index);
        }
    }
    return matches;
}

void PrimTreeDialog::updateStatus() {
    if (!m_stage) return;
    const PXR_NS::UsdStageRefPtr& stage = m_stage->usdStage();
    int prims = 0;
    for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange::Stage(stage, PXR_NS::UsdPrimAllPrimsPredicate)) {
        (void)prim;
        ++prims;
    }
    const QLocale locale;
    const QString dot = QStringLiteral("  \xC2\xB7  ");
    QString status = prims == 1 ? tr("1 prim") : tr("%1 prims").arg(locale.toString(prims));
    const std::size_t loadable = stage->FindLoadable().size();
    const std::size_t loaded = stage->GetLoadSet().size();
    if (loaded < loadable) {
        const int missing = int(loadable - loaded);
        status += dot + (missing == 1 ? tr("1 payload not loaded") : tr("%1 payloads not loaded").arg(locale.toString(missing)));
    }
    m_loadPayloads->setEnabled(loaded < loadable);
    if (m_filtering) {
        const int matches = countMatches();
        status = (matches == 1 ? tr("1 match") : tr("%1 matches").arg(locale.toString(matches))) + dot + status;
        if (!stage->GetPrototypes().empty()) status += dot + tr("instances: expand to search inside");
    }
    m_status->setText(status);
}

std::set<PXR_NS::SdfPath> PrimTreeDialog::expandedPaths() const {
    std::set<PXR_NS::SdfPath> paths;
    std::vector<QModelIndex> pending{QModelIndex()};
    while (!pending.empty()) {
        const QModelIndex parent = pending.back();
        pending.pop_back();
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (!m_tree->isExpanded(index)) continue;  // what's under it is hidden anyway
            paths.insert(m_model->pathForIndex(m_proxy->mapToSource(index)));
            pending.push_back(index);
        }
    }
    return paths;
}

void PrimTreeDialog::expandPaths(const std::set<PXR_NS::SdfPath>& paths) {
    for (const PXR_NS::SdfPath& path : paths) {
        const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(path));
        if (index.isValid()) m_tree->setExpanded(index, true);
    }
}

void PrimTreeDialog::expandRoots() {
    for (int row = 0; row < m_proxy->rowCount(); ++row) m_tree->expand(m_proxy->index(row, 0));
}

void PrimTreeDialog::reject() {
    if (m_filter->text().isEmpty()) {
        QDialog::reject();
    } else {
        setFilter(QString());
    }
}

void PrimTreeDialog::done(int result) {
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kSizeKey, size());
    QDialog::done(result);
}

}  // namespace usdcc::usd
