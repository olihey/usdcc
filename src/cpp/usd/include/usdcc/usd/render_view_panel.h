#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/render_camera.h"
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
#include <string>
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

// The camera of `viewport`, at the viewport's own aspect ratio.
USDCC_USD_UI_API RenderCamera viewportRenderCamera(const ViewportViewPanel& viewport);
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

    // Shows a rendered image (copied), as it is: already display colors.
    void showImage(const QImage& image);
    // Shows a rendered image in the working space's linear values: a float format
    // (Format_RGBA32FPx4, RGBA16FPx4, ...). The panel keeps it and shows it through
    // the view transform (docs/COLOR_MANAGEMENT.md §6), on a worker thread; image()
    // is the result, once it's there. A newer image replaces one still waiting.
    void showLinearImage(const QImage& image);
    const QImage& image() const;
    // The last linear image, or a null one when the renderer shows display colors.
    const QImage& linearImage() const { return m_linearImage; }

    // The OCIO view this panel shows with: "" for Settings > Color Management's
    // (docs/COLOR_MANAGEMENT.md §3.3). In its options menu, View Transform; kept in
    // sessions. colorTransformChanged() follows it, and the settings.
    const std::string& colorView() const { return m_colorView; }
    void setColorView(const std::string& view);
    // {"colorView": name} when it has one. A subclass with state of its own adds to it.
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;
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

Q_SIGNALS:
    // The view transform changed: the panel's view, or Settings > Color Management. A
    // renderer that presents on its own (RTX) applies it again.
    void colorTransformChanged();

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
    // Save Image: PNG, JPEG or TIFF of the image shown; ".exr" writes the linear image
    // (showLinearImage()) without the view transform, and fails without one.
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
    // The linear image through the view transform, on a worker; one at a time.
    void transformLinearImage();

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
    std::string m_colorView;
    QImage m_linearImage;          // RGBA32FPx4
    bool m_transforming = false;   // a worker has the linear image
    bool m_transformAgain = false; // and a newer one, or another view, came meanwhile

    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_viewportConnection;
};

}  // namespace usdcc::usd
