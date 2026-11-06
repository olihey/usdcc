#include "usdcc/usd/render_view_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/range2d.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace usdcc::usd {

namespace {
constexpr double kDegrees = 180.0 / std::numbers::pi;

// A field of view across one side from the one across the other, for an image
// `ratio` times as long on the side wanted: tan(half angle) scales with the side.
double scaledFov(double degrees, double ratio) {
    return 2.0 * std::atan(std::tan(degrees / kDegrees / 2.0) * ratio) * kDegrees;
}

// The Resolution choices: a scale of the panel's size, or a fixed size.
struct Resolution {
    const char* label;
    double scale;  // 0 for a fixed size
    QSize size;
};
const Resolution kResolutions[] = {
    {"Fit Panel", 1.0, {}},
    {"Fit Panel ½", 0.5, {}},
    {"Fit Panel ¼", 0.25, {}},
    {"1280 × 720", 0.0, {1280, 720}},
    {"1920 × 1080", 0.0, {1920, 1080}},
};

// The Camera menu's entry for the viewport's camera (a prim's is its path).
const QString kViewportCamera = QStringLiteral("viewport");

void appendPaths(std::vector<PXR_NS::SdfPath>& to, const std::vector<PXR_NS::SdfPath>& paths) {
    to.insert(to.end(), paths.begin(), paths.end());
}
}  // namespace

// --- Cameras.

RenderCamera RenderCamera::atAspect(double newAspect) const {
    RenderCamera camera = *this;
    if (newAspect <= 0.0) return camera;
    camera.aspect = newAspect;
    if (prim.IsEmpty()) {
        camera.fovX = scaledFov(fovY, newAspect);
    } else {
        camera.fovY = scaledFov(fovX, 1.0 / newAspect);
    }
    return camera;
}

RenderCamera viewportRenderCamera(const ViewportViewPanel& viewport) {
    const PXR_NS::GfFrustum frustum = viewport.hydraWindow()->frustum();
    RenderCamera camera;
    camera.cameraToWorld = frustum.ComputeViewInverse();
    const PXR_NS::GfRange2d window = frustum.GetWindow();
    camera.aspect = window.GetSize()[1] > 0.0 ? window.GetSize()[0] / window.GetSize()[1] : 1.0;
    camera.fovY = HydraViewportWindow::verticalFieldOfView();
    camera.fovX = scaledFov(camera.fovY, camera.aspect);
    camera.nearClip = frustum.GetNearFar().GetMin();
    camera.farClip = frustum.GetNearFar().GetMax();
    if (frustum.GetProjectionType() == PXR_NS::GfFrustum::Orthographic) {
        camera.orthographic = true;
        camera.orthoWidth = window.GetSize()[0];
    }
    return camera;
}

std::optional<RenderCamera> primRenderCamera(const PXR_NS::UsdPrim& prim, double aspect) {
    const PXR_NS::UsdGeomCamera usdCamera(prim);
    if (!usdCamera) return std::nullopt;
    const PXR_NS::GfCamera gfCamera = usdCamera.GetCamera(PXR_NS::UsdTimeCode::Default());
    RenderCamera camera;
    camera.prim = prim.GetPath();
    camera.cameraToWorld = gfCamera.GetTransform();
    camera.fovX = gfCamera.GetFieldOfView(PXR_NS::GfCamera::FOVHorizontal);
    camera.nearClip = gfCamera.GetClippingRange().GetMin();
    camera.farClip = gfCamera.GetClippingRange().GetMax();
    if (gfCamera.GetProjection() == PXR_NS::GfCamera::Orthographic) {
        camera.orthographic = true;
        camera.orthoWidth = gfCamera.GetHorizontalAperture() * PXR_NS::GfCamera::APERTURE_UNIT;
    }
    // As its aperture frames it, then at the image's aspect ratio.
    const double apertureAspect = gfCamera.GetAspectRatio();
    camera.aspect = apertureAspect > 0.0 ? apertureAspect : 1.0;
    camera.fovY = scaledFov(camera.fovX, 1.0 / camera.aspect);
    return camera.atAspect(aspect);
}

