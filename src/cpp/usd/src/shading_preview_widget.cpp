#include "usdcc/usd/shading_preview_widget.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/orbit_camera.h"
#include "usdcc/usd/shader_node_catalog.h"
#include "usdcc/usd/shading_preview.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QOpenGLContext>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <functional>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {
const TfToken kStorm("HdStormRendererPlugin");

// Whether this platform can make an OpenGL context at all. Without one (Qt's offscreen
// platform, which the tests use), the preview isn't embedded: a native child window there
// gains nothing, and Qt's offscreen backing store crashes on one moved into a floating dock.
bool canRenderHere() {
    static const bool can = []() {
        QOpenGLContext context;
        return context.create();
    }();
    return can;
}

QString qs(const std::string& text) { return QString::fromStdString(text); }

QStringList foldersOf(const char* key, const QString& shipped) {
    QStringList folders;
    auto& settings = usdcc::core::Settings::instance();
    if (settings.contains(key)) {
        for (const QString& folder : qs(settings.get<std::string>(key)).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            if (!folder.trimmed().isEmpty()) folders << folder.trimmed();
        }
    }
    if (!shipped.isEmpty()) folders << shipped;
    return folders;
}

std::vector<std::string> utf8(const QStringList& list) {
    std::vector<std::string> out;
    for (const QString& text : list) out.push_back(text.toStdString());
    return out;
}

QString settingText(const char* key, const char* fallback) {
    auto& settings = usdcc::core::Settings::instance();
    return settings.contains(key) ? qs(settings.get<std::string>(key)) : QString::fromLatin1(fallback);
}

// The render context a surface terminal is for: "" for outputs:surface, "mtlx" for outputs:mtlx:surface.
std::string contextOf(const std::string& terminal) {
    const std::string prefix = "outputs:";
    std::string name = terminal.substr(std::min(terminal.size(), prefix.size()));
    const auto colon = name.rfind(':');
    return colon == std::string::npos ? std::string() : name.substr(0, colon);
}
}  // namespace

void defineMaterialPreviewSettings() {
    using usdcc::core::SettingType;
    auto& settings = usdcc::core::Settings::instance();
    const QString shipped = shippedPreviewFolder();
    settings.define({
        .key = kPreviewShapeFoldersKey,
        .category = "Materials",
        .group = "Preview",
        .label = "Shape folders",
        .description = "Folders of USD files to preview materials on, one shape per file: its defaultPrim, with UVs. "
                       "Prims listed in a collection:preview on it get the material; without one, every gprim does.",
        .type = SettingType::FolderList,
        .defaultValue = std::string(),
        .fixedFolders = {{.label = "Shipped with usdcc", .folder = (shipped + QStringLiteral("/shapes")).toStdString(), .after = true}},
    });
    settings.define({
        .key = kPreviewHdriFoldersKey,
        .category = "Materials",
        .group = "Preview",
        .label = "HDRI folders",
        .description = "Folders of .hdr and .exr maps to light the preview with.",
        .type = SettingType::FolderList,
        .defaultValue = std::string(),
        .fixedFolders = {{.label = "Shipped with usdcc", .folder = (shipped + QStringLiteral("/hdri")).toStdString(), .after = true}},
    });
    settings.define({
        .key = kPreviewShapeKey,
        .category = "Materials",
        .group = "Preview",
        .label = "Shape for new previews",
        .description = "The name of the shape a new Material Editor's preview starts with, as its menu lists it.",
        .type = SettingType::String,
        .defaultValue = std::string("Shader Ball"),
    });
    settings.define({
        .key = kPreviewHdriKey,
        .category = "Materials",
        .group = "Preview",
        .label = "HDRI for new previews",
        .description = "The name of the HDRI a new preview starts with; empty for a plain grey dome.",
        .type = SettingType::String,
        .defaultValue = std::string("Studio"),
    });
}

