#pragma once

#include "usdcc/core/export.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QAbstractItemModel>
#include <QString>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace usdcc::usd {

// Qt tree model over a UsdStage's prim hierarchy — includes inactive prims
// (shown visually dimmed, via a checkbox reflecting active state) rather
// than hiding them, since "disable" (deactivate) needs to stay visible and
// toggleable in the outliner. See docs/PLAN.md milestone M4. Each prim has
// its type's icon (prim_icons.h, docs/PRIM_ICONS.md) as Qt::DecorationRole.
//
// Names are coloured and weighted as usdview's prim browser does them
// (docs/OUTLINER_NAMES.md): instances light sky blue, prims with composition
// arcs dark yellow, instance proxies slate blue, others the theme's text
// colour, all darker when inactive; bold for a def, light italic for an
// over, normal for a class. The tooltip says the same in words.
//
// An instance's children are its instance proxies, as in usdview. They are
// read-only (no rename, no checkbox) and loaded only when the instance is
// first expanded (canFetchMore()/fetchMore()), since a heavily instanced
// stage would otherwise build the prototype's whole subtree under every
// instance; fetchPath() loads them down to a given path.
//
// Structural edits (rename/reorder) call refresh(), which rebuilds the
// whole tree via beginResetModel()/endResetModel() rather than incremental
// inserts/removes/moves: simpler and safer than tracking exact index shifts
// through USD's own composition/namespace-edit machinery. A reset drops the
// view's QModelIndex-based state (expansion, selection); OutlinerViewPanel
// saves it by prim path around every reset and restores it, and only
// refreshes when the hierarchy actually changed.
class USDCC_USD_UI_API UsdPrimTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit UsdPrimTreeModel(QObject* parent = nullptr);

    void setStage(const PXR_NS::UsdStageRefPtr& stage);
    const PXR_NS::UsdStageRefPtr& stage() const { return m_stage; }
    // Only to look at (Show Prims): no rename, no active checkbox.
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    // Asked for each row when one is needed (a relationship's target dialog): one
    // not usable is dimmed and can't be selected, a warning amber, each with the
    // note in its tooltip.
    struct RowStatus {
        bool usable = true;
        bool warning = false;
        QString note;
    };
    using RowStatusFunction = std::function<RowStatus(const PXR_NS::UsdPrim&)>;
    void setRowStatus(RowStatusFunction status) { m_rowStatus = std::move(status); }
    // Rebuilds the tree from the stage's current state. Call after any
    // structural edit (rename, reorder, add/remove prim) made outside this
    // model's own setData() (which already refreshes after renaming).
    void refresh();

    // Tints the ancestors of these (selected) prims, so a selection deep in a
    // collapsed or long tree can be traced from the top. Kept by path, so it
    // survives refresh().
    void setSelectedPaths(const std::vector<PXR_NS::SdfPath>& paths);
    bool isSelectionAncestor(const PXR_NS::SdfPath& path) const { return m_selectionAncestors.count(path) > 0; }

    PXR_NS::SdfPath pathForIndex(const QModelIndex& index) const;
    // Only finds rows already loaded; fetchPath() loads instance proxies
    // down to `path` first.
    QModelIndex indexForPath(const PXR_NS::SdfPath& path) const;
    QModelIndex fetchPath(const PXR_NS::SdfPath& path);

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    // Dragging rows (docs/OUTLINER_REPARENT.md). A drop doesn't change the tree: it asks,
    // as a rename does, and the stage's change notice rebuilds it.
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDropActions() const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

Q_SIGNALS:
    // `row` is where among `parent`'s children, or -1 for on it.
    void dropRequested(const std::vector<PXR_NS::SdfPath>& paths, const PXR_NS::SdfPath& parent, int row);
    // While hovering: why this drop can't happen, or empty when it can.
    void dropHint(const QString& why) const;
    // The row's checkbox and its name editor ask for these rather than editing
    // the stage, so the panel can make each an undo step (prim_actions.h).
    void activeChangeRequested(const PXR_NS::SdfPath& path, bool active);
    void renameRequested(const PXR_NS::SdfPath& path, const std::string& name);

private:
    struct Node {
        PXR_NS::SdfPath path;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        // An instance whose instance proxies aren't loaded yet.
        bool unfetched = false;
    };

    void buildChildren(Node* node);
    Node* nodeForIndex(const QModelIndex& index) const;
    QModelIndex indexForNode(Node* node) const;
    Node* findNode(Node* node, const PXR_NS::SdfPath& path) const;

    PXR_NS::UsdStageRefPtr m_stage;
    std::unique_ptr<Node> m_root;
    std::set<PXR_NS::SdfPath> m_selectionAncestors;
    bool m_readOnly = false;
    RowStatusFunction m_rowStatus;
};

}  // namespace usdcc::usd
