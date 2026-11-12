#include "usdcc/usd/viewport_view_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/scene_bounds.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QJsonArray>
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

namespace {
// Every open viewport, oldest first, and the one clicked last (see activeViewport()).
std::vector<ViewportViewPanel*>& openViewports() {
    static std::vector<ViewportViewPanel*> viewports;
    return viewports;
}
ViewportViewPanel* s_clickedViewport = nullptr;
// Every open viewport, the one clicked last at the back (see rendererFor()).
std::vector<ViewportViewPanel*>& clickOrder() {
    static std::vector<ViewportViewPanel*> viewports;
    return viewports;
}

QIcon colorSwatch(const QColor& color) {
    QPixmap pixmap(28, 14);
    pixmap.fill(color);
    QPainter painter(&pixmap);
    painter.setPen(QColor(255, 255, 255, 60));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return QIcon(pixmap);
}

// The Display dialog (grid, background and selection): this viewport's values, each with a
// Default button that shows while the viewport overrides it and drops the override. Edits
// apply at once.
QDialog* makeDisplayDialog(ViewportViewPanel* panel) {
    auto* dialog = new QDialog(panel);
    dialog->setObjectName(QStringLiteral("viewportDisplayDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QObject::tr("Viewport Display - %1").arg(panel->windowTitle()));

    auto* layout = new QVBoxLayout(dialog);
    auto* grid = new QGridLayout();
    layout->addLayout(grid);

    auto* showGrid = new QCheckBox(QObject::tr("Show grid"), dialog);
    showGrid->setObjectName(QStringLiteral("displayShowGrid"));
    auto* size = new QDoubleSpinBox(dialog);
    size->setObjectName(QStringLiteral("displayGridSize"));
    size->setRange(0.01, 1000000.0);
    size->setDecimals(3);
    auto* step = new QDoubleSpinBox(dialog);
    step->setObjectName(QStringLiteral("displayGridStep"));
    step->setRange(0.001, 100000.0);
    step->setDecimals(3);
    for (QDoubleSpinBox* spin : {size, step}) spin->setKeyboardTracking(false);  // a value per edit, not per key
    auto* background = new QPushButton(dialog);
    background->setObjectName(QStringLiteral("displayBackground"));
    background->setIconSize(QSize(28, 14));
    // Enter in a spin box only commits its value: not the dialog's default button, so it
    // doesn't open the picker (or, below, close the dialog).
    background->setAutoDefault(false);
    auto* selectionStyle = new QComboBox(dialog);
    selectionStyle->setObjectName(QStringLiteral("displaySelectionStyle"));
    for (SelectionStyle style : {SelectionStyle::Solid, SelectionStyle::Outline, SelectionStyle::Wireframe}) {
        selectionStyle->addItem(QObject::tr(qPrintable(selectionStyleLabel(style))), static_cast<int>(style));
    }
    auto* selectionColor = new QPushButton(dialog);
    selectionColor->setObjectName(QStringLiteral("displaySelectionColor"));
    selectionColor->setIconSize(QSize(28, 14));
    selectionColor->setAutoDefault(false);
    auto* selectionWidth = new QSpinBox(dialog);
    selectionWidth->setObjectName(QStringLiteral("displaySelectionWidth"));
    selectionWidth->setRange(kMinSelectionWidth, kMaxSelectionWidth);
    selectionWidth->setSuffix(QObject::tr(" px"));
    selectionWidth->setKeyboardTracking(false);

    auto addRow = [dialog, grid](int row, const QString& label, QWidget* editor, const char* name) {
        if (!label.isEmpty()) grid->addWidget(new QLabel(label, dialog), row, 0);
        grid->addWidget(editor, row, 1);
        auto* reset = new QToolButton(dialog);
        reset->setObjectName(QString::fromLatin1(name));
        reset->setText(QObject::tr("Default"));
        reset->setToolTip(QObject::tr("Use the Settings > Viewport value"));
        grid->addWidget(reset, row, 2);
        return reset;
    };
    QToolButton* resetShow = addRow(0, QString(), showGrid, "resetShowGrid");
    QToolButton* resetSize = addRow(1, QObject::tr("Grid size"), size, "resetGridSize");
    QToolButton* resetStep = addRow(2, QObject::tr("Grid step"), step, "resetGridStep");
    QToolButton* resetBackground = addRow(3, QObject::tr("Background"), background, "resetBackground");
    QToolButton* resetSelectionStyle = addRow(4, QObject::tr("Selection"), selectionStyle, "resetSelectionStyle");
    QToolButton* resetSelectionColor = addRow(5, QObject::tr("Selection color"), selectionColor, "resetSelectionColor");
    QToolButton* resetSelectionWidth = addRow(6, QObject::tr("Line width"), selectionWidth, "resetSelectionWidth");

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    // Shows the panel's values; run again whenever they change, from here or elsewhere.
    auto refresh = [=]() {
        const HydraViewportWindow::DisplayOptions shown = panel->displayOptions();
        const ViewportDisplayOverrides overrides = panel->displayOverrides();
        const QSignalBlocker blockShow(showGrid), blockSize(size), blockStep(step), blockStyle(selectionStyle),
            blockWidth(selectionWidth);
        showGrid->setChecked(shown.showGrid);
        size->setValue(shown.gridSize);
        step->setValue(shown.gridStep);
        background->setIcon(colorSwatch(shown.background));
        background->setText(shown.background.name());
        selectionStyle->setCurrentIndex(selectionStyle->findData(static_cast<int>(shown.selectionStyle)));
        selectionColor->setIcon(colorSwatch(shown.selectionColor));
        selectionColor->setText(shown.selectionColor.name());
        selectionWidth->setValue(shown.selectionWidth);
        selectionWidth->setEnabled(shown.selectionStyle != SelectionStyle::Solid);  // Solid draws no lines
        resetShow->setVisible(overrides.showGrid.has_value());
        resetSize->setVisible(overrides.gridSize.has_value());
        resetStep->setVisible(overrides.gridStep.has_value());
        resetBackground->setVisible(overrides.background.has_value());
        resetSelectionStyle->setVisible(overrides.selectionStyle.has_value());
        resetSelectionColor->setVisible(overrides.selectionColor.has_value());
        resetSelectionWidth->setVisible(overrides.selectionWidth.has_value());
    };
    refresh();
    QObject::connect(panel, &ViewportViewPanel::displayChanged, dialog, refresh);

    // Changes one override, leaving the others as they are.
    auto edit = [panel](auto change) {
        ViewportDisplayOverrides overrides = panel->displayOverrides();
        change(overrides);
        panel->setDisplayOverrides(overrides);
    };
    QObject::connect(showGrid, &QCheckBox::toggled, dialog,
                     [edit](bool on) { edit([on](ViewportDisplayOverrides& o) { o.showGrid = on; }); });
    QObject::connect(size, &QDoubleSpinBox::valueChanged, dialog,
                     [edit](double v) { edit([v](ViewportDisplayOverrides& o) { o.gridSize = v; }); });
    QObject::connect(step, &QDoubleSpinBox::valueChanged, dialog,
                     [edit](double v) { edit([v](ViewportDisplayOverrides& o) { o.gridStep = v; }); });
    // A swatch's picker: the viewport follows it while it's open; Cancel puts back what was there.
    auto pickColor = [dialog, panel, edit](const QString& title, QColor HydraViewportWindow::DisplayOptions::*shown,
                                           std::optional<QColor> ViewportDisplayOverrides::*override) {
        const ViewportDisplayOverrides before = panel->displayOverrides();
        QColorDialog picker(panel->displayOptions().*shown, dialog);
        picker.setOption(QColorDialog::DontUseNativeDialog);
        picker.setWindowTitle(title);
        QObject::connect(&picker, &QColorDialog::currentColorChanged, &picker, [edit, override](const QColor& c) {
            edit([c, override](ViewportDisplayOverrides& o) { o.*override = c; });
        });
        if (picker.exec() == QDialog::Accepted) {
            const QColor chosen = picker.selectedColor();
            edit([chosen, override](ViewportDisplayOverrides& o) { o.*override = chosen; });
        } else {
            panel->setDisplayOverrides(before);
        }
    };
    QObject::connect(background, &QPushButton::clicked, dialog, [pickColor]() {
        pickColor(QObject::tr("Viewport Background"), &HydraViewportWindow::DisplayOptions::background,
                  &ViewportDisplayOverrides::background);
    });
    QObject::connect(selectionColor, &QPushButton::clicked, dialog, [pickColor]() {
        pickColor(QObject::tr("Selection Color"), &HydraViewportWindow::DisplayOptions::selectionColor,
                  &ViewportDisplayOverrides::selectionColor);
    });
    QObject::connect(selectionStyle, &QComboBox::currentIndexChanged, dialog, [edit, selectionStyle](int) {
        const auto style = static_cast<SelectionStyle>(selectionStyle->currentData().toInt());
        edit([style](ViewportDisplayOverrides& o) { o.selectionStyle = style; });
    });
    QObject::connect(selectionWidth, &QSpinBox::valueChanged, dialog,
                     [edit](int v) { edit([v](ViewportDisplayOverrides& o) { o.selectionWidth = v; }); });
    QObject::connect(resetShow, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.showGrid.reset(); }); });
    QObject::connect(resetSize, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.gridSize.reset(); }); });
    QObject::connect(resetStep, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.gridStep.reset(); }); });
    QObject::connect(resetBackground, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.background.reset(); }); });
    QObject::connect(resetSelectionStyle, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.selectionStyle.reset(); }); });
    QObject::connect(resetSelectionColor, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.selectionColor.reset(); }); });
    QObject::connect(resetSelectionWidth, &QToolButton::clicked, dialog,
                     [edit]() { edit([](ViewportDisplayOverrides& o) { o.selectionWidth.reset(); }); });
    return dialog;
}
}  // namespace

