#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/preview_stage.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QTreeView;

namespace usdcc::usd {

class PrimTreeWidget;

// Show Prims (docs/SHOW_PRIMS.md): a file's prims as a tree that looks like the
// Outliner (a PrimTreeWidget), to look at and nothing else: no menus, and the
// app's selection stays out of it. A non-modal dialog with a stage of its own, as
// in Show USDA, loaded behind "Loading stage..." without payloads (Load Payloads
// loads them).
class USDCC_USD_UI_API PrimTreeDialog : public QDialog {
    Q_OBJECT

public:
    // `path`: a file, or an open layer's identifier.
    explicit PrimTreeDialog(const QString& path, QWidget* parent = nullptr);

    const QString& path() const { return m_path; }
    // False until the stage has loaded, or failed to.
    bool isStageLoaded() const { return m_loaded; }
    const StageRefPtr& stage() const { return m_stage; }
    QTreeView* treeView() const;
    QLabel* statusLabel() const { return m_status; }
    // Filters at once (typing waits for a pause).
    void setFilter(const QString& text);
    // Selects the prim at `path` (opening its ancestors) now, or once the
    // stage has loaded.
    void selectWhenLoaded(const PXR_NS::SdfPath& path);
    PXR_NS::SdfPath selectedPath() const;

public Q_SLOTS:
    // Esc: clears the filter first, then closes.
    void reject() override;
    void done(int result) override;

private:
    void onStageLoaded(const StageRefPtr& stage, const QString& error);
    void updateStatus();

    QString m_path;
    StageRefPtr m_stage;
    PrimTreeWidget* m_tree;
    QLabel* m_status;
    QPushButton* m_loadPayloads;
    bool m_loaded = false;
    bool m_refreshQueued = false;
    PXR_NS::SdfPath m_selectOnLoad;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

// Shows `path` in a dialog over `parent`'s window, or brings forward the one
// already showing it. Null if it can't be shown.
PrimTreeDialog* showPrimTree(const QString& path, QWidget* parent);

}  // namespace usdcc::usd
