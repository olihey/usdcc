#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QAbstractItemModel>

#include <memory>
#include <vector>

namespace usdcc::usd {

// Qt tree model over a UsdStage's prim hierarchy — includes inactive prims
// (shown visually dimmed, via a checkbox reflecting active state) rather
// than hiding them, since "disable" (deactivate) needs to stay visible and
// toggleable in the outliner. See docs/PLAN.md milestone M4.
//
// Structural edits (rename/reorder) call refresh(), which rebuilds the
// whole tree via beginResetModel()/endResetModel() rather than incremental
// inserts/removes/moves: simpler and safer than tracking exact index shifts
// through USD's own composition/namespace-edit machinery, at the cost of
// losing transient QModelIndex-based view state (e.g. expansion) across an
// edit. Activating/deactivating a prim (no structural change) only emits
// dataChanged() and does not need a full refresh.
class UsdPrimTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit UsdPrimTreeModel(QObject* parent = nullptr);

    void setStage(const PXR_NS::UsdStageRefPtr& stage);
    // Rebuilds the tree from the stage's current state. Call after any
    // structural edit (rename, reorder, add/remove prim) made outside this
    // model's own setData() (which already refreshes after renaming).
    void refresh();

    PXR_NS::SdfPath pathForIndex(const QModelIndex& index) const;
    QModelIndex indexForPath(const PXR_NS::SdfPath& path) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

signals:
    // Emitted when the user renames a prim via this model (Qt::EditRole) —
    // the panel listens to fix up the shared selection if the renamed prim
    // was selected (its path changed).
    void primRenamed(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath);

private:
    struct Node {
        PXR_NS::SdfPath path;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

    void buildChildren(Node* node);
    Node* findNode(Node* node, const PXR_NS::SdfPath& path) const;

    PXR_NS::UsdStageRefPtr m_stage;
    std::unique_ptr<Node> m_root;
};

}  // namespace usdcc::usd