ViewportViewPanel* ViewportViewPanel::activeViewport() {
    if (s_clickedViewport) return s_clickedViewport;
    return openViewports().empty() ? nullptr : openViewports().front();
}

usdcc::core::Signal<>& ViewportViewPanel::activeViewportChanged() {
    static usdcc::core::Signal<> signal;
    return signal;
}

usdcc::core::Signal<>& ViewportViewPanel::viewportsChanged() {
    static usdcc::core::Signal<> signal;
    return signal;
}

PXR_NS::TfToken ViewportViewPanel::rendererFor(const StageRefPtr& stage) {
    if (!stage) return {};
    const std::vector<ViewportViewPanel*>& clicked = clickOrder();
    for (auto it = clicked.rbegin(); it != clicked.rend(); ++it) {
        if ((*it)->currentStage() == stage) return (*it)->m_viewport->currentRendererPlugin();
    }
    for (ViewportViewPanel* viewport : openViewports()) {
        if (viewport->currentStage() == stage) return viewport->m_viewport->currentRendererPlugin();
    }
    return {};
}

ViewportViewPanel::~ViewportViewPanel() {
    const bool wasActive = activeViewport() == this;
    std::erase(openViewports(), this);
    std::erase(clickOrder(), this);
    if (s_clickedViewport == this) s_clickedViewport = nullptr;
    if (wasActive) activeViewportChanged()();
    viewportsChanged()();
}