std::vector<PXR_NS::SdfPath> cameraPrims(const StageRefPtr& stage) {
    std::vector<PXR_NS::SdfPath> paths;
    if (!stage) return paths;
    for (const PXR_NS::UsdPrim& prim : stage->usdStage()->Traverse()) {
        if (prim.IsA<PXR_NS::UsdGeomCamera>()) paths.push_back(prim.GetPath());
    }
    return paths;
}

// --- The image.

RenderImageView::RenderImageView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(64, 64);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void RenderImageView::setImage(QImage image) {
    m_image = std::move(image);
    update();
}

void RenderImageView::setMessage(const QString& message) {
    m_message = message;
    update();
}

void RenderImageView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(24, 24, 26));
    if (!m_image.isNull()) {
        QRect target(QPoint(0, 0), m_image.size().scaled(size(), Qt::KeepAspectRatio));
        target.moveCenter(rect().center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, m_image);
    }
    if (!m_message.isEmpty()) {
        painter.setPen(QColor(205, 205, 210));
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, m_message);
    }
}

void RenderImageView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    Q_EMIT resized();
}

// --- The panel.

RenderViewPanel::RenderViewPanel(StageManager* stageManager, StageRefPtr stage, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* row = new QHBoxLayout(toolbar);
    addPanelControls(row);

    m_renderButton = new QToolButton(toolbar);
    m_renderButton->setObjectName(QStringLiteral("renderButton"));
    m_renderButton->setText(tr("Render"));
    m_renderButton->setCheckable(true);
    m_renderButton->setToolTip(tr("Start or stop rendering"));
    connect(m_renderButton, &QToolButton::toggled, this, &RenderViewPanel::onRenderToggled);

    m_refreshButton = new QToolButton(toolbar);
    m_refreshButton->setObjectName(QStringLiteral("renderRefresh"));
    m_refreshButton->setText(tr("Refresh"));
    m_refreshButton->setToolTip(tr("Send the stage to the renderer again"));
    connect(m_refreshButton, &QToolButton::clicked, this, &RenderViewPanel::onRefresh);

    m_cameraCombo = new QComboBox(toolbar);
    m_cameraCombo->setObjectName(QStringLiteral("renderCamera"));
    m_cameraCombo->setToolTip(tr("What the image is seen through"));
    m_cameraCombo->addItem(tr("Viewport Camera"), kViewportCamera);
    connect(m_cameraCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { sendCamera(); });

    m_resolutionCombo = new QComboBox(toolbar);
    m_resolutionCombo->setObjectName(QStringLiteral("renderResolution"));
    m_resolutionCombo->setToolTip(tr("The image's size"));
    for (const Resolution& resolution : kResolutions) {
        m_resolutionCombo->addItem(QString::fromUtf8(resolution.label));
    }
    connect(m_resolutionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { sendSize(); });

    m_extraToolbar = new QHBoxLayout();
    m_extraToolbar->setContentsMargins(0, 0, 0, 0);

    m_liveCheck = new QCheckBox(tr("Update on Edits"), toolbar);
    m_liveCheck->setObjectName(QStringLiteral("renderLive"));
    m_liveCheck->setChecked(true);
    m_liveCheck->setToolTip(tr("Render again after the stage is edited"));
    connect(m_liveCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) {
            m_editTimer->stop();
            m_pendingChanges = {};
        }
    });

    m_saveButton = new QToolButton(toolbar);
    m_saveButton->setObjectName(QStringLiteral("renderSave"));
    m_saveButton->setText(tr("Save Image…"));
    connect(m_saveButton, &QToolButton::clicked, this, &RenderViewPanel::saveImage);

    m_settingsButton = new QToolButton(toolbar);
    m_settingsButton->setObjectName(QStringLiteral("renderSettings"));
    m_settingsButton->setText(tr("Settings…"));
    m_settingsButton->setVisible(false);  // until setSettingsCategory()
    connect(m_settingsButton, &QToolButton::clicked, this, &RenderViewPanel::openSettings);

    row->addWidget(m_renderButton);
    row->addWidget(m_refreshButton);
    row->addWidget(m_cameraCombo);
    row->addWidget(m_resolutionCombo);
    row->addLayout(m_extraToolbar);
    row->addWidget(m_liveCheck);
    row->addStretch(1);
    row->addWidget(m_saveButton);
    row->addWidget(m_settingsButton);

    m_view = new RenderImageView(container);
    m_view->setObjectName(QStringLiteral("renderImage"));
    connect(m_view, &RenderImageView::resized, this, &RenderViewPanel::onResized);
    m_status = new QLabel(container);
    m_status->setObjectName(QStringLiteral("renderStatus"));
    m_status->setContentsMargins(8, 3, 8, 3);

    layout->addWidget(toolbar);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_status);
    setContentWidget(container);

    auto timer = [this](int interval, void (RenderViewPanel::*slot)()) {
        auto* made = new QTimer(this);
        made->setSingleShot(true);
        made->setInterval(interval);
        connect(made, &QTimer::timeout, this, slot);
        return made;
    };
    m_editTimer = timer(kDefaultEditDelayMs, &RenderViewPanel::onEditTimeout);
    m_cameraTimer = timer(kCameraIntervalMs, &RenderViewPanel::sendCamera);
    m_sizeTimer = timer(kResizeDelayMs, &RenderViewPanel::sendSize);

    // Closing the tab only hides a panel: a hidden one doesn't keep rendering.
    connect(this, &ads::CDockWidget::closed, this, [this]() {
        if (isRendering()) m_renderButton->setChecked(false);
    });
    m_viewportConnection = ViewportViewPanel::activeViewportChanged().connect([this]() { onViewportCameraChanged(); });
}