QString shippedPreviewFolder() {
    // Deployed, bin/usdcc.exe has resources/ next to it.
    const QString deployed = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../resources/preview"));
    if (QFileInfo(deployed).isDir()) return QDir::cleanPath(deployed);
#ifdef USDCC_PREVIEW_ASSETS_DIR
    const QString source = QStringLiteral(USDCC_PREVIEW_ASSETS_DIR);
    if (QFileInfo(source).isDir()) return QDir::cleanPath(source);
#endif
    return {};
}

QStringList previewShapeFolders() {
    const QString shipped = shippedPreviewFolder();
    return foldersOf(kPreviewShapeFoldersKey, shipped.isEmpty() ? QString() : shipped + QStringLiteral("/shapes"));
}

QStringList previewHdriFolders() {
    const QString shipped = shippedPreviewFolder();
    return foldersOf(kPreviewHdriFoldersKey, shipped.isEmpty() ? QString() : shipped + QStringLiteral("/hdri"));
}

ShadingPreviewWidget::ShadingPreviewWidget(QWidget* parent) : QWidget(parent) {
    m_shapeName = settingText(kPreviewShapeKey, "Shader Ball");
    m_hdriName = settingText(kPreviewHdriKey, "Studio");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    m_toolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("panelToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* bar = new QHBoxLayout(toolbar);
    usdcc::ui::scaled::margins(bar, 4, 2, 4, 2);
    usdcc::ui::scaled::spacing(bar, 4);

    // Pause shows the bars while it renders and play while it's paused: what a click does.
    m_pause = new usdcc::ui::PanelIconButton(QStringLiteral(":/usdcc/theme/pause.png"), QString(), toolbar);
    m_pause->setObjectName(QStringLiteral("previewPause"));
    m_pause->setCheckable(true);
    bar->addWidget(m_pause);
    bar->addStretch(1);

    // Everything else: what's shown, the shape, the light and Reset View.
    auto* more = new usdcc::ui::PanelMenuButton(tr("Preview options: what's shown, the shape, the light"),
                                                [this](QMenu* menu) { populateOptionsMenu(menu); }, toolbar);
    more->setObjectName(QStringLiteral("previewMore"));
    more->setIcon(QIcon(QStringLiteral(":/usdcc/theme/more.png")));
    m_more = more;
    bar->addWidget(m_more);
    updatePauseButton();

    m_viewport = new HydraViewportWindow();
    m_viewport->setPreviewMode(true);
    HydraViewportWindow::DisplayOptions display;
    display.showGrid = false;
    m_viewport->setDisplayOptions(display);
    m_viewport->setDomeLightVisible(m_background);
    m_viewport->setRendererPlugin(kStorm);

    m_stack = new QStackedWidget(this);
    if (canRenderHere()) {
        m_viewportContainer = QWidget::createWindowContainer(m_viewport, m_stack);
    } else {
        static_cast<QObject*>(m_viewport)->setParent(this);  // owned, never shown
        m_viewportContainer = new QLabel(tr("No OpenGL here, so the preview can't render."), m_stack);
        static_cast<QLabel*>(m_viewportContainer)->setAlignment(Qt::AlignCenter);
    }
    m_viewportContainer->setMinimumSize(usdcc::ui::scaled::pixels(120), usdcc::ui::scaled::pixels(120));
    m_empty = new QLabel(m_stack);
    m_empty->setObjectName(QStringLiteral("propertyNote"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_stack->addWidget(m_viewportContainer);
    m_stack->addWidget(m_empty);

    m_problem = new QLabel(this);
    m_problem->setObjectName(QStringLiteral("previewProblem"));
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: #ffbd2e;"));
    usdcc::ui::scaled::margins(m_problem, 6, 3, 6, 3);
    m_problem->hide();

    layout->addWidget(toolbar);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_problem);
    // Given more height than its width, the picture stays square and the rest is left below.
    layout->addStretch(0);

    connect(m_pause, &QToolButton::toggled, this, [this](bool paused) {
        m_viewport->setPaused(paused);
        updatePauseButton();
        Q_EMIT stateChanged();
    });
    connect(m_viewport, &HydraViewportWindow::firstFrameRendered, this, [this]() {
        if (!m_framed) frame();
    });
    connect(m_viewport, &HydraViewportWindow::rendererChanged, this, [this]() { checkRenderer(); });
    connect(m_viewport, &HydraViewportWindow::contextMenuRequested, this, [this](const QPoint& globalPos) {
        // Parented to the top-level window, as PanelMenuButton's: the viewport's native
        // container can't be a popup's transient parent.
        QMenu menu(window());
        populateOptionsMenu(&menu);
        menu.exec(globalPos);
    });
    m_viewportsChanged = ViewportViewPanel::viewportsChanged().connect([this]() { followRenderer(); });

    updateShown();
}

void ShadingPreviewWidget::updatePauseButton() {
    const bool paused = m_pause->isChecked();
    m_pause->setIcon(QIcon(paused ? QStringLiteral(":/usdcc/theme/play.png") : QStringLiteral(":/usdcc/theme/pause.png")));
    m_pause->setText(paused ? tr("Play") : tr("Pause"));  // not shown; for accessibility and tests
    m_pause->setToolTip(paused ? tr("Render the preview again") : tr("Stop rendering the preview; the last frame stays"));
}

ShadingPreviewWidget::~ShadingPreviewWidget() = default;

void ShadingPreviewWidget::setStage(const StageRefPtr& stage) {
    if (stage == m_stage && m_preview) return;
    m_stage = stage;
    m_stageChanged = {};
    m_material = SdfPath();
    m_node = SdfPath();
    rebuildPreview();
    if (stage) {
        m_stageChanged = stage->changed.connect([this](const StageChanges& changes) {
            if (!m_preview) return;
            m_preview->syncWithSource();
            // The material may have gone, or come back.
            if (!changes.resyncedPaths.empty()) {
                applyMaterial();
                applySubject();
            }
            m_viewport->requestUpdate();
        });
    }
}

void ShadingPreviewWidget::rebuildPreview() {
    m_preview.reset();
    m_problems.clear();
    m_framed = false;
    m_hasCamera = false;
    m_hasLights = false;
    if (m_stage) m_preview = std::make_unique<ShadingPreviewStage>(m_stage->usdStage());
    if (m_preview) {
        applyShape();
        applyEnvironment();
        applyMaterial();
        applySubject();
    }
    // A new engine for a new stage (HydraViewportWindow::setStage()).
    m_viewport->setStage(m_preview ? m_preview->stage() : UsdStageRefPtr());
    followRenderer();
    updateShown();
}

void ShadingPreviewWidget::setMaterial(const SdfPath& material) {
    if (material == m_material) return;
    m_material = material;
    applyMaterial();
    applySubject();
    updateShown();
}

void ShadingPreviewWidget::applyMaterial() {
    if (!m_preview) return;
    std::string why;
    m_preview->setMaterial(m_material, &why);
    setProblem("material", qs(why));
    checkRenderer();
    m_viewport->requestUpdate();
}

void ShadingPreviewWidget::setSelectedNode(const SdfPath& node) {
    if (node == m_node) return;
    m_node = node;
    if (m_subject == Subject::SelectedNode) applySubject();
}

void ShadingPreviewWidget::setSubject(Subject subject) {
    if (subject == m_subject) return;
    m_subject = subject;
    applySubject();
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::applySubject() {
    if (!m_preview) return;
    SdfPath output;
    QString problem;
    if (m_subject == Subject::SelectedNode && !m_preview->material().IsEmpty()) {
        const UsdPrim node = m_stage ? m_stage->usdStage()->GetPrimAtPath(m_node) : UsdPrim();
        if (!node || !node.IsA<UsdShadeShader>() || !m_node.HasPrefix(m_preview->material())) {
            problem = tr("Select a shader node of this material to see its output.");
        } else {
            output = defaultPreviewOutput(node);
            if (output.IsEmpty()) problem = tr("%1 has no output to show.").arg(qs(m_node.GetName()));
        }
    }
    if (output != m_preview->previewedOutput() || output.IsEmpty()) {
        std::string why;
        if (!m_preview->setPreviewedOutput(output, &why)) problem = qs(why);
    }
    setProblem("node", problem);
    m_viewport->requestUpdate();
}

void ShadingPreviewWidget::setPaused(bool paused) { m_pause->setChecked(paused); }
bool ShadingPreviewWidget::isPaused() const { return m_pause->isChecked(); }

void ShadingPreviewWidget::setShapeName(const QString& name) {
    if (name == m_shapeName && m_preview && !m_preview->shape().empty()) return;
    m_shapeName = name;
    applyShape();
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::applyShape() {
    if (!m_preview) return;
    const std::vector<PreviewAsset> shapes = listPreviewAssets(utf8(previewShapeFolders()), previewShapeExtensions());
    if (shapes.empty()) {
        setProblem("shape", tr("No preview shapes in %1.").arg(previewShapeFolders().join(QStringLiteral(", "))));
        return;
    }
    const auto it = std::find_if(shapes.begin(), shapes.end(),
                                 [this](const PreviewAsset& asset) { return qs(asset.name).compare(m_shapeName, Qt::CaseInsensitive) == 0; });
    const PreviewAsset& chosen = it != shapes.end() ? *it : shapes.front();
    std::string why;
    if (m_preview->setShape(chosen.file, &why)) {
        m_shapeName = qs(chosen.name);
        setProblem("shape", QString());
        applyCamera();
        // Use scene lights applies to a shape that has some, and is where one starts. Only for
        // another file: the same one again (a new stage) keeps what was chosen for it.
        m_hasLights = m_preview->shapeHasLights();
        if (m_hasLights && chosen.file != m_lightsShape) {
            m_sceneLights = true;
            Q_EMIT stateChanged();
        }
        m_lightsShape = chosen.file;
        applyEnvironment();
        m_framed = false;
        frame();
    } else {
        setProblem("shape", qs(why));
    }
    m_viewport->requestUpdate();
}

void ShadingPreviewWidget::applyCamera() {
    const auto camera = m_preview ? m_preview->shapeCamera() : std::nullopt;
    m_hasCamera = camera.has_value();
    // The camera's lens while it's looked through and orbited from; the usual one otherwise.
    m_viewport->setFieldOfView(camera ? camera->verticalFov : HydraViewportWindow::verticalFieldOfView());
}

void ShadingPreviewWidget::resetView() {
    const auto camera = m_preview ? m_preview->shapeCamera() : std::nullopt;
    if (!camera) return;
    m_viewport->setFieldOfView(camera->verticalFov);
    const GfVec3d fromTarget = camera->eye - camera->target;
    const OrbitAngles angles = orbitAnglesLookingFrom(fromTarget, m_viewport->cameraState().yaw);
    m_viewport->setCameraState({camera->target, fromTarget.GetLength(), angles.yaw, angles.pitch});
}

void ShadingPreviewWidget::setHdriName(const QString& name) {
    m_hdriName = name;
    applyEnvironment();
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::setHdriRotation(double degrees) {
    m_rotation = degrees;
    applyEnvironment();
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::setHdriExposure(double stops) {
    m_exposure = stops;
    applyEnvironment();
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::setHdriBackground(bool shown) {
    m_background = shown;
    m_viewport->setDomeLightVisible(shown);
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::setUseSceneLights(bool use) {
    if (use == m_sceneLights) return;
    m_sceneLights = use;
    applyEnvironment();
    Q_EMIT stateChanged();
}

void ShadingPreviewWidget::applyEnvironment() {
    if (!m_preview) return;
    ShadingPreviewStage::Environment environment;
    environment.rotation = m_rotation;
    environment.exposure = m_exposure;
    environment.sceneLights = m_sceneLights && m_hasLights;
    if (environment.sceneLights) {
        setProblem("hdri", QString());  // the HDRI isn't used
    } else if (!m_hdriName.isEmpty()) {
        const std::vector<PreviewAsset> maps = listPreviewAssets(utf8(previewHdriFolders()), previewHdriExtensions());
        const auto it = std::find_if(maps.begin(), maps.end(),
                                     [this](const PreviewAsset& asset) { return qs(asset.name).compare(m_hdriName, Qt::CaseInsensitive) == 0; });
        if (it != maps.end()) environment.hdriFile = it->file;
        setProblem("hdri", it != maps.end() ? QString() : tr("No HDRI named %1: lit by a grey dome.").arg(m_hdriName));
    } else {
        setProblem("hdri", QString());
    }
    m_preview->setEnvironment(environment);
    m_viewport->requestUpdate();
}

void ShadingPreviewWidget::populateOptionsMenu(QMenu* menu) {
    menu->setToolTipsVisible(true);
    menu->addSection(tr("Show"));
    auto* subjects = new QActionGroup(menu);
    const auto addSubject = [this, menu, subjects](const QString& label, const QString& tip, Subject subject) {
        QAction* action = menu->addAction(label, this, [this, subject]() { setSubject(subject); });
        action->setCheckable(true);
        action->setChecked(m_subject == subject);
        action->setToolTip(tip);
        subjects->addAction(action);
    };
    addSubject(tr("Material"), tr("Show the material"), Subject::Material);
    addSubject(tr("Selected Node"), tr("Show the selected node's output on the shape"), Subject::SelectedNode);
    menu->addSeparator();

    // The menu is built each time it opens, so a file dropped into a folder shows up.
    QMenu* shapes = menu->addMenu(tr("Shape: %1").arg(m_shapeName));
    shapes->setObjectName(QStringLiteral("previewShapeMenu"));
    populateShapeMenu(shapes);
    const QString light = m_sceneLights && m_hasLights ? tr("Scene lights") : m_hdriName.isEmpty() ? tr("None") : m_hdriName;
    QMenu* lights = menu->addMenu(tr("Light: %1").arg(light));
    lights->setObjectName(QStringLiteral("previewLightMenu"));
    populateLightMenu(lights);

    if (m_hasCamera) {
        menu->addSeparator();
        QAction* reset = menu->addAction(tr("Reset View"), this, [this]() { resetView(); });
        reset->setObjectName(QStringLiteral("previewResetView"));
        reset->setToolTip(tr("Look through the shape file's camera again"));
    }
}

void ShadingPreviewWidget::populateShapeMenu(QMenu* menu) {
    menu->clear();
    menu->setToolTipsVisible(true);
    auto* group = new QActionGroup(menu);
    for (const PreviewAsset& asset : listPreviewAssets(utf8(previewShapeFolders()), previewShapeExtensions())) {
        const QString name = qs(asset.name);
        QAction* action = menu->addAction(name, this, [this, name]() { setShapeName(name); });
        action->setCheckable(true);
        action->setChecked(name.compare(m_shapeName, Qt::CaseInsensitive) == 0);
        action->setToolTip(qs(asset.file));
        group->addAction(action);
    }
    if (menu->isEmpty()) menu->addAction(tr("No shapes found"))->setEnabled(false);
}

void ShadingPreviewWidget::populateLightMenu(QMenu* menu) {
    menu->clear();
    // A shape with lights of its own can be lit by them alone; the HDRI's options then
    // don't apply, and are disabled.
    const bool sceneLights = m_sceneLights && m_hasLights;
    if (m_hasLights) {
        QAction* use = menu->addAction(tr("Use scene lights"), this, [this](bool on) { setUseSceneLights(on); });
        use->setObjectName(QStringLiteral("previewSceneLights"));
        use->setCheckable(true);
        use->setChecked(sceneLights);
        use->setToolTip(tr("Light the shape with its own lights only: no HDRI dome or key light"));
        menu->addSeparator();
    }
    auto* group = new QActionGroup(menu);
    const auto add = [this, menu, group, sceneLights](const QString& label, const QString& name, const QString& tip) {
        QAction* action = menu->addAction(label, this, [this, name]() { setHdriName(name); });
        action->setCheckable(true);
        action->setChecked(m_hdriName.compare(name, Qt::CaseInsensitive) == 0);
        action->setToolTip(tip);
        action->setEnabled(!sceneLights);
        group->addAction(action);
    };
    add(tr("None (Grey Dome)"), QString(), QString());
    for (const PreviewAsset& asset : listPreviewAssets(utf8(previewHdriFolders()), previewHdriExtensions())) {
        add(qs(asset.name), qs(asset.name), qs(asset.file));
    }
    menu->setToolTipsVisible(true);
    menu->addSeparator();
    QAction* background = menu->addAction(tr("Show as Background"), this, [this](bool on) { setHdriBackground(on); });
    background->setCheckable(true);
    background->setChecked(m_background);
    background->setEnabled(!sceneLights);

    // Rotation and exposure, live as they're dragged.
    auto* controls = new QWidget(menu);
    auto* grid = new QVBoxLayout(controls);
    usdcc::ui::scaled::margins(grid, 12, 4, 12, 4);
    auto* rotationLabel = new QLabel(tr("Rotation %1\xC2\xB0").arg(qRound(m_rotation)), controls);
    auto* rotation = new QSlider(Qt::Horizontal, controls);
    rotation->setObjectName(QStringLiteral("previewHdriRotation"));
    rotation->setRange(-180, 180);
    rotation->setValue(qRound(m_rotation));
    connect(rotation, &QSlider::valueChanged, this, [this, rotationLabel](int value) {
        rotationLabel->setText(tr("Rotation %1\xC2\xB0").arg(value));
        setHdriRotation(value);
    });
    auto* exposureRow = new QHBoxLayout;
    auto* exposure = new QDoubleSpinBox(controls);
    exposure->setObjectName(QStringLiteral("previewHdriExposure"));
    exposure->setRange(-10.0, 10.0);
    exposure->setSingleStep(0.25);
    exposure->setValue(m_exposure);
    exposure->setSuffix(tr(" stops"));
    connect(exposure, &QDoubleSpinBox::valueChanged, this, [this](double value) { setHdriExposure(value); });
    exposureRow->addWidget(new QLabel(tr("Exposure"), controls));
    exposureRow->addWidget(exposure, 1);
    grid->addWidget(rotationLabel);
    grid->addWidget(rotation);
    grid->addLayout(exposureRow);
    controls->setEnabled(!sceneLights);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(controls);
    action->setEnabled(!sceneLights);
    menu->addAction(action);
}

void ShadingPreviewWidget::followRenderer() {
    TfToken renderer = ViewportViewPanel::rendererFor(m_stage);
    if (renderer.IsEmpty()) renderer = kStorm;
    if (renderer != m_viewport->currentRendererPlugin()) m_viewport->setRendererPlugin(renderer);
    checkRenderer();
}

void ShadingPreviewWidget::checkRenderer() {
    QString problem;
    if (m_preview && !m_preview->material().IsEmpty()) {
        const std::vector<std::string> terminals = m_preview->surfaceTerminals();
        const std::string renderer = m_viewport->currentRendererPlugin().GetString();
        if (!terminals.empty() && !renderer.empty()) {
            auto cached = m_rendererContexts.find(renderer);
            if (cached == m_rendererContexts.end()) {
                cached = m_rendererContexts.emplace(renderer, renderDelegateShading(renderer).materialContexts).first;
            }
            const std::vector<std::string>& contexts = cached->second;
            // outputs:surface is the universal context, which UsdImaging falls back to for
            // every renderer, so a renderer needn't list it.
            const bool readable = contexts.empty() || std::any_of(terminals.begin(), terminals.end(), [&contexts](const std::string& terminal) {
                                      const std::string context = contextOf(terminal);
                                      return context.empty() || std::find(contexts.begin(), contexts.end(), context) != contexts.end();
                                  });
            if (!readable) {
                QStringList names;
                for (const std::string& terminal : terminals) names << qs(terminal);
                QString display = m_viewport->rendererDisplayName(TfToken(renderer));
                if (display.isEmpty()) display = qs(renderer);
                problem = tr("%1 can't draw this material: it reads none of %2.").arg(display, names.join(QStringLiteral(", ")));
            }
        }
    }
    setProblem("renderer", problem);
}

void ShadingPreviewWidget::setProblem(const std::string& key, const QString& text) {
    if (text.isEmpty()) {
        m_problems.erase(key);
    } else {
        m_problems[key] = text;
    }
    const QString all = problem();
    if (all == m_problem->text()) return;
    m_problem->setText(all);
    m_problem->setVisible(!all.isEmpty());
    Q_EMIT squareHeightChanged();
}

void ShadingPreviewWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_stack->setMaximumHeight(std::max(m_stack->minimumHeight(), width()));
}

int ShadingPreviewWidget::heightForSquare(int width) const {
    int height = width + m_toolbar->sizeHint().height();
    if (!m_problem->isHidden()) height += m_problem->heightForWidth(width);
    return height;
}

QString ShadingPreviewWidget::problem() const {
    QStringList lines;
    for (const char* key : {"material", "renderer", "node", "shape", "hdri"}) {
        const auto it = m_problems.find(key);
        if (it != m_problems.end()) lines << it->second;
    }
    return lines.join(QLatin1Char('\n'));
}

QString ShadingPreviewWidget::emptyText() const {
    if (!m_stage) return tr("No stage.");
    if (!m_preview || m_preview->material().IsEmpty()) return tr("Select a material, or a node in one, to preview it.");
    return {};
}

void ShadingPreviewWidget::updateShown() {
    const QString text = emptyText();
    m_empty->setText(text);
    m_stack->setCurrentWidget(text.isEmpty() ? m_viewportContainer : static_cast<QWidget*>(m_empty));
}

void ShadingPreviewWidget::frame() {
    if (!m_preview) return;
    if (m_hasCamera) {
        resetView();
    } else {
        const GfRange3d bounds = m_preview->shapeBounds();
        if (bounds.IsEmpty()) return;
        m_viewport->frameBox(bounds);
    }
    m_framed = m_viewport->hasRenderedFirstFrame();
}

QJsonObject ShadingPreviewWidget::saveState() const {
    return {{QStringLiteral("shape"), m_shapeName},
            {QStringLiteral("hdri"), m_hdriName},
            {QStringLiteral("rotation"), m_rotation},
            {QStringLiteral("exposure"), m_exposure},
            {QStringLiteral("background"), m_background},
            {QStringLiteral("sceneLights"), m_sceneLights},
            {QStringLiteral("paused"), isPaused()},
            {QStringLiteral("subject"), m_subject == Subject::SelectedNode ? QStringLiteral("node") : QStringLiteral("material")}};
}

void ShadingPreviewWidget::restoreState(const QJsonObject& state) {
    if (state.contains(QStringLiteral("shape"))) {
        m_shapeName = state.value(QStringLiteral("shape")).toString();
        applyShape();
    }
    if (state.contains(QStringLiteral("hdri"))) m_hdriName = state.value(QStringLiteral("hdri")).toString();
    m_rotation = state.value(QStringLiteral("rotation")).toDouble(m_rotation);
    m_sceneLights = state.value(QStringLiteral("sceneLights")).toBool(m_sceneLights);
    m_exposure = state.value(QStringLiteral("exposure")).toDouble(m_exposure);
    applyEnvironment();
    setHdriBackground(state.value(QStringLiteral("background")).toBool(m_background));
    setPaused(state.value(QStringLiteral("paused")).toBool(isPaused()));
    setSubject(state.value(QStringLiteral("subject")).toString() == QLatin1String("node") ? Subject::SelectedNode : Subject::Material);
}

}  // namespace usdcc::usd