void ViewportViewPanel::makeActive() {
    if (clickOrder().empty() || clickOrder().back() != this) {
        std::erase(clickOrder(), this);
        clickOrder().push_back(this);
        viewportsChanged()();
    }
    if (activeViewport() == this) {
        s_clickedViewport = this;
        return;
    }
    s_clickedViewport = this;
    activeViewportChanged()();
}

ViewportViewPanel::ViewportViewPanel(StageManager* stageManager, StageRefPtr stage,
                                      std::shared_ptr<usdcc::tools::ToolSet> toolSet,
                                      std::shared_ptr<UndoManager> undoManager, const QString& title,
                                      QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent),
      m_toolSet(std::move(toolSet)),
      m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);

    addPanelControls(toolbarLayout);

    // Embedded via createWindowContainer() (QWindow, not QOpenGLWidget) —
    // see hydra_viewport_window.h for why.
    m_viewport = new HydraViewportWindow();
    m_viewport->setToolSet(m_toolSet);
    m_viewport->setDisplayOptions(displayOptions());
    // What isn't overridden follows Settings > Viewport as it changes.
    usdcc::ui::onSettingsChanged(this, [this](const std::string& key) {
        if (key == kViewportShowGridKey || key == kViewportGridSizeKey || key == kViewportGridStepKey ||
            key == kViewportBackgroundKey || key == kViewportSelectionStyleKey || key == kViewportSelectionColorKey ||
            key == kViewportSelectionWidthKey) {
            applyDisplay();
        }
    });
    QWidget* viewportContainer = QWidget::createWindowContainer(m_viewport, container);

    layout->addWidget(toolbar);
    layout->addWidget(viewportContainer, 1);

    setContentWidget(container);

    connect(m_viewport, &HydraViewportWindow::selectionRequested, this, &ViewportViewPanel::onSelectionRequested);
    // The options list the renderers, and tick the one in use, which a script can switch.
    connect(m_viewport, &HydraViewportWindow::rendererPluginsChanged, this, &ViewportViewPanel::notifyOptionsChanged);
    connect(m_viewport, &HydraViewportWindow::rendererChanged, this, &ViewportViewPanel::notifyOptionsChanged);
    // What a Material Editor's preview follows (rendererFor()).
    connect(m_viewport, &HydraViewportWindow::rendererPluginsChanged, this, []() { viewportsChanged()(); });
    connect(m_viewport, &HydraViewportWindow::rendererChanged, this, []() { viewportsChanged()(); });
    // A click in the viewport focuses this panel, which makes its stage the
    // current one (CurrentStageTracker): ADS doesn't see the native window's
    // focus. Only the highlight moves; the viewport keeps the keyboard.
    connect(m_viewport, &HydraViewportWindow::pressed, this, [this]() {
        makeActive();
        if (dockManager()) dockManager()->setDockWidgetFocused(this);
    });
    connect(m_viewport, &HydraViewportWindow::cameraChanged, this, [this]() {
        if (activeViewport() == this) activeViewportChanged()();
    });
    openViewports().push_back(this);
    viewportsChanged()();
    // One undo step per gizmo drag. The stage is remembered at the start so the
    // session is closed on the same stage it was opened on.
    connect(m_viewport, &HydraViewportWindow::toolEditStarted, this, [this](const QString& description) {
        if (m_undoManager && currentStage()) {
            m_toolEditStage = currentStage();
            m_undoManager->beginEdit(m_toolEditStage, description);
        }
    });
    connect(m_viewport, &HydraViewportWindow::toolEditFinished, this, [this]() {
        if (m_undoManager && m_toolEditStage) {
            m_undoManager->endEdit(m_toolEditStage);
            m_toolEditStage.reset();
        }
    });

    // NB: uses the `stageManager` constructor parameter directly, not the
    // stageManager() accessor — the parameter shadows it in this scope.
    m_selectionChangedConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
            onStageSelectionChanged(stage, std::move(paths));
        });

    refreshStageCombo();
    // The combo's initial selection can't trigger this itself: ViewPanel
    // already holds the initial stage, so setStage() sees it as unchanged.
    onStageChanged(currentStage());
}

void ViewportViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_viewport->setStage(stage ? stage->usdStage() : PXR_NS::UsdStageRefPtr());
    m_viewport->setSelectedPaths(stageManager()->selectedPaths(stage));
    if (activeViewport() == this) activeViewportChanged()();  // the active camera's stage
    viewportsChanged()();
    if (!stage) {
        m_stageChangedConnection = {};
        m_targetConnection = {};
        m_viewport->setTargetXformOp(PXR_NS::TfToken());
        m_viewport->requestUpdate();
        return;
    }
    // The viewport only draws when asked: redraw after every edit, wherever it
    // came from (undo/redo, another panel, a script) — requestUpdate() folds a
    // burst of notices into one frame.
    m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) { m_viewport->requestUpdate(); });
    m_targetConnection = stage->targetXformOpChanged.connect([this]() { syncTarget(); });
    syncTarget();
}

void ViewportViewPanel::syncTarget() {
    const std::vector<PXR_NS::SdfPath> selection = stageManager()->selectedPaths(currentStage());
    m_viewport->setTargetXformOp(selection.empty() || !currentStage() ? PXR_NS::TfToken()
                                                   : currentStage()->targetXformOpFor(selection.front()));
}

void ViewportViewPanel::onSelectionRequested(std::vector<PXR_NS::SdfPath> paths) {
    stageManager()->setSelectedPaths(currentStage(), std::move(paths));
}

void ViewportViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage == currentStage()) {
        m_viewport->setSelectedPaths(std::move(paths));
        syncTarget();
    }
}

HydraViewportWindow::DisplayOptions ViewportViewPanel::displayOptions() const {
    return m_displayOverrides.applyTo(defaultViewportDisplay());
}

void ViewportViewPanel::setDisplayOverrides(const ViewportDisplayOverrides& overrides) {
    if (overrides == m_displayOverrides) return;
    m_displayOverrides = overrides;
    applyDisplay();
}

void ViewportViewPanel::applyDisplay() {
    m_viewport->setDisplayOptions(displayOptions());
    Q_EMIT displayChanged();
    notifyOptionsChanged();  // Show Grid's tick, Use Settings Defaults
}

QDialog* ViewportViewPanel::showDisplayDialog() {
    if (!m_displayDialog) m_displayDialog = makeDisplayDialog(this);
    m_displayDialog->show();
    m_displayDialog->raise();
    m_displayDialog->activateWindow();
    return m_displayDialog;
}

usdcc::ui::ViewPanel* ViewportViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new ViewportViewPanel(stageManager(), currentStage(), m_toolSet, m_undoManager, windowTitle(), parent);
    copy->setDisplayOverrides(m_displayOverrides);

    // Renderer selection and camera state: the duplicate's HydraViewportWindow
    // has no GL context yet (it's created lazily on first expose), so it has
    // no renderers to pick from until rendererPluginsChanged() fires. Apply
    // both once that happens rather than immediately.
    const PXR_NS::TfToken rendererPluginId = m_viewport->currentRendererPlugin();
    const HydraViewportWindow::CameraState cameraState = m_viewport->cameraState();
    connect(copy->m_viewport, &HydraViewportWindow::rendererPluginsChanged, copy,
            [copy, rendererPluginId, cameraState]() {
                const std::vector<PXR_NS::TfToken> plugins = copy->m_viewport->rendererPlugins();
                if (std::find(plugins.begin(), plugins.end(), rendererPluginId) != plugins.end()) {
                    copy->m_viewport->setRendererPlugin(rendererPluginId);
                }
                copy->m_viewport->setCameraState(cameraState);
            });

    return copy;
}

QJsonObject ViewportViewPanel::saveSessionState() const {
    const HydraViewportWindow::CameraState camera = m_viewport->cameraState();
    QJsonObject orbit;
    orbit.insert(QStringLiteral("target"), QJsonArray{camera.target[0], camera.target[1], camera.target[2]});
    orbit.insert(QStringLiteral("distance"), camera.distance);
    orbit.insert(QStringLiteral("yaw"), camera.yaw);
    orbit.insert(QStringLiteral("pitch"), camera.pitch);
    QJsonObject state;
    state.insert(QStringLiteral("camera"), orbit);
    const PXR_NS::TfToken renderer = m_viewport->currentRendererPlugin();
    if (!renderer.IsEmpty()) state.insert(QStringLiteral("renderer"), QString::fromStdString(renderer.GetString()));
    if (!m_displayOverrides.isEmpty()) state.insert(QStringLiteral("display"), m_displayOverrides.toJson());
    return state;
}

