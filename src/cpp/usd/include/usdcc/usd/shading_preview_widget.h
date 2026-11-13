#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <map>
#include <memory>
#include <string>

class QLabel;
class QMenu;
class QStackedWidget;
class QToolButton;

namespace usdcc::usd {

class HydraViewportWindow;
class ShadingPreviewStage;

// The settings of the preview (docs/MATERIAL_EDITOR.md §7.3, §7.4).
inline constexpr const char* kPreviewShapeFoldersKey = "Materials/previewShapePaths";
inline constexpr const char* kPreviewHdriFoldersKey = "Materials/previewHdriPaths";
inline constexpr const char* kPreviewShapeKey = "Materials/previewShape";
inline constexpr const char* kPreviewHdriKey = "Materials/previewHdri";
USDCC_USD_UI_API void defineMaterialPreviewSettings();
// What usdcc ships: resources/preview, next to bin/ when deployed, else in the source tree.
USDCC_USD_UI_API QString shippedPreviewFolder();
// Where the menus look, the setting's folders first, then what usdcc ships.
USDCC_USD_UI_API QStringList previewShapeFolders();
USDCC_USD_UI_API QStringList previewHdriFolders();

// The Material Editor's preview (docs/MATERIAL_EDITOR.md §7.2–§7.6): a material, or one node's
// output, on a shape under an HDRI, rendered by the renderer of the viewport showing the same
// stage. Above it: pause/play, and a menu of what's shown, the shape, the HDRI and Reset View.
class USDCC_USD_UI_API ShadingPreviewWidget : public QWidget {
    Q_OBJECT

public:
    enum class Subject { Material, SelectedNode };

    explicit ShadingPreviewWidget(QWidget* parent = nullptr);
    ~ShadingPreviewWidget() override;

    void setStage(const StageRefPtr& stage);
    // The Material shown; empty for none.
    void setMaterial(const PXR_NS::SdfPath& material);
    const PXR_NS::SdfPath& material() const { return m_material; }
    // The node a SelectedNode preview shows: a Shader in the material, else none.
    void setSelectedNode(const PXR_NS::SdfPath& node);

    void setSubject(Subject subject);
    Subject subject() const { return m_subject; }
    void setPaused(bool paused);
    bool isPaused() const;

    // By name, as the menus list them.
    void setShapeName(const QString& name);
    QString shapeName() const { return m_shapeName; }
    void setHdriName(const QString& name);  // empty for none: a grey dome and a key light
    QString hdriName() const { return m_hdriName; }
    void setHdriRotation(double degrees);
    double hdriRotation() const { return m_rotation; }
    void setHdriExposure(double stops);
    double hdriExposure() const { return m_exposure; }
    void setHdriBackground(bool shown);
    bool hdriBackground() const { return m_background; }
    // Light the shape with its own lights only, switching off the dome and key light. Turned
    // on whenever a shape file with lights is chosen; does nothing for one without.
    void setUseSceneLights(bool use);
    bool useSceneLights() const { return m_sceneLights; }
    // Whether the shape file has lights, so the Light menu offers Use scene lights.
    bool shapeHasLights() const { return m_hasLights; }

    // The above, for a session or a duplicate.
    QJsonObject saveState() const;
    void restoreState(const QJsonObject& state);

    // What the line under the preview says: why it can't show something, or empty.
    QString problem() const;
    // Why nothing is shown at all, or empty when something is.
    QString emptyText() const;

    HydraViewportWindow* viewport() const { return m_viewport; }
    ShadingPreviewStage* previewStage() const { return m_preview.get(); }
    // Looks through the shape's camera when it has one, else frames the shape.
    void frame();
    // Whether the shape file has a camera to look through, which Reset View goes back to.
    bool hasShapeCamera() const { return m_hasCamera; }
    void resetView();

    // What the ... button's menu holds, built afresh each time it opens.
    void populateOptionsMenu(QMenu* menu);

    // How tall this must be, at `width`, for the picture to be square: the toolbar and the
    // warning line, when one shows, take their share on top.
    int heightForSquare(int width) const;

Q_SIGNALS:
    // Something saveState() holds changed.
    void stateChanged();
    // heightForSquare() may have changed: the warning line came, went or says something else.
    void squareHeightChanged();

protected:
    // The picture is never taller than wide: what's left goes below it.
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildPreview();
    void applyMaterial();
    void applySubject();
    void applyShape();
    void applyCamera();
    void applyEnvironment();
    void followRenderer();
    void checkRenderer();
    void updateShown();
    void updatePauseButton();
    void populateShapeMenu(QMenu* menu);
    void populateLightMenu(QMenu* menu);
    void setProblem(const std::string& key, const QString& text);

    StageRefPtr m_stage;
    std::unique_ptr<ShadingPreviewStage> m_preview;
    PXR_NS::SdfPath m_material;
    PXR_NS::SdfPath m_node;
    Subject m_subject = Subject::Material;
    QString m_shapeName;
    QString m_hdriName;
    double m_rotation = 0;
    double m_exposure = 0;
    bool m_background = true;
    bool m_framed = false;
    bool m_hasCamera = false;
    bool m_hasLights = false;
    bool m_sceneLights = false;
    std::string m_lightsShape;  // the shape file m_sceneLights was last defaulted for

    // Why something can't be shown, by what it's about ("shape", "renderer", "node").
    std::map<std::string, QString> m_problems;
    std::map<std::string, std::vector<std::string>> m_rendererContexts;  // by renderer, cached

    HydraViewportWindow* m_viewport = nullptr;
    QWidget* m_toolbar = nullptr;
    QWidget* m_viewportContainer = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_empty = nullptr;
    QLabel* m_problem = nullptr;
    QToolButton* m_pause = nullptr;
    QToolButton* m_more = nullptr;

    usdcc::core::Signal<const StageChanges&>::Connection m_stageChanged;
    usdcc::core::Signal<>::Connection m_viewportsChanged;
};

}  // namespace usdcc::usd
