#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/dependencies.h"
#include "usdcc/usd/stage.h"

#include <memory>
#include <vector>

class QAction;
class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// Every file the stage uses (stageDependencies(), docs/COMPOSITION_ARCS.md
// §7): one row per file with its type, how many prims use it and its state
// (missing, unloaded), the prims (or, for a sublayer, the layers) under it.
// Missing files are red and come first. Double-click selects the prims; the
// right-click menu also opens a file as a stage, reveals it, copies its path
// and loads or unloads a payload everywhere it's used. "Show Asset
// Attributes" (options menu, saved) adds the files attributes name.
// Follows the stage's change notices, coalesced.
class USDCC_USD_UI_API DependenciesViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit DependenciesViewPanel(StageManager* stageManager, StageRefPtr stage,
                                   std::shared_ptr<UndoManager> undoManager,
                                   const QString& title = QStringLiteral("Dependencies"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // What the rows show, as of the last refresh.
    const std::vector<Dependency>& dependencies() const { return m_dependencies; }
    void setShowAssetAttributes(bool on);
    // The right-click menu of file `index` (into dependencies()).
    void populateFileMenu(QMenu* menu, int index);
    // Selects the prims using file `index`.
    void selectPrimsUsing(int index);

public Q_SLOTS:
    void refresh();

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void showMenu(const QPoint& pos);
    void onActivated(QTreeWidgetItem* item);
    void onStageObjectsChanged(const StageChanges& changes);

    QTreeWidget* m_tree;
    QLabel* m_summary;
    QAction* m_showAssetAttributes;
    QTimer* m_refreshTimer;
    std::shared_ptr<UndoManager> m_undoManager;
    std::vector<Dependency> m_dependencies;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

}  // namespace usdcc::usd
