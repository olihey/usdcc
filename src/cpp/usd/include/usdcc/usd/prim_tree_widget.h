#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QWidget>

#include <set>

class QLineEdit;
class QTimer;
class QTreeView;

namespace usdcc::usd {

class PrimTreeFilterBar;
class PrimTreeFilterProxy;

// A stage's prims as a read-only tree that looks like the Outliner (its model and
// delegate), with a search and a type filter above it (PrimTreeFilterBar): Show Prims and
// the relationship target dialog. They keep the prims that match, and their
// ancestors, then restore what was open when cleared. An instance's contents
// load when it's expanded, so until then they can't match.
class USDCC_USD_UI_API PrimTreeWidget : public QWidget {
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

    QLineEdit* filterEdit() const;
    PrimTreeFilterBar* filterBar() const { return m_filterBar; }
    // By name, at once (typing waits for a pause). The bar also filters by type.
    void setFilter(const QString& text);
    // Name and types both.
    void clearFilter();
    bool isFiltering() const;
    // The prims the filter matches, not counting their ancestors.
    int matchCount() const;

Q_SIGNALS:
    void filterApplied();
    void selectionChanged();
    // A double-click on a row that can be selected.
    void rowActivated();

private:
    std::set<PXR_NS::SdfPath> expandedPaths() const;
    void expandPaths(const std::set<PXR_NS::SdfPath>& paths);
    void expandRoots();

    UsdPrimTreeModel* m_model;
    PrimTreeFilterProxy* m_proxy;
    PrimTreeFilterBar* m_filterBar;
    QTreeView* m_tree;
    // What was open across a rebuild.
    std::set<PXR_NS::SdfPath> m_expanded;
};

}  // namespace usdcc::usd
