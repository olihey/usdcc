#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QStringList>

#include <memory>
#include <vector>

class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QToolButton;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}  // namespace QtNodes

namespace usdcc::usd {

class StageManager;
class UndoManager;
class UsdShadeGraphModel;

// A material's shading network as a node graph (docs/MATERIAL_EDITOR.md §4). It follows
// the selection both ways, and a pin keeps it on what it's showing. A NodeGraph is one node
// until double-clicked, which enters it; a breadcrumb walks back up. So far, only a node's
// position can be edited.
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

    QtNodes::GraphicsView* view() const { return m_view; }
    QtNodes::BasicGraphicsScene* scene() const { return m_scene.get(); }
    UsdShadeGraphModel* model() const { return m_model.get(); }
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

    std::shared_ptr<UndoManager> m_undo;
    QStackedWidget* m_stack = nullptr;
    QtNodes::GraphicsView* m_view = nullptr;
    QLabel* m_empty = nullptr;
    QToolButton* m_pin = nullptr;
    QHBoxLayout* m_breadcrumb = nullptr;
    PXR_NS::SdfPath m_level;
    std::unique_ptr<UsdShadeGraphModel> m_model;
    std::unique_ptr<QtNodes::BasicGraphicsScene> m_scene;
    bool m_syncingSelection = false;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChanged;
    usdcc::core::Signal<const PrimMoves&>::Connection m_primsMoved;
};

}  // namespace usdcc::usd
