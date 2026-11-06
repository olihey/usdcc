#pragma once

#include "usdcc/ui/view_panel.h"
#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <memory>

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}  // namespace QtNodes

namespace usdcc::usd {

class StageManager;
class UndoManager;
class UsdShadeSpikeModel;

// A throwaway, from Debug > Material Graph Spike, to find out if QtNodes will do for
// the Material Editor (docs/MATERIAL_EDITOR.md §5, phase 0). It draws one Material's
// shaders and connections, and the only edit is dragging a node, which writes
// ui:nodegraph:node:pos as one undo step. Phase 2's panel replaces it.
class USDCC_USD_UI_API MaterialGraphSpikePanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    MaterialGraphSpikePanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo, const QString& title,
                            QWidget* parent = nullptr);
    ~MaterialGraphSpikePanel() override;

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

    // Which Material it shows: the first selected prim inside one, else the stage's first.
    void showMaterial(const PXR_NS::SdfPath& path);
    const PXR_NS::SdfPath& shownMaterial() const { return m_material; }

    QtNodes::GraphicsView* view() const { return m_view; }
    QtNodes::BasicGraphicsScene* scene() const { return m_scene.get(); }
    UsdShadeSpikeModel* model() const { return m_model.get(); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void pickMaterial();
    void fitGraph();

    std::shared_ptr<UndoManager> m_undo;
    QtNodes::GraphicsView* m_view = nullptr;
    PXR_NS::SdfPath m_material;
    std::unique_ptr<UsdShadeSpikeModel> m_model;
    std::unique_ptr<QtNodes::BasicGraphicsScene> m_scene;
};

}  // namespace usdcc::usd