RenderViewPanel::~RenderViewPanel() { closePanel(); }

void RenderViewPanel::finishSetup() {
    m_setUp = true;
    refreshStageCombo();
    connectStage();
    fillCameras();
    if (const std::optional<QString> problem = setupProblem()) {
        m_view->setMessage(m_settingsCategory.isEmpty() ? *problem
                                                        : tr("%1\n\nSet it in Settings… > %2.").arg(*problem, m_settingsCategory));
    } else {
        m_view->setMessage(currentStage() ? idleMessage() : tr("No stage to render."));
    }
}

void RenderViewPanel::closePanel() {
    if (m_closed) return;
    m_closed = true;
    m_editTimer->stop();
    m_cameraTimer->stop();
    m_sizeTimer->stop();
    m_stageChangedConnection = {};
    m_viewportConnection = {};
    closing();
}

// --- For the renderer.

void RenderViewPanel::showImage(const QImage& image) {
    m_view->setMessage(QString());
    m_view->setImage(image.copy());
    showProgress();
}

const QImage& RenderViewPanel::image() const { return m_view->image(); }

void RenderViewPanel::setState(const QString& state) {
    m_state = state;
    showProgress();
}

void RenderViewPanel::showMessage(const QString& message) { m_view->setMessage(message); }

void RenderViewPanel::reportError(const QString& message, bool fatal) {
    setStatus(message);
    if (fatal) {
        setRendering(false);
        m_view->setMessage(tr("%1\n\nThe Log panel has more.").arg(message));
    }
}

void RenderViewPanel::setRendering(bool on) {
    const QSignalBlocker blocker(m_renderButton);
    m_renderButton->setChecked(on);
    m_renderButton->setText(on ? tr("Stop") : tr("Render"));
}

bool RenderViewPanel::isRendering() const { return m_renderButton->isChecked(); }

void RenderViewPanel::setEditDelay(int ms) { m_editTimer->setInterval(std::max(0, ms)); }

int RenderViewPanel::editDelay() const { return m_editTimer->interval(); }

bool RenderViewPanel::updatesOnEdits() const { return m_liveCheck->isChecked(); }

