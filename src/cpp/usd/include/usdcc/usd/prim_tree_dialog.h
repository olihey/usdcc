#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/preview_stage.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QDialog>
#include <QString>

#include <set>

class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTimer;
class QTreeView;

namespace usdcc::usd {

class UsdPrimTreeModel;

// Show Prims (docs/SHOW_PRIMS.md): a file's prims as a tree that looks like the
// Outliner (its model and delegate, read-only), to look at and nothing else: no
// menus, and the app's selection stays out of it. A non-modal dialog with a
// stage of its own, as in Show USDA, loaded behind "Loading stage..." without
// payloads (Load Payloads loads them). The filter keeps the prims whose name
// contains its text, and their ancestors. An instance's contents load when
// it's expanded, so until then they can't match.
class PrimTreeDialog : public QDialog {
    Q_OBJECT

public:
    // `path`: a file, or an open layer's identifier.
    explicit PrimTreeDialog(const QString& path, QWidget* parent = nullptr);

    const QString& path() const { return m_path; }
    // False until the stage has loaded, or failed to.
    bool isStageLoaded() const { return m_loaded; }
    const StageRefPtr& stage() const { return m_stage; }
    QTreeView* treeView() const { return m_tree; }
    QLabel* statusLabel() const { return m_status; }
    // Filters at once (typing waits for a pause).
    void setFilter(const QString& text);

public Q_SLOTS:
    // Esc: clears the filter first, then closes.
    void reject() override;
    void done(int result) override;

private:
    void onStageLoaded(const StageRefPtr& stage, const QString& error);
    void applyFilter();
    void updateStatus();
    std::set<PXR_NS::SdfPath> expandedPaths() const;
    void expandPaths(const std::set<PXR_NS::SdfPath>& paths);
    void expandRoots();
    int countMatches() const;

    QString m_path;
    StageRefPtr m_stage;
    UsdPrimTreeModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QLineEdit* m_filter;
    QTreeView* m_tree;
    QLabel* m_status;
    QPushButton* m_loadPayloads;
    QTimer* m_filterTimer;
    bool m_loaded = false;
    bool m_refreshQueued = false;
    bool m_filtering = false;  // the proxy has a filter
    // What was open: across a rebuild, and while a filter opens everything.
    std::set<PXR_NS::SdfPath> m_expanded;
    std::set<PXR_NS::SdfPath> m_expandedBeforeFilter;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

// Shows `path` in a dialog over `parent`'s window, or brings forward the one
// already showing it. Null if it can't be shown.
PrimTreeDialog* showPrimTree(const QString& path, QWidget* parent);

}  // namespace usdcc::usd
