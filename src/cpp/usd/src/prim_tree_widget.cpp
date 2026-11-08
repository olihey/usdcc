#include "usdcc/usd/prim_tree_widget.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/outliner_item_delegate.h"
#include "usdcc/usd/prim_tree_filter.h"

#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QTreeView>
#include <QVBoxLayout>

#include <utility>
#include <vector>

namespace usdcc::usd {

PrimTreeWidget::PrimTreeWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_model = new UsdPrimTreeModel(this);
    m_model->setReadOnly(true);
    m_proxy = new PrimTreeFilterProxy(this);
    m_proxy->setPrimModel(m_model);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setItemDelegate(new OutlinerItemDelegate(m_tree));
    usdcc::ui::scaled::iconSize(m_tree, 16, 16);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* filterBar = new QWidget(this);
    filterBar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    filterBar->setAttribute(Qt::WA_StyledBackground);
    auto* filterLayout = new QHBoxLayout(filterBar);
    usdcc::ui::scaled::margins(filterLayout, 4, 4, 4, 4);
    m_filterBar = new PrimTreeFilterBar(m_tree, m_proxy, filterBar);
    filterLayout->addWidget(m_filterBar);
    layout->addWidget(filterBar);
    layout->addWidget(m_tree, 1);

    connect(m_filterBar, &PrimTreeFilterBar::filterApplied, this, &PrimTreeWidget::filterApplied);
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { Q_EMIT selectionChanged(); });
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.flags() & Qt::ItemIsSelectable) Q_EMIT rowActivated();
    });
    // A rebuild keeps what was open.
    connect(m_proxy, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        if (!isFiltering()) m_expanded = expandedPaths();
    });
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this]() {
        if (isFiltering()) {
            m_filterBar->reapply();
        } else {
            expandPaths(m_expanded);
        }
    });
}

void PrimTreeWidget::setStage(const PXR_NS::UsdStageRefPtr& stage) {
    m_model->setStage(stage);
    expandRoots();
}

void PrimTreeWidget::refresh() { m_model->refresh(); }

void PrimTreeWidget::setRowStatus(UsdPrimTreeModel::RowStatusFunction status) { m_model->setRowStatus(std::move(status)); }

QLineEdit* PrimTreeWidget::filterEdit() const { return m_filterBar->searchEdit(); }

PXR_NS::SdfPath PrimTreeWidget::selectedPath() const {
    const QModelIndexList rows = m_tree->selectionModel()->selectedRows(0);
    return rows.isEmpty() ? PXR_NS::SdfPath() : m_model->pathForIndex(m_proxy->mapToSource(rows.front()));
}

bool PrimTreeWidget::selectPath(const PXR_NS::SdfPath& path) {
    const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(path));
    if (!index.isValid()) return false;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) m_tree->expand(parent);
    m_tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_tree->scrollTo(index);
    return true;
}

void PrimTreeWidget::setFilter(const QString& text) { m_filterBar->setNameFilter(text); }
void PrimTreeWidget::clearFilter() { m_filterBar->clear(); }
bool PrimTreeWidget::isFiltering() const { return m_filterBar->isFiltering(); }
int PrimTreeWidget::matchCount() const { return m_proxy->matchCount(); }

std::set<PXR_NS::SdfPath> PrimTreeWidget::expandedPaths() const {
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

void PrimTreeWidget::expandPaths(const std::set<PXR_NS::SdfPath>& paths) {
    for (const PXR_NS::SdfPath& path : paths) {
        const QModelIndex index = m_proxy->mapFromSource(m_model->fetchPath(path));
        if (index.isValid()) m_tree->setExpanded(index, true);
    }
}

void PrimTreeWidget::expandRoots() {
    for (int row = 0; row < m_proxy->rowCount(); ++row) m_tree->expand(m_proxy->index(row, 0));
}

}  // namespace usdcc::usd
