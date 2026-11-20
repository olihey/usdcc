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

class QComboBox;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}  // namespace QtNodes

namespace usdcc::usd {

class PropertySheet;
class ShadeBackdropItem;
class ShaderNodePalette;
class ShadingPreviewWidget;
class ShaderNodeSearchPopup;
class StageManager;
class UndoManager;
class UsdShadeGraphModel;

// A material's shading network as a node graph (docs/MATERIAL_EDITOR.md §4). It follows the
// selection both ways, and a pin keeps it on what it's showing. A NodeGraph is one node until
// double-clicked, which enters it; a breadcrumb walks back up. Every edit is one undo step.
//
// On its right (§7), each shown or hidden: a preview of the material on a shape, and the
// shading parameters of the selected node, or of the level's interface.
class USDCC_USD_UI_API MaterialEditorViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    MaterialEditorViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo, const QString& title,
                            QWidget* parent = nullptr);
    ~MaterialEditorViewPanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    // Which of the pane's parts show, and the preview's shape, light and pause.
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

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
    void setPreviewShown(bool shown);
    bool isPreviewShown() const;
    void setParametersShown(bool shown);
    bool areParametersShown() const;
    // What a right-click there offers, on what's under it.
    void populateGraphMenu(QMenu* menu, QPointF scenePosition);

    // Find (docs/MATERIAL_EDITOR.md §4.1): a bar under the toolbar, Ctrl+F, over the whole
    // material. It frames what it finds, entering the level that's on, and leaves the
    // selection alone. What it didn't find on the shown level is dimmed.
    void showFind();
    void closeFind();
    bool isFindShown() const;
    // The next match, or the previous one, round the end. F3 and Shift+F3.
    void findNext(bool backward = false);
    QLineEdit* findEdit() const { return m_findEdit; }
    // Any, Name, Type, Shading System or Context: ShadeFindField, as the item's data.
    QComboBox* findField() const { return m_findField; }
    QLabel* findCount() const { return m_findCount; }
    const std::vector<ShadeFindMatch>& findMatches() const { return m_findMatches; }
    // Into findMatches(); -1 when it isn't on one.
    int currentFindMatch() const { return m_findIndex; }

    QtNodes::GraphicsView* view() const { return m_view; }
    QtNodes::BasicGraphicsScene* scene() const { return m_scene.get(); }
    UsdShadeGraphModel* model() const { return m_model.get(); }
    ShaderNodePalette* palette() const { return m_palette; }
    PropertySheet* parameters() const { return m_params; }
    ShadingPreviewWidget* preview() const { return m_preview; }
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
    bool clickWithSelectionKeys(QMouseEvent* mouse);
    std::vector<PXR_NS::SdfPath> selectedPrims() const;
    void rebuildBackdropItems();
    void startInlineEdit(const QString& text, const QRectF& sceneRect, const std::function<void(const QString&)>& commit);
    QPointF mouseScenePosition() const;
    void selectAfterEdit(const std::vector<PXR_NS::SdfPath>& paths);
    // What the pane on the right shows: the selected node, or the level, and its material.
    void updateSidePane();
    void updateSideVisibility();
    // Keeps the preview's picture square, as wide as the pane on the right, with the
    // Parameters below in what's left (docs/MATERIAL_EDITOR.md §7.5).
    void fitPreview();
    // Finds again. `frame`: and goes to the first match, on this level if one is.
    void updateFind(bool frame);
    void showFindMatch();
    void updateFindMarks();
    void updateFindCount();

    std::shared_ptr<UndoManager> m_undo;
    QWidget* m_findBar = nullptr;
    QLineEdit* m_findEdit = nullptr;
    QComboBox* m_findField = nullptr;
    QLabel* m_findCount = nullptr;
    QTimer* m_findRefresh = nullptr;
    std::vector<ShadeFindMatch> m_findMatches;
    int m_findIndex = -1;
    PXR_NS::SdfPath m_findRoot;
    QSplitter* m_split = nullptr;
    ShaderNodePalette* m_palette = nullptr;
    QToolButton* m_paletteButton = nullptr;
    QToolButton* m_previewButton = nullptr;
    QToolButton* m_paramsButton = nullptr;
    QSplitter* m_side = nullptr;
    ShadingPreviewWidget* m_preview = nullptr;
    PropertySheet* m_params = nullptr;
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
    // A Ctrl click on a node, whose move and release go no further than its press.
    bool m_swallowingClick = false;
    // A Shift click on a node, whose release mustn't leave it the only one selected.
    bool m_keepSelectionOnRelease = false;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChanged;
    usdcc::core::Signal<const PrimMoves&>::Connection m_primsMoved;
    usdcc::core::Signal<const StageChanges&>::Connection m_findStageChanged;
};

}  // namespace usdcc::usd