void ViewportViewPanel::restoreSessionState(const QJsonObject& state) {
    // Needs no renderer, so applies at once. None saved (an older session): no overrides.
    setDisplayOverrides(ViewportDisplayOverrides::fromJson(state.value(QStringLiteral("display")).toObject()));
    HydraViewportWindow::CameraState camera = m_viewport->cameraState();
    const QJsonObject orbit = state.value(QStringLiteral("camera")).toObject();
    const QJsonArray target = orbit.value(QStringLiteral("target")).toArray();
    if (target.size() == 3) camera.target = PXR_NS::GfVec3d(target[0].toDouble(), target[1].toDouble(), target[2].toDouble());
    camera.distance = orbit.value(QStringLiteral("distance")).toDouble(camera.distance);
    camera.yaw = orbit.value(QStringLiteral("yaw")).toDouble(camera.yaw);
    camera.pitch = orbit.value(QStringLiteral("pitch")).toDouble(camera.pitch);
    const PXR_NS::TfToken renderer(state.value(QStringLiteral("renderer")).toString().toStdString());
    auto apply = [this, camera, renderer]() {
        const std::vector<PXR_NS::TfToken> plugins = m_viewport->rendererPlugins();
        if (!renderer.IsEmpty() && std::find(plugins.begin(), plugins.end(), renderer) != plugins.end()) {
            m_viewport->setRendererPlugin(renderer);
        }
        m_viewport->setCameraState(camera);
    };
    // As for duplicate(): a viewport that hasn't drawn yet has no renderers to pick from.
    if (!m_viewport->rendererPlugins().empty()) {
        apply();
    } else {
        connect(m_viewport, &HydraViewportWindow::rendererPluginsChanged, this, apply, Qt::SingleShotConnection);
    }
}

bool ViewportViewPanel::fitToView() {
    const StageRefPtr stage = currentStage();
    if (!stage) return false;
    const std::vector<PXR_NS::SdfPath> selection = stageManager()->selectedPaths(stage);
    const std::optional<PXR_NS::GfRange3d> bounds = sceneBounds(stage->usdStage(), selection);
    if (!bounds) {
        const usdcc::core::ToastRequest toast{.kind = usdcc::core::ToastKind::Warning, .title = "Nothing to fit to view"};
        if (selection.empty()) {
            USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Warning, "usdcc.viewport", toast, "The stage has no geometry.");
        } else if (selection.size() == 1) {
            USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Warning, "usdcc.viewport", toast,
                               "{} has no bounds.", selection.front().GetName());
        } else {
            USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Warning, "usdcc.viewport", toast,
                               "None of the {} selected prims has bounds.", selection.size());
        }
        return false;
    }
    m_viewport->frameBox(*bounds);
    return true;
}

QAction* ViewportViewPanel::addFitToViewAction(QWidget* window) {
    usdcc::core::Settings::instance().define({
        .key = kFitToViewShortcutKey,
        .category = "Shortcuts",
        .group = "Viewport",
        .label = "Fit to view",
        .description = "Frame the selected prims in the viewport, or the whole stage when nothing is selected.",
        .type = usdcc::core::SettingType::Shortcut,
        .defaultValue = std::string("F"),
    });
    auto* action = new QAction(tr("Fit to View"), window);
    action->setObjectName(QStringLiteral("fitToViewAction"));
    // Application-wide like the tool keys (see ToolRail::addTool()): it also works in a
    // floating viewport, and text fields still get the letter typed into them.
    action->setShortcutContext(Qt::ApplicationShortcut);
    usdcc::ui::bindShortcut(action, kFitToViewShortcutKey);
    window->addAction(action);
    connect(action, &QAction::triggered, window, []() {
        if (ViewportViewPanel* viewport = activeViewport()) viewport->fitToView();
    });
    return action;
}