QSize RenderViewPanel::renderSize() const {
    const int index = std::clamp(m_resolutionCombo->currentIndex(), 0, int(std::size(kResolutions)) - 1);
    const Resolution& resolution = kResolutions[index];
    double width = resolution.size.width();
    double height = resolution.size.height();
    if (resolution.scale > 0.0) {
        const double ratio = m_view->devicePixelRatioF() * resolution.scale;
        width = m_view->width() * ratio;
        height = m_view->height() * ratio;
    }
    // A squashed panel, mid-layout, can be very thin: keep it a size renderers take.
    const double shrink = std::min(1.0, kMaxImageSize / std::max({width, height, 1.0}));
    return QSize(std::max(16, int(width * shrink)), std::max(16, int(height * shrink)));
}

RenderCamera RenderViewPanel::currentCamera() const {
    const QSize size = renderSize();
    const double aspect = double(size.width()) / std::max(1, size.height());
    const StageRefPtr& stage = currentStage();
    const QString source = m_cameraCombo->currentData().toString();
    if (source != kViewportCamera && stage) {
        const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath(source.toStdString()));
        if (std::optional<RenderCamera> camera = primRenderCamera(prim, aspect)) return *camera;
    }
    if (ViewportViewPanel* viewport = ViewportViewPanel::activeViewport(); viewport && stage && viewport->currentStage() == stage) {
        return viewportRenderCamera(*viewport).atAspect(aspect);
    }
    if (m_camera) return m_camera->atAspect(aspect);
    // No viewport on this stage: back from the origin, as a new viewport starts.
    RenderCamera camera;
    camera.cameraToWorld.SetTranslate(PXR_NS::GfVec3d(0.0, 0.0, 10.0));
    camera.fovY = HydraViewportWindow::verticalFieldOfView();
    return camera.atAspect(aspect);
}

void RenderViewPanel::setRendererName(const QString& name) {
    const bool showingIdle = m_view->message() == idleMessage();
    m_rendererName = name;
    if (showingIdle) m_view->setMessage(idleMessage());
}

void RenderViewPanel::setSettingsCategory(const QString& category) {
    m_settingsCategory = category;
    m_settingsButton->setVisible(!category.isEmpty());
    m_settingsButton->setToolTip(tr("The %1 settings").arg(category));
}

void RenderViewPanel::stageEdited(const StageChanges&) { reload(); }

// --- The view panel's own calls.

void RenderViewPanel::populateOptionsMenu(QMenu* menu) {
    if (!m_settingsCategory.isEmpty()) {
        menu->addAction(tr("%1 Settings…").arg(m_settingsCategory), this, &RenderViewPanel::openSettings);
    }
}

void RenderViewPanel::onStageChanged(const StageRefPtr& stage) {
    if (!m_setUp || m_closed) return;
    connectStage();
    fillCameras();
    m_pendingChanges = {};
    m_editTimer->stop();
    if (!stage) {
        m_renderButton->setChecked(false);  // stopRender()
        m_view->setMessage(tr("No stage to render."));
        return;
    }
    if (m_view->message() == tr("No stage to render.")) m_view->setMessage(idleMessage());
    onRefresh();
}

// --- What happens.

void RenderViewPanel::onRenderToggled(bool on) {
    m_renderButton->setText(on ? tr("Stop") : tr("Render"));
    if (!on) {
        stopRender();
        return;
    }
    std::optional<QString> problem = setupProblem();
    if (!problem && !currentStage()) problem = tr("No stage to render.");
    if (problem) {
        m_view->setMessage(*problem);
        setStatus(*problem);
        setRendering(false);
        return;
    }
    m_clock.start();
    startRender();
}

void RenderViewPanel::onRefresh() {
    if (!currentStage()) return;
    m_size = renderSize();
    m_camera = currentCamera();
    m_clock.start();
    reload();
}

void RenderViewPanel::connectStage() {
    m_stageChangedConnection = {};
    if (const StageRefPtr& stage = currentStage()) {
        m_stageChangedConnection =
            stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
    }
}

