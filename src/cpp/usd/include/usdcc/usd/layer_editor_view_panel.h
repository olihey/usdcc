#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/stage.h"

#include <memory>
#include <string>
#include <vector>

class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// The stage's layer stack (layerTree(), docs/LAYER_EDITOR.md §2): the session
// layer, then the root layer, each with its sublayers, strongest first. A
// layer's name carries "*" for unsaved changes, "(in memory)" for an
// anonymous one, red "(not found)" for a missing one, and is dimmed when
// it's muted. The Target column marks the edit target; clicking it, a
// double-click or Set as Edit Target moves it there (§3). The Mute column's
// checkbox mutes a sublayer, one undo step (layer_actions::setMuted()). The
// toolbar says which layer is being edited. Follows the stage's change
// notices and edit target, coalesced.
class LayerEditorViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    enum Column { Name, Target, Mute, ColumnCount };

    explicit LayerEditorViewPanel(StageManager* stageManager, StageRefPtr stage,
                                  std::shared_ptr<UndoManager> undoManager,
                                  const QString& title = QStringLiteral("Layers"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // The tree as of the last refresh, and the row showing `identifier`.
    const std::vector<LayerEntry>& layers() const { return m_layers; }
    QTreeWidgetItem* itemFor(const std::string& identifier) const;
    // The right-click menu of the layer `identifier`.
    void populateLayerMenu(QMenu* menu, const std::string& identifier);

public Q_SLOTS:
    void refresh();

protected:
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    const LayerEntry* entryFor(const std::string& identifier) const;
    void addItems(const LayerEntry& entry, QTreeWidgetItem* parent);
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemClicked(QTreeWidgetItem* item, int column);
    void showMenu(const QPoint& pos);
    void setEditTarget(const std::string& identifier);

    QTreeWidget* m_tree;
    QLabel* m_editing;
    QTimer* m_refreshTimer;
    std::shared_ptr<UndoManager> m_undoManager;
    std::vector<LayerEntry> m_layers;
    bool m_filling = false;  // refresh() setting the checkboxes, not the user
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_editTargetConnection;
};

}  // namespace usdcc::usd
