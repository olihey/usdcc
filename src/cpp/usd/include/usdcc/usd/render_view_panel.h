#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <QElapsedTimer>
#include <QImage>
#include <QSize>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QTimer;
class QToolButton;

namespace usdcc::usd {

class StageManager;
class ViewportViewPanel;

// A camera as a renderer is given it (docs/RENDER_VIEW_PANEL.md §3): where it is,
// and how much it sees at the image's aspect ratio.
struct USDCC_USD_UI_API RenderCamera {
    PXR_NS::GfMatrix4d cameraToWorld{1.0};
    // Fields of view, in degrees, at `aspect`.
    double fovX = 45.0;
    double fovY = 45.0;
    // Width / height of the image.
    double aspect = 1.0;
    double nearClip = 0.1;
    double farClip = 10000.0;
    bool orthographic = false;
    // What an orthographic camera sees across, in scene units.
    double orthoWidth = 0.0;
    // The camera prim; empty for a viewport's camera.
    PXR_NS::SdfPath prim;

    // The same camera at another aspect ratio: a viewport's or the default camera
    // keeps its vertical field of view (a wider image shows more at the sides), a
    // camera prim its horizontal one, as its aperture frames it.
    RenderCamera atAspect(double newAspect) const;
};

// The camera of `viewport`, at the viewport's own aspect ratio.
USDCC_USD_UI_API RenderCamera viewportRenderCamera(const ViewportViewPanel& viewport);
// The camera of a UsdGeomCamera prim at `aspect`, at the default time; nothing for
// a prim that isn't one.
USDCC_USD_UI_API std::optional<RenderCamera> primRenderCamera(const PXR_NS::UsdPrim& prim, double aspect);
// The paths of `stage`'s UsdGeomCamera prims, in the order they appear.
USDCC_USD_UI_API std::vector<PXR_NS::SdfPath> cameraPrims(const StageRefPtr& stage);

// The image area: the rendered image scaled to fit, or a message.
class USDCC_USD_UI_API RenderImageView : public QWidget {
    Q_OBJECT

public:
    explicit RenderImageView(QWidget* parent = nullptr);

    const QImage& image() const { return m_image; }
    void setImage(QImage image);
    const QString& message() const { return m_message; }
    void setMessage(const QString& message);
    // Optional native surface, owned by this view. Messages cover it until a
    // renderer explicitly publishes a frame; ordinary QImage users are unchanged.
    void setPresentationWidget(QWidget* widget);

Q_SIGNALS:
    void resized();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QImage m_image;
    QString m_message;
    QWidget* m_presentation = nullptr;
};

// A view panel that shows its stage rendered by a renderer that isn't Hydra's,
// such as native RTX or V-Ray in a helper process (docs/RENDER_VIEW_PANEL.md). This
// class is the renderer-independent part: the toolbar's icons (Render/Stop, Refresh,
// Save Image, Settings), the options menu (Camera, Resolution, Update on Edits, then
// the renderer's own options), the image, the status line, and following the camera,
// the panel's size and the stage's edits. A subclass
// does the rendering through the hooks below and hands back images with
// showImage(). ScriptedRenderViewPanel is the subclass for renderers written in
// Python (usdcc.ui.RenderViewPanel).
//
// A subclass ends its constructor with finishSetup(), which calls hooks (they
// can't be called from this constructor).
class USDCC_USD_UI_API RenderViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // Camera moves come as fast as the mouse; the renderer hears of them at most
    // this often (ms).
    static constexpr int kCameraIntervalMs = 30;
    // After a resize stops (ms).
    static constexpr int kResizeDelayMs = 150;
    static constexpr int kDefaultEditDelayMs = 500;
    // The longest side of an image, in pixels.
    static constexpr int kMaxImageSize = 8192;

    RenderViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& title, QWidget* parent = nullptr);
    ~RenderViewPanel() override;

    // --- For the renderer.

    // Shows a rendered image (copied).
    void showImage(const QImage& image);
    const QImage& image() const;
    void setPresentationWidget(QWidget* widget);
    void showPresentedFrame(const QSize& size);
    // What the renderer is doing, for the status line: "Rendering", "Done", ...
    void setState(const QString& state);
    // A message in the image area instead of the image; empty shows the image.
    void showMessage(const QString& message);
    // An error: in the status line, and in the image area. A fatal one also stops
    // rendering (Render is unchecked without calling stopRender()). Logging and
    // toasts are the renderer's: it knows its log's name.
    void reportError(const QString& message, bool fatal);
    // Checks or unchecks Render without calling startRender()/stopRender(), for
    // when the renderer stopped by itself.
    void setRendering(bool on);
    bool isRendering() const;
    // How long after the last edit stageEdited() is called (ms).
    void setEditDelay(int ms);
    int editDelay() const;
    // What the image is seen through now, at renderSize()'s aspect ratio.
    RenderCamera currentCamera() const;
    // Renderer-local viewing camera. Adds/selects Free Camera without authoring
    // a scene camera; normal viewport/prim choices remain available.
    void setLocalCamera(const RenderCamera& camera);
    bool usesLocalCamera() const;
    // The image's size in pixels, from the Resolution choice and the panel's size.
    QSize renderSize() const;
    // Where a renderer adds its own toolbar buttons, after Render and Refresh. Keep
    // the toolbar to icons (usdcc::ui::PanelIconButton); choices belong in the options
    // menu (populateRendererOptions()).
    QHBoxLayout* extraToolbarLayout() const { return m_extraToolbar; }
    // The renderer's name, in "Press Render to render the stage with %1."
    void setRendererName(const QString& name);
    // The Settings dialog's category Settings… opens; empty hides the button.
    void setSettingsCategory(const QString& category);

    // Whether this renders again after edits (the Update on Edits check box).
    bool updatesOnEdits() const;

protected:
    // --- Hooks, for the subclass; all do nothing here.

    // Why it can't render (no SDK, say), or nothing. Asked when the panel is made
    // and when Render is pressed; while there is one, Render does nothing.
    virtual std::optional<QString> setupProblem() { return std::nullopt; }
    // Render was checked or unchecked.
    virtual void startRender() {}
    virtual void stopRender() {}
    // Send the whole stage again: Refresh, another stage, or (by default) an edit.
    virtual void reload() {}
    // The camera moved, or another one was picked.
    virtual void cameraChanged(const RenderCamera& camera) { (void)camera; }
    // The image's size changed (the camera follows, as its aspect ratio did).
    virtual void sizeChanged(const QSize& size) { (void)size; }
    // The stage was edited, editDelay() after the last edit, while Update on
    // Edits is on: every change since the last call. Calls reload() here; a
    // renderer that can update in place does that instead.
    virtual void stageEdited(const StageChanges& changes);
    // The panel is going away: end what it started. Called once, from the
    // subclass's destructor (closePanel()) or this one's.
    virtual void closing() {}
    // The renderer's own entries in the options menu (a render mode, Fit to View),
    // after the shared Camera, Resolution and Update on Edits, before Settings….
    virtual void populateRendererOptions(QMenu* menu) { (void)menu; }
    virtual void saveRenderedImage(const QString& path);

    // Ends a subclass's constructor: the setup message, the camera list, the stage.
    void finishSetup();
    // Calls closing() once; a subclass calls it first in its destructor, while it
    // is still whole.
    void closePanel();

    void populateOptionsMenu(QMenu* menu) override;
    void onStageChanged(const StageRefPtr& stage) override;

private:
    void onRenderToggled(bool on);
    // Render's icon, text and tooltip: play, or stop while rendering.
    void showRenderButton(bool rendering);
    void onRefresh();
    void onStageObjectsChanged(const StageChanges& changes);
    void onViewportCameraChanged();
    void onEditTimeout();
    void onResized();
    void sendCamera();
    void sendSize();
    void fillCameras();
    void saveImage();
    void openSettings();
    void showProgress();
    void setStatus(const QString& text);
    void connectStage();
    QString idleMessage() const;

    QToolButton* m_renderButton;
    QToolButton* m_refreshButton;
    QComboBox* m_cameraCombo;
    QComboBox* m_resolutionCombo;
    QHBoxLayout* m_extraToolbar;
    QCheckBox* m_liveCheck;
    QToolButton* m_saveButton;
    QToolButton* m_settingsButton;
    RenderImageView* m_view;
    QLabel* m_status;
    QTimer* m_editTimer;
    QTimer* m_cameraTimer;
    QTimer* m_sizeTimer;

    QString m_rendererName;
    QString m_settingsCategory;
    QString m_state;
    QElapsedTimer m_clock;  // since the last reload or camera change
    QSize m_size;           // the last size sent
    // The last camera sent, kept for when no viewport shows the stage.
    std::optional<RenderCamera> m_camera;
    std::optional<RenderCamera> m_localCamera;
    StageChanges m_pendingChanges;
    bool m_closed = false;
    bool m_setUp = false;
    QSize m_presentedSize;

    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_viewportConnection;
};

}  // namespace usdcc::usd
