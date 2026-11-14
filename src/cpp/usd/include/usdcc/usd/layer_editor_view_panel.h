#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/vt/value.h>

#include <QList>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QLabel;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace usdcc::usd {

class StageManager;
class UndoManager;
namespace properties {
class PropertyEditor;
class PropertyRow;
}  // namespace properties

// The stage's layer stack (layerTree(), docs/LAYER_EDITOR.md §2): the session
// layer, then the root layer, each with its sublayers, strongest first. A
// layer's name carries "*" for unsaved changes, "(in memory)" for an
// anonymous one, red "(not found)" for a missing one, and is dimmed when
// it's muted. The Target column marks the edit target; clicking it, a
// double-click or Set as Edit Target moves it there (§3). The Mute column's
// checkbox mutes a sublayer, one undo step (layer_actions::setMuted()). The
// toolbar says which layer is being edited, and its + adds a new in-memory
// layer, a new layer file or an existing one under the selected layer (§4).
// Remove (Delete), Move Up/Down (Ctrl+Up/Down) and dragging a sublayer among
// its siblings are undo steps too, as is Reload (a file layer fresh from
// disk, after a warning when it has unsaved changes). Follows the stage's change notices and
// edit target, coalesced.
class USDCC_USD_UI_API LayerEditorViewPanel : public usdcc::ui::ViewPanel {
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

    // Adds a layer under `parent` (docs/LAYER_EDITOR.md §4): a new in-memory
    // one, a new file or an existing one. Without `file` it asks for one
    // with a file dialog; an error is shown in a message box only then. With
    // "Edit the New Layer" (the + menu, saved) a new layer becomes the edit
    // target. Returns the new layer's identifier, empty if none was added.
    enum class AddKind { InMemory, NewFile, Existing };
    std::string addLayer(AddKind kind, const std::string& parent, const QString& file = QString());

    // Saving (§5): Save writes a file layer (an in-memory one goes to Save
    // As); Save As writes it to `file`, or one asked for, see
    // layer_actions::saveLayerAs(). Errors go to a message box.
    bool saveLayer(const std::string& identifier);
    std::string saveLayerAs(const std::string& identifier, const QString& file = QString());

    // The layer properties under the tree fold to their header (click it);
    // the panel's session state keeps it, with the splitter's sizes.
    bool isPropertiesCollapsed() const { return m_propertiesCollapsed; }
    void setPropertiesCollapsed(bool collapsed);

    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

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
    std::string selectedIdentifier() const;
    // Where + adds: the selected layer, else the root layer.
    std::string parentForAdd() const;
    void populateAddMenu(QMenu* menu, const std::string& parent);
    // Queued, as every edit from a menu or shortcut: its change notice
    // rebuilds the tree.
    void removeLayer(const std::string& identifier);
    void moveBy(const std::string& identifier, int delta);
    // Right-click > Reload: asks first when the layer has unsaved changes
    // (or the reload would clear the undo history), then reloads it.
    void reloadLayer(const std::string& identifier);
    // What Save and Ctrl+S save: the selected layer, else the edit target.
    std::string layerToSave() const;
    // The properties under the tree, for the selected layer (§6): read-only
    // identifier, file and format; its time offset and scale in its parent;
    // its default prim, time codes, comment and documentation, each edit one
    // undo step. Rebuilt for another layer, else only refilled.
    void updateProperties();
    void rebuildProperties(const LayerEntry* entry, bool sublayer);
    struct PropertyField {
        properties::PropertyRow* row;
        properties::PropertyEditor* editor;
        std::function<PXR_NS::VtValue(const LayerEntry&)> value;
        std::function<bool(const LayerEntry&)> authored;  // unauthored look when false
        std::function<void(const LayerEntry&)> update;    // anything else to refill (a warning, a button)
    };

    QTreeWidget* m_tree;
    QLabel* m_editing;
    QAction* m_editNewLayer;
    QSplitter* m_splitter;
    QWidget* m_propertiesPane;  // the header and the scroll area
    QToolButton* m_propertiesHeader;
    QScrollArea* m_propertiesScroll;
    bool m_propertiesCollapsed = false;
    QList<int> m_expandedSizes;  // the splitter's, from before it folded
    QWidget* m_properties;
    QVBoxLayout* m_propertiesLayout;
    std::string m_propertiesFor;
    std::vector<PropertyField> m_fields;
    std::string m_selectAfterRefresh;  // a layer just added or moved
    QTimer* m_refreshTimer;
    std::shared_ptr<UndoManager> m_undoManager;
    std::vector<LayerEntry> m_layers;
    bool m_filling = false;  // refresh() setting the checkboxes, not the user
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_editTargetConnection;
    usdcc::core::Signal<>::Connection m_layersSavedConnection;
};

}  // namespace usdcc::usd
