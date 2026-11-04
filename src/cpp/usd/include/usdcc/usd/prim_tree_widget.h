#pragma once

#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QWidget>

#include <set>

class QLineEdit;
class QSortFilterProxyModel;
class QTimer;
class QTreeView;

namespace usdcc::usd {

// A stage's prims as a read-only tree that looks like the Outliner (its model and
// delegate), with a name filter above it: Show Prims and the relationship target
// dialog. The filter keeps the prims whose name contains its text, and their
// ancestors, then restores what was open when it's cleared. An instance's contents
// load when it's expanded, so until then they can't match.
class PrimTreeWidget : public QWidget {
    Q_OBJECT

public:
    explicit PrimTreeWidget(QWidget* parent = nullptr);

    // The root prims start expanded.
    void setStage(const PXR_NS::UsdStageRefPtr& stage);
    // Rebuilds the tree, keeping what's open.
    void refresh();
    void setRowStatus(UsdPrimTreeModel::RowStatusFunction status);

    QTreeView* treeView() const { return m_tree; }
    PXR_NS::SdfPath selectedPath() const;
    // Selects it, opening its ancestors; false if it isn't in the tree.
    bool selectPath(const PXR_NS::SdfPath& path);

    QLineEdit* filterEdit() const { return m_filter; }
    // Filters at once (typing waits for a pause).
    void setFilter(const QString& text);
    bool isFiltering() const { return m_filtering; }
    // The prims the filter matches, not counting their ancestors.
    int matchCount() const;

Q_SIGNALS:
    void filterApplied();
    void selectionChanged();
    // A double-click on a row that can be selected.
    void rowActivated();

private:
    void applyFilter();
    std::set<PXR_NS::SdfPath> expandedPaths() const;
    void expandPaths(const std::set<PXR_NS::SdfPath>& paths);
    void expandRoots();

    UsdPrimTreeModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QLineEdit* m_filter;
    QTreeView* m_tree;
    QTimer* m_filterTimer;
    bool m_filtering = false;  // the proxy has a filter
    // What was open: across a rebuild, and while a filter opens everything.
    std::set<PXR_NS::SdfPath> m_expanded;
    std::set<PXR_NS::SdfPath> m_expandedBeforeFilter;
};

}  // namespace usdcc::usd