void RenderViewPanel::onStageObjectsChanged(const StageChanges& changes) {
    // Inside USD's change notice: only note it, the timers act afterwards.
    if (m_closed) return;
    if (!changes.resyncedPaths.empty()) {
        QTimer::singleShot(0, this, [this]() { fillCameras(); });
    }
    if (!m_liveCheck->isChecked()) return;
    appendPaths(m_pendingChanges.resyncedPaths, changes.resyncedPaths);
    appendPaths(m_pendingChanges.changedInfoOnlyPaths, changes.changedInfoOnlyPaths);
    m_editTimer->start();
}

void RenderViewPanel::onEditTimeout() {
    StageChanges changes = std::exchange(m_pendingChanges, {});
    if (!currentStage() || m_closed) return;
    m_size = renderSize();
    m_camera = currentCamera();
    m_clock.start();
    stageEdited(changes);
}

void RenderViewPanel::onViewportCameraChanged() {
    if (m_closed || m_cameraCombo->currentData().toString() != kViewportCamera) return;
    ViewportViewPanel* viewport = ViewportViewPanel::activeViewport();
    if (!viewport || !currentStage() || viewport->currentStage() != currentStage()) return;
    if (!m_cameraTimer->isActive()) m_cameraTimer->start();
}

void RenderViewPanel::onResized() {
    const int index = m_resolutionCombo->currentIndex();
    if (index >= 0 && index < int(std::size(kResolutions)) && kResolutions[index].scale > 0.0) m_sizeTimer->start();
}

void RenderViewPanel::sendCamera() {
    if (m_closed || !m_setUp) return;
    m_camera = currentCamera();
    m_clock.start();
    cameraChanged(*m_camera);
}

void RenderViewPanel::sendSize() {
    if (m_closed || !m_setUp) return;
    const QSize size = renderSize();
    if (size == m_size) return;
    m_size = size;
    sizeChanged(size);
    sendCamera();  // the field of view follows the aspect ratio
}

void RenderViewPanel::fillCameras() {
    if (m_closed) return;
    const QString current = m_cameraCombo->currentData().toString();
    int index = -1;
    {
        const QSignalBlocker blocker(m_cameraCombo);
        m_cameraCombo->clear();
        m_cameraCombo->addItem(tr("Viewport Camera"), kViewportCamera);
        for (const PXR_NS::SdfPath& path : cameraPrims(currentStage())) {
            m_cameraCombo->addItem(QString::fromStdString(path.GetName()), QString::fromStdString(path.GetString()));
        }
        index = m_cameraCombo->findData(current);
        m_cameraCombo->setCurrentIndex(std::max(0, index));
    }
    if (index < 0 && current != kViewportCamera && !current.isEmpty()) sendCamera();  // its camera went away
}

void RenderViewPanel::saveImage() {
    if (m_view->image().isNull()) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Image"), QStringLiteral("render.png"),
                                                      tr("PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif)"));
    if (!path.isEmpty() && !m_view->image().save(path)) {
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Error, "usdcc.render", "Could not save {}.", path.toStdString());
    }
}

void RenderViewPanel::openSettings() {
    if (m_settingsCategory.isEmpty()) return;
    usdcc::ui::SettingsDialog::showDialog(window())->showCategory(m_settingsCategory);
}

void RenderViewPanel::showProgress() {
    QStringList parts{m_state.isEmpty() ? tr("Rendering") : m_state};
    if (m_clock.isValid()) parts << tr("%1 s").arg(m_clock.elapsed() / 1000.0, 0, 'f', 1);
    if (!m_view->image().isNull()) parts << tr("%1 × %2").arg(m_view->image().width()).arg(m_view->image().height());
    setStatus(parts.join(QStringLiteral(" · ")));
}

void RenderViewPanel::setStatus(const QString& text) { m_status->setText(text); }

QString RenderViewPanel::idleMessage() const {
    return m_rendererName.isEmpty() ? tr("Press Render to render the stage.")
                                    : tr("Press Render to render the stage with %1.").arg(m_rendererName);
}

}  // namespace usdcc::usd
