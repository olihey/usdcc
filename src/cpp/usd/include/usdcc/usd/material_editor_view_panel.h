#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/shade_graph.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QElapsedTimer>
#include <QPointF>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}  // namespace QtNodes

namespace usdcc::usd {

class ShadeBackdropItem;
class ShaderNodePalette;
class ShaderNodeSearchPopup;
class StageManager;
class UndoManager;
class UsdShadeGraphModel;

// A material's shading network as a node graph (docs/MATERIAL_EDITOR.md §4). It follows the
// selection both ways, and a pin keeps it on what it's showing. A NodeGraph is one node until
// double-clicked, which enters it; a breadcrumb walks back up. Every edit is one undo step.
class USDCC_USD_UI_API MaterialEditorViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    MaterialEditorViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo, const QString& title,
                            QWidget* parent = nullptr);
    ~MaterialEditorViewPanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // Shows a Material's or a NodeGraph's insides.
    void showLevel(const PXR_NS::SdfPath& level);
    const PXR_NS::SdfPath& shownLevel() const { return m_level; }

    void setPinned(bool pinned);
    bool isPinned() const;

    // The edits, on what's selected, as the keys and menus do them.
    PXR_NS::SdfPath addNode(const QString& identifier, QPointF scenePosition);
    PXR_NS::SdfPath addNodeAtMouse(const QString& identifier);
    bool deleteSelected();
    void duplicateSelected();
    void copySelected();
    void cutSelected();
    void paste();
    void groupSelected();
    void ungroupSelected();
    void autoLayout();
    PXR_NS::SdfPath addBackdrop(QPointF scenePosition);
    // An edit over the node's caption: Return renames it, Escape doesn't.
    void startRename();
    // Tab's.
    ShaderNodeSearchPopup* showNodeSearch();
    void setPaletteShown(bool shown);
    // What a right-click there offers, on what's under it.
    void populateGraphMenu(QMenu* menu, QPointF scenePosition);

    QtNodes::GraphicsView* view() const { return m_view; }
    QtNodes::BasicGraphicsScene* scene() const { return m_scene.get(); }
    UsdShadeGraphModel* model() const { return m_model.get(); }
    ShaderNodePalette* palette() const { return m_palette; }
    QLineEdit* inlineEdit() const { return m_inlineEdit; }
    const std::vector<ShadeBackdropItem*>& backdropItems() const { return m_backdropItems; }
    // The breadcrumb's prim names, outermost first.
    QStringList breadcrumb() const;
    // Whether it's saying there's nothing to show.
    bool isEmpty() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void attachStage();
    void pickLevel();
    void fitGraph(double smallest);
    void rebuildBreadcrumb();
    void selectionFromStage(const std::vector<PXR_NS::SdfPath>& paths);
    void selectionToStage();
    void showSelectionInGraph();
    bool pressOnPage(QPoint viewportPoint);
    bool handleKey(QKeyEvent* event, bool run);
    bool handleViewportEvent(QEvent* event);
    std::vector<PXR_NS::SdfPath> selectedPrims() const;
    void rebuildBackdropItems();
    void startInlineEdit(const QString& text, const QRectF& sceneRect, const std::function<void(const QString&)>& commit);
    QPointF mouseScenePosition() const;
    void selectAfterEdit(const std::vector<PXR_NS::SdfPath>& paths);

    std::shared_ptr<UndoManager> m_undo;
    QSplitter* m_split = nullptr;
    ShaderNodePalette* m_palette = nullptr;
    QToolButton* m_paletteButton = nullptr;
    QStackedWidget* m_stack = nullptr;
    QtNodes::GraphicsView* m_view = nullptr;
    QLabel* m_empty = nullptr;
    QToolButton* m_pin = nullptr;
    QHBoxLayout* m_breadcrumb = nullptr;
    QLineEdit* m_inlineEdit = nullptr;
    PXR_NS::SdfPath m_level;
    std::unique_ptr<UsdShadeGraphModel> m_model;
    std::unique_ptr<QtNodes::BasicGraphicsScene> m_scene;
    std::vector<ShadeBackdropItem*> m_backdropItems;
    std::vector<unsigned int> m_nodesOnBackdrop;
    unsigned int m_pressedNode = 0;
    ShakeDetector m_shake;
    QElapsedTimer m_shakeClock;
    bool m_syncingSelection = false;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChanged;
    usdcc::core::Signal<const PrimMoves&>::Connection m_primsMoved;
};

}  // namespace usdcc::usd