void ViewportViewPanel::populateContextMenu(QMenu* menu) {
    usdcc::ui::ViewPanel::populateContextMenu(menu);
    // The key is shown, not set: the main window's action has it (addFitToViewAction()).
    const QKeySequence key = usdcc::ui::settingShortcut(kFitToViewShortcutKey);
    const QString fitText = key.isEmpty() ? tr("Fit to View")
                                          : tr("Fit to View") + QStringLiteral("\t") + key.toString(QKeySequence::NativeText);
    menu->addAction(fitText, this, [this]() { fitToView(); });
    menu->addAction(tr("Reset Camera"), this, [this]() { m_viewport->setCameraState({}); });
}

void ViewportViewPanel::populateOptionsMenu(QMenu* menu) {
    QMenu* renderers = menu->addMenu(tr("Renderer"));
    // Empty until the viewport has been shown once: its GL context, and with
    // it the Hydra engine that lists the plugins, is created on first expose.
    const std::vector<PXR_NS::TfToken> plugins = m_viewport->rendererPlugins();
    renderers->setEnabled(!plugins.empty());

    auto* group = new QActionGroup(renderers);  // exclusive: one renderer checked
    const PXR_NS::TfToken current = m_viewport->currentRendererPlugin();
    for (const PXR_NS::TfToken& pluginId : plugins) {
        QAction* action = renderers->addAction(m_viewport->rendererDisplayName(pluginId));
        action->setCheckable(true);
        action->setChecked(pluginId == current);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, pluginId]() { m_viewport->setRendererPlugin(pluginId); });
    }

    menu->addSeparator();
    QAction* showGrid = menu->addAction(tr("Show Grid"));
    showGrid->setObjectName(QStringLiteral("showGridAction"));
    showGrid->setCheckable(true);
    showGrid->setChecked(displayOptions().showGrid);
    connect(showGrid, &QAction::triggered, this, [this](bool on) {
        ViewportDisplayOverrides overrides = m_displayOverrides;
        overrides.showGrid = on;
        setDisplayOverrides(overrides);
    });
    QMenu* selection = menu->addMenu(tr("Selection"));
    selection->setObjectName(QStringLiteral("selectionStyleMenu"));
    auto* styles = new QActionGroup(selection);  // exclusive: one style checked
    const SelectionStyle currentStyle = displayOptions().selectionStyle;
    for (SelectionStyle style : {SelectionStyle::Solid, SelectionStyle::Outline, SelectionStyle::Wireframe}) {
        QAction* action = selection->addAction(tr(qPrintable(selectionStyleLabel(style))));
        action->setObjectName(QStringLiteral("selectionStyle_%1").arg(QLatin1String(selectionStyleName(style))));
        action->setCheckable(true);
        action->setChecked(style == currentStyle);
        styles->addAction(action);
        connect(action, &QAction::triggered, this, [this, style]() {
            ViewportDisplayOverrides overrides = m_displayOverrides;
            overrides.selectionStyle = style;
            setDisplayOverrides(overrides);
        });
    }
    QAction* dialog = menu->addAction(tr("Display..."), this, [this]() { showDisplayDialog(); });
    dialog->setObjectName(QStringLiteral("displayDialogAction"));
    QAction* defaults = menu->addAction(tr("Use Settings Defaults"), this, [this]() { setDisplayOverrides({}); });
    defaults->setObjectName(QStringLiteral("displayDefaultsAction"));
    defaults->setToolTip(tr("Drop this viewport's own grid, background and selection values"));
    defaults->setEnabled(!m_displayOverrides.isEmpty());
}

}  // namespace usdcc::usd
