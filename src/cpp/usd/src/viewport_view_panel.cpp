#include "usdcc/usd/viewport_view_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/renderer_settings.h"
#include "usdcc/usd/scene_bounds.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_render_params.h"

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
#include <QElapsedTimer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace usdcc::usd {

namespace {
// Every open viewport, oldest first, and the one clicked last (see activeViewport()).
std::vector<ViewportViewPanel*>& viewportList() {
    static std::vector<ViewportViewPanel*> viewports;
    return viewports;
}
ViewportViewPanel* s_clickedViewport = nullptr;
int s_lastViewportId = 0;  // ViewportViewPanel::id()
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

// The Display dialog (grid, background, selection, and how Hydra draws): this viewport's values, each with a
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

    // How Hydra draws the stage (docs/VIEWPORT_DISPLAY.md): a row each, below the selection's.
    // Each keeps itself up to date as refresh() does for the rows above.
    auto updates = std::make_shared<std::vector<std::function<void()>>>();
    int row = 7;
    const QString builtInTip = QObject::tr("Use the built-in value");
    // A choice: a combo of `values`, labelled by `labelOf`.
    auto addChoice = [&](const QString& label, const char* editorName, const char* resetName, const auto& values, auto labelOf,
                         auto option, auto override, bool hasSetting) {
        auto* combo = new QComboBox(dialog);
        combo->setObjectName(QString::fromLatin1(editorName));
        for (auto value : values) combo->addItem(QObject::tr(qPrintable(labelOf(value))), static_cast<int>(value));
        QToolButton* reset = addRow(row++, label, combo, resetName);
        if (!hasSetting) reset->setToolTip(builtInTip);
        updates->push_back([panel, combo, reset, option, override]() {
            const QSignalBlocker block(combo);
            combo->setCurrentIndex(combo->findData(static_cast<int>(panel->displayOptions().*option)));
            reset->setVisible((panel->displayOverrides().*override).has_value());
        });
        QObject::connect(combo, &QComboBox::currentIndexChanged, dialog, [edit, combo, override](int) {
            using Value = typename std::remove_reference_t<decltype(std::declval<ViewportDisplayOverrides>().*override)>::value_type;
            const auto value = static_cast<Value>(combo->currentData().toInt());
            edit([value, override](ViewportDisplayOverrides& o) { o.*override = value; });
        });
        QObject::connect(reset, &QToolButton::clicked, dialog,
                         [edit, override]() { edit([override](ViewportDisplayOverrides& o) { (o.*override).reset(); }); });
    };
    addChoice(QObject::tr("Shading"), "displayShading", "resetShading", kShadingModes, &shadingModeLabel,
              &HydraViewportWindow::DisplayOptions::shading, &ViewportDisplayOverrides::shading, false);
    addChoice(QObject::tr("Complexity"), "displayComplexity", "resetComplexity", kComplexities, &complexityLabel,
              &HydraViewportWindow::DisplayOptions::complexity, &ViewportDisplayOverrides::complexity, true);

    // An on/off option: a checkbox.
    auto addSwitch = [&](const QString& text, const char* editorName, const char* resetName,
                         bool HydraViewportWindow::DisplayOptions::*option,
                         std::optional<bool> ViewportDisplayOverrides::*override, bool hasSetting) {
        auto* box = new QCheckBox(text, dialog);
        box->setObjectName(QString::fromLatin1(editorName));
        QToolButton* reset = addRow(row++, QString(), box, resetName);
        if (!hasSetting) reset->setToolTip(builtInTip);
        updates->push_back([panel, box, reset, option, override]() {
            const QSignalBlocker block(box);
            box->setChecked(panel->displayOptions().*option);
            reset->setVisible((panel->displayOverrides().*override).has_value());
        });
        QObject::connect(box, &QCheckBox::toggled, dialog,
                         [edit, override](bool on) { edit([on, override](ViewportDisplayOverrides& o) { o.*override = on; }); });
        QObject::connect(reset, &QToolButton::clicked, dialog,
                         [edit, override]() { edit([override](ViewportDisplayOverrides& o) { (o.*override).reset(); }); });
    };
    using Options = HydraViewportWindow::DisplayOptions;
    using Overrides = ViewportDisplayOverrides;
    addSwitch(QObject::tr("Show guides"), "displayShowGuides", "resetShowGuides", &Options::showGuides,
              &Overrides::showGuides, true);
    addSwitch(QObject::tr("Show proxy"), "displayShowProxy", "resetShowProxy", &Options::showProxy, &Overrides::showProxy,
              true);
    addSwitch(QObject::tr("Show render"), "displayShowRender", "resetShowRender", &Options::showRender,
              &Overrides::showRender, true);
    addSwitch(QObject::tr("Scene materials"), "displaySceneMaterials", "resetSceneMaterials", &Options::sceneMaterials,
              &Overrides::sceneMaterials, false);
    addSwitch(QObject::tr("Cull backfaces"), "displayCullBackfaces", "resetCullBackfaces", &Options::cullBackfaces,
              &Overrides::cullBackfaces, true);
    addSwitch(QObject::tr("Dome light textures"), "displayDomeLightTextures", "resetDomeLightTextures",
              &Options::domeLightTextures, &Overrides::domeLightTextures, false);
    addSwitch(QObject::tr("Scene lights"), "displaySceneLights", "resetSceneLights", &Options::sceneLights,
              &Overrides::sceneLights, true);
    addSwitch(QObject::tr("Camera light"), "displayCameraLight", "resetCameraLight", &Options::cameraLight,
              &Overrides::cameraLight, true);
    addSwitch(QObject::tr("Dome light"), "displayDomeLight", "resetDomeLight", &Options::domeLight, &Overrides::domeLight,
              true);
    // The HUD (docs/VIEWPORT_HUD.md).
    addSwitch(QObject::tr("Show HUD"), "displayHud", "resetHud", &Options::hud, &Overrides::hud, true);
    addSwitch(QObject::tr("HUD: camera and renderer"), "displayHudCamera", "resetHudCamera", &Options::hudCamera,
              &Overrides::hudCamera, true);
    addSwitch(QObject::tr("HUD: axes"), "displayHudAxes", "resetHudAxes", &Options::hudAxes, &Overrides::hudAxes, true);
    addSwitch(QObject::tr("HUD: scene counts"), "displayHudStats", "resetHudStats", &Options::hudStats,
              &Overrides::hudStats, true);
    addSwitch(QObject::tr("HUD: performance"), "displayHudPerformance", "resetHudPerformance", &Options::hudPerformance,
              &Overrides::hudPerformance, true);
    addSwitch(QObject::tr("HUD: tool readout"), "displayHudToolReadout", "resetHudToolReadout", &Options::hudToolReadout,
              &Overrides::hudToolReadout, true);
    addSwitch(QObject::tr("HUD: prim under cursor"), "displayHudRollover", "resetHudRollover", &Options::hudRollover,
              &Overrides::hudRollover, true);

    auto updateAll = [updates]() {
        for (const std::function<void()>& update : *updates) update();
    };
    updateAll();
    QObject::connect(panel, &ViewportViewPanel::displayChanged, dialog, updateAll);
    return dialog;
}

QJsonValue jsonOf(const usdcc::core::SettingValue& value) {
    return std::visit(
        [](const auto& v) -> QJsonValue {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return QString::fromStdString(v);
            } else {
                return QJsonValue(v);
            }
        },
        value);
}

// A session's value: as the delegate's type when it has the setting, else as the JSON's.
std::optional<PXR_NS::VtValue> rendererValueFromJson(const PXR_NS::TfToken& pluginId, const PXR_NS::TfToken& key,
                                                     const QJsonValue& json) {
    usdcc::core::SettingValue value;
    if (json.isBool()) {
        value = json.toBool();
    } else if (json.isDouble()) {
        value = json.toDouble();
    } else if (json.isString()) {
        value = json.toString().toStdString();
    } else {
        return std::nullopt;
    }
    for (const RendererSettingInfo& setting : rendererSettings(pluginId)) {
        if (setting.key == key) return toRendererValue(value, setting.defaultValue);
    }
    // A renderer (or setting) this installation hasn't: kept as it is for a session saved later.
    return std::visit([](const auto& v) { return PXR_NS::VtValue(v); }, value);
}

// The Renderer Settings dialog: the current renderer's settings, each with a Default button
// that shows while this viewport overrides it and drops the override. Edits apply at once.
// Built again when the viewport switches renderer.
QDialog* makeRendererSettingsDialog(ViewportViewPanel* panel) {
    auto* dialog = new QDialog(panel);
    dialog->setObjectName(QStringLiteral("viewportRendererSettingsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    struct State {
        PXR_NS::TfToken pluginId;
        std::vector<std::function<void()>> updates;  // a row each: shows the value, and its Default button
    };
    auto state = std::make_shared<State>();
    auto build = [panel, dialog, scroll, state]() {
        HydraViewportWindow* viewport = panel->hydraWindow();
        const PXR_NS::TfToken pluginId = viewport->currentRendererPlugin();
        state->pluginId = pluginId;
        state->updates.clear();
        const QString name = pluginId.IsEmpty() ? QString() : viewport->rendererDisplayName(pluginId);
        dialog->setWindowTitle(QObject::tr("Renderer Settings - %1 - %2")
                                   .arg(name.isEmpty() ? QString::fromStdString(pluginId.GetString()) : name, panel->windowTitle()));
        auto* content = new QWidget;
        auto* grid = new QGridLayout(content);
        grid->setColumnStretch(1, 1);
        const std::vector<RendererSettingInfo> settings = pluginId.IsEmpty() ? std::vector<RendererSettingInfo>{}
                                                                             : rendererSettings(pluginId);
        int row = 0;
        if (pluginId.IsEmpty()) {
            grid->addWidget(new QLabel(QObject::tr("The viewport has no renderer until it has drawn."), content), row++, 0, 1, 3);
        } else if (settings.empty()) {
            grid->addWidget(new QLabel(QObject::tr("%1 has no settings of its own.").arg(name), content), row++, 0, 1, 3);
        }
        for (const RendererSettingInfo& setting : settings) {
            const PXR_NS::TfToken key = setting.key;
            const PXR_NS::VtValue like = setting.defaultValue;
            const auto set = [panel, pluginId, key, like](const usdcc::core::SettingValue& value) {
                if (const std::optional<PXR_NS::VtValue> converted = toRendererValue(value, like)) {
                    panel->setRendererOverride(pluginId, key, converted);
                }
            };
            const QString objectName = QStringLiteral("rendererSetting_%1").arg(QString::fromStdString(key.GetString()));
            QWidget* editor = nullptr;
            std::function<void(const usdcc::core::SettingValue&)> show;
            switch (setting.type) {
                case usdcc::core::SettingType::Bool: {
                    auto* box = new QCheckBox(QString::fromStdString(setting.label), content);
                    QObject::connect(box, &QCheckBox::toggled, box, [set](bool on) { set(on); });
                    show = [box](const usdcc::core::SettingValue& v) {
                        const QSignalBlocker block(box);
                        if (const bool* on = std::get_if<bool>(&v)) box->setChecked(*on);
                    };
                    editor = box;
                    break;
                }
                case usdcc::core::SettingType::Int: {
                    auto* spin = new QSpinBox(content);
                    spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
                    spin->setKeyboardTracking(false);  // a value per edit, not per key
                    QObject::connect(spin, &QSpinBox::valueChanged, spin, [set](int v) { set(v); });
                    show = [spin](const usdcc::core::SettingValue& v) {
                        const QSignalBlocker block(spin);
                        if (const int* number = std::get_if<int>(&v)) spin->setValue(*number);
                    };
                    editor = spin;
                    break;
                }
                case usdcc::core::SettingType::Float: {
                    auto* spin = new QDoubleSpinBox(content);
                    spin->setRange(-1e9, 1e9);
                    spin->setDecimals(4);
                    spin->setKeyboardTracking(false);
                    QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [set](double v) { set(v); });
                    show = [spin](const usdcc::core::SettingValue& v) {
                        const QSignalBlocker block(spin);
                        if (const double* number = std::get_if<double>(&v)) spin->setValue(*number);
                    };
                    editor = spin;
                    break;
                }
                default: {
                    auto* edit = new QLineEdit(content);
                    QObject::connect(edit, &QLineEdit::editingFinished, edit, [set, edit]() { set(edit->text().toStdString()); });
                    show = [edit](const usdcc::core::SettingValue& v) {
                        const QSignalBlocker block(edit);
                        if (const std::string* text = std::get_if<std::string>(&v)) edit->setText(QString::fromStdString(*text));
                    };
                    editor = edit;
                    break;
                }
            }
            editor->setObjectName(objectName);
            editor->setToolTip(QString::fromStdString(key.GetString()));
            if (setting.type != usdcc::core::SettingType::Bool) {
                grid->addWidget(new QLabel(QString::fromStdString(setting.label), content), row, 0);
            }
            grid->addWidget(editor, row, setting.type == usdcc::core::SettingType::Bool ? 0 : 1, 1,
                            setting.type == usdcc::core::SettingType::Bool ? 2 : 1);
            auto* reset = new QToolButton(content);
            reset->setObjectName(objectName + QStringLiteral("_default"));
            reset->setText(QObject::tr("Default"));
            reset->setToolTip(QObject::tr("Use the Settings > Renderers value"));
            QObject::connect(reset, &QToolButton::clicked, reset,
                             [panel, pluginId, key]() { panel->setRendererOverride(pluginId, key, std::nullopt); });
            grid->addWidget(reset, row++, 2);
            state->updates.push_back([panel, pluginId, key, show, reset]() {
                const RendererSettingValues values = panel->rendererSettingValues(pluginId);
                if (auto found = values.find(key); found != values.end()) {
                    if (const std::optional<usdcc::core::SettingValue> value = toSettingValue(found->second)) show(*value);
                }
                const auto overrides = panel->rendererOverrides().find(pluginId);
                reset->setVisible(overrides != panel->rendererOverrides().end() && overrides->second.contains(key));
            });
        }
        grid->setRowStretch(row, 1);
        scroll->setWidget(content);  // deletes the last renderer's
        for (const auto& update : state->updates) update();
    };
    build();
    QObject::connect(panel, &ViewportViewPanel::rendererSettingsChanged, dialog, [panel, state, build]() {
        if (panel->hydraWindow()->currentRendererPlugin() != state->pluginId) {
            build();
            return;
        }
        for (const auto& update : state->updates) update();
    });
    dialog->resize(dialog->sizeHint().width() + 80, std::min(dialog->sizeHint().height() + 40, 640));
    return dialog;
}
}  // namespace

ViewportViewPanel* ViewportViewPanel::activeViewport() {
    if (s_clickedViewport) return s_clickedViewport;
    return viewportList().empty() ? nullptr : viewportList().front();
}

std::vector<ViewportViewPanel*> ViewportViewPanel::openViewports() { return viewportList(); }

ViewportViewPanel* ViewportViewPanel::withId(int id) {
    for (ViewportViewPanel* viewport : viewportList()) {
        if (viewport->m_id == id) return viewport;
    }
    return nullptr;
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
    for (ViewportViewPanel* viewport : viewportList()) {
        if (viewport->currentStage() == stage) return viewport->m_viewport->currentRendererPlugin();
    }
    return {};
}

ViewportViewPanel::~ViewportViewPanel() {
    const bool wasActive = activeViewport() == this;
    std::erase(viewportList(), this);
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
      m_id(++s_lastViewportId),
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
        if (isViewportDisplayKey(key)) applyDisplay();
        if (isRendererSettingKey(key)) applyRendererSettings();
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
    // The delegate's own settings: the first renderer's once there is one, and each new one's.
    connect(m_viewport, &HydraViewportWindow::rendererPluginsChanged, this, &ViewportViewPanel::applyRendererSettings);
    connect(m_viewport, &HydraViewportWindow::rendererChanged, this, &ViewportViewPanel::applyRendererSettings);
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
    // A HUD label's menu, under it (docs/VIEWPORT_HUD.md §3).
    connect(m_viewport, &HydraViewportWindow::hudMenuRequested, this, [this](HudItem item, QPoint globalPos) {
        makeActive();
        QMenu menu(this);
        populateHudMenu(&menu, item);
        if (!menu.isEmpty()) menu.exec(globalPos);
    });
    // The HUD's counts: a slice at a time, a moment after the last change.
    m_countTimer = new QTimer(this);
    m_countTimer->setSingleShot(true);
    connect(m_countTimer, &QTimer::timeout, this, &ViewportViewPanel::runCounts);
    viewportList().push_back(this);
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
    m_countsOn = countsWanted();
    recountStage(true);
    recountSelection(true);
    if (!stage) {
        m_stageChangedConnection = {};
        m_targetConnection = {};
        m_editTargetConnection = {};
        m_viewport->setTargetXformOp(PXR_NS::TfToken());
        m_viewport->requestUpdate();
        return;
    }
    // The viewport only draws when asked: redraw after every edit, wherever it
    // came from (undo/redo, another panel, a script) — requestUpdate() folds a
    // burst of notices into one frame.
    m_stageChangedConnection = stage->changed.connect([this](const StageChanges& changes) {
        m_viewport->requestUpdate();
        // The HUD's counts change with what's there, and with what decides whether it's
        // drawn or how big it is; not with a transform or a color.
        static const PXR_NS::TfToken counted[] = {
            PXR_NS::TfToken("points"),     PXR_NS::TfToken("faceVertexCounts"), PXR_NS::TfToken("visibility"),
            PXR_NS::TfToken("purpose"),    PXR_NS::TfToken("protoIndices"),     PXR_NS::TfToken("prototypes")};
        bool recount = !changes.resyncedPaths.empty();
        for (const PXR_NS::SdfPath& path : changes.changedInfoOnlyPaths) {
            if (recount) break;
            recount = path.IsPropertyPath() && std::find(std::begin(counted), std::end(counted), path.GetNameToken()) != std::end(counted);
        }
        if (recount) {
            recountStage(false);
            recountSelection(false);
        }
    });
    m_targetConnection = stage->targetXformOpChanged.connect([this]() { syncTarget(); });
    // The HUD names the edit target when it isn't the root layer.
    m_editTargetConnection = stage->editTargetChanged.connect([this]() { m_viewport->refreshHud(); });
    syncTarget();
}

bool ViewportViewPanel::countsWanted() const {
    const HydraViewportWindow::DisplayOptions shown = displayOptions();
    return shown.hud && shown.hudStats && currentStage();
}

void ViewportViewPanel::recountStage(bool reset) {
    m_stageCountJob.reset();
    if (reset || !countsWanted()) m_counts.stage.reset();
    if (countsWanted()) {
        m_countedPurposes = viewportPurposes(displayOptions());
        m_stageCountJob = std::make_unique<SceneCountJob>(currentStage()->usdStage(), std::vector<PXR_NS::SdfPath>{},
                                                          m_countedPurposes);
        m_stageCountSlices = 0;
        m_countTimer->start(100);  // a moment after the last change
    }
    publishCounts();
}

void ViewportViewPanel::recountSelection(bool reset) {
    m_selectionCountJob.reset();
    const std::vector<PXR_NS::SdfPath> selection =
        currentStage() ? stageManager()->selectedPaths(currentStage()) : std::vector<PXR_NS::SdfPath>{};
    if (reset || selection.empty() || !countsWanted()) m_counts.selection.reset();
    if (countsWanted() && !selection.empty()) {
        m_countedPurposes = viewportPurposes(displayOptions());
        m_selectionCountJob = std::make_unique<SceneCountJob>(currentStage()->usdStage(), countRoots(selection),
                                                              m_countedPurposes);
        m_selectionCountSlices = 0;
        // Debounced, so drag-selecting doesn't count every step of the way.
        m_countTimer->start(100);
    }
    publishCounts();
}

void ViewportViewPanel::runCounts() {
    // About half a frame each time round, then the UI gets its turn.
    QElapsedTimer slice;
    slice.start();
    while (slice.elapsed() < 8) {
        // The selection first: it's what the user is looking at, and usually small.
        if (m_selectionCountJob) {
            if (m_selectionCountJob->step(256)) {
                m_counts.selection = m_selectionCountJob->counts();
                m_selectionCountJob.reset();
            }
        } else if (m_stageCountJob) {
            if (m_stageCountJob->step(256)) {
                m_counts.stage = m_stageCountJob->counts();
                m_stageCountJob.reset();
            }
        } else {
            break;
        }
    }
    // Still under way after a slice: the old numbers aren't right any more.
    if (m_selectionCountJob && ++m_selectionCountSlices > 1) m_counts.selection.reset();
    if (m_stageCountJob && ++m_stageCountSlices > 1) m_counts.stage.reset();
    publishCounts();
    if (m_selectionCountJob || m_stageCountJob) m_countTimer->start(0);
}

void ViewportViewPanel::publishCounts() { m_viewport->setHudCounts(m_counts); }

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
        recountSelection(false);
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
    // The counts: wanted now and not before, or the purposes they count changed.
    const bool wanted = countsWanted();
    if (wanted != m_countsOn || (wanted && viewportPurposes(displayOptions()) != m_countedPurposes)) {
        m_countsOn = wanted;
        recountStage(true);
        recountSelection(true);
    }
    Q_EMIT displayChanged();
    notifyOptionsChanged();  // Show Grid's tick, Use Settings Defaults
}

void ViewportViewPanel::editOverrides(const std::function<void(ViewportDisplayOverrides&)>& change) {
    ViewportDisplayOverrides overrides = m_displayOverrides;
    change(overrides);
    setDisplayOverrides(overrides);
}

QDialog* ViewportViewPanel::showDisplayDialog() {
    if (!m_displayDialog) m_displayDialog = makeDisplayDialog(this);
    m_displayDialog->show();
    m_displayDialog->raise();
    m_displayDialog->activateWindow();
    return m_displayDialog;
}

void ViewportViewPanel::setRendererOverrides(const RendererOverrides& overrides) {
    RendererOverrides kept = overrides;
    std::erase_if(kept, [](const auto& entry) { return entry.second.empty(); });
    if (kept == m_rendererOverrides) return;
    m_rendererOverrides = std::move(kept);
    applyRendererSettings();
}

void ViewportViewPanel::setRendererOverride(const PXR_NS::TfToken& pluginId, const PXR_NS::TfToken& key,
                                            const std::optional<PXR_NS::VtValue>& value) {
    RendererOverrides overrides = m_rendererOverrides;
    if (value) {
        overrides[pluginId][key] = *value;
    } else if (auto found = overrides.find(pluginId); found != overrides.end()) {
        found->second.erase(key);
    }
    setRendererOverrides(overrides);
}

RendererSettingValues ViewportViewPanel::rendererSettingValues(const PXR_NS::TfToken& pluginId) const {
    RendererSettingValues values = rendererSettingDefaults(pluginId);
    if (auto found = m_rendererOverrides.find(pluginId); found != m_rendererOverrides.end()) {
        for (const auto& [key, value] : found->second) {
            // Not a setting the delegate has (here): kept, not handed to it.
            if (values.contains(key)) values[key] = value;
        }
    }
    return values;
}

void ViewportViewPanel::applyRendererSettings() {
    const PXR_NS::TfToken pluginId = m_viewport->currentRendererPlugin();
    RendererState state{pluginId, pluginId.IsEmpty() ? RendererSettingValues{} : rendererSettingValues(pluginId),
                        m_rendererOverrides};
    if (!pluginId.IsEmpty()) m_viewport->setRendererSettings(pluginId, state.values);
    Q_EMIT rendererSettingsChanged();
    if (state == m_toldRendererState) return;
    m_toldRendererState = std::move(state);
    anyRendererSettingsChanged()(this);
}

void ViewportViewPanel::setRenderer(const PXR_NS::TfToken& pluginId) {
    m_viewport->setRendererPlugin(pluginId);  // with an engine, rendererChanged() applies them already
    applyRendererSettings();
}

usdcc::core::Signal<ViewportViewPanel*>& ViewportViewPanel::anyRendererSettingsChanged() {
    static usdcc::core::Signal<ViewportViewPanel*> signal;
    return signal;
}

QDialog* ViewportViewPanel::showRendererSettingsDialog() {
    if (!m_rendererSettingsDialog) m_rendererSettingsDialog = makeRendererSettingsDialog(this);
    m_rendererSettingsDialog->show();
    m_rendererSettingsDialog->raise();
    m_rendererSettingsDialog->activateWindow();
    return m_rendererSettingsDialog;
}

usdcc::ui::ViewPanel* ViewportViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new ViewportViewPanel(stageManager(), currentStage(), m_toolSet, m_undoManager, windowTitle(), parent);
    copy->setDisplayOverrides(m_displayOverrides);
    copy->setRendererOverrides(m_rendererOverrides);

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
    // {"HdEmbreeRendererPlugin": {"ty:maxBounces": 8}}: only what's overridden.
    QJsonObject renderers;
    for (const auto& [pluginId, values] : m_rendererOverrides) {
        QJsonObject settings;
        for (const auto& [key, value] : values) {
            if (const std::optional<usdcc::core::SettingValue> setting = toSettingValue(value)) {
                settings.insert(QString::fromStdString(key.GetString()), jsonOf(*setting));
            }
        }
        if (!settings.isEmpty()) renderers.insert(QString::fromStdString(pluginId.GetString()), settings);
    }
    if (!renderers.isEmpty()) state.insert(QStringLiteral("rendererSettings"), renderers);
    return state;
}

void ViewportViewPanel::restoreSessionState(const QJsonObject& state) {
    // Needs no renderer, so applies at once. None saved (an older session): no overrides.
    setDisplayOverrides(ViewportDisplayOverrides::fromJson(state.value(QStringLiteral("display")).toObject()));
    // Before the renderer below: it gets them as it's switched to.
    RendererOverrides renderers;
    const QJsonObject savedRenderers = state.value(QStringLiteral("rendererSettings")).toObject();
    for (auto plugin = savedRenderers.begin(); plugin != savedRenderers.end(); ++plugin) {
        const PXR_NS::TfToken pluginId(plugin.key().toStdString());
        const QJsonObject settings = plugin.value().toObject();
        for (auto setting = settings.begin(); setting != settings.end(); ++setting) {
            const PXR_NS::TfToken key(setting.key().toStdString());
            if (const std::optional<PXR_NS::VtValue> value = rendererValueFromJson(pluginId, key, setting.value())) {
                renderers[pluginId][key] = *value;
            }
        }
    }
    setRendererOverrides(renderers);
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
    const std::optional<PXR_NS::GfRange3d> bounds = sceneBounds(stage->usdStage(), selection, PXR_NS::UsdTimeCode::Default(), viewportPurposes(displayOptions()));
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

QAction* ViewportViewPanel::addToggleHudAction(QWidget* window) {
    usdcc::core::Settings::instance().define({
        .key = kToggleHudShortcutKey,
        .category = "Shortcuts",
        .group = "Viewport",
        .label = "Toggle HUD",
        .description = "Show or hide the heads-up display in the viewport you worked in last.",
        .type = usdcc::core::SettingType::Shortcut,
        .defaultValue = std::string(),
    });
    auto* action = new QAction(tr("Toggle HUD"), window);
    action->setObjectName(QStringLiteral("toggleHudAction"));
    action->setShortcutContext(Qt::ApplicationShortcut);  // as Fit to View
    usdcc::ui::bindShortcut(action, kToggleHudShortcutKey);
    window->addAction(action);
    connect(action, &QAction::triggered, window, []() {
        if (ViewportViewPanel* viewport = activeViewport()) {
            const bool on = viewport->displayOptions().hud;
            viewport->editOverrides([on](ViewportDisplayOverrides& o) { o.hud = !on; });
        }
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

void ViewportViewPanel::addRendererEntries(QMenu* menu) {
    // Empty until the viewport has been shown once: its GL context, and with
    // it the Hydra engine that lists the plugins, is created on first expose.
    const std::vector<PXR_NS::TfToken> plugins = m_viewport->rendererPlugins();
    menu->setEnabled(!plugins.empty());
    auto* group = new QActionGroup(menu);  // exclusive: one renderer checked
    const PXR_NS::TfToken current = m_viewport->currentRendererPlugin();
    for (const PXR_NS::TfToken& pluginId : plugins) {
        QAction* action = menu->addAction(m_viewport->rendererDisplayName(pluginId));
        action->setCheckable(true);
        action->setChecked(pluginId == current);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, pluginId]() { m_viewport->setRendererPlugin(pluginId); });
    }
    if (plugins.empty()) return;

    // The current renderer's own settings, and its commands.
    menu->addSeparator();
    QAction* settings = menu->addAction(tr("Renderer Settings..."), this, [this]() { showRendererSettingsDialog(); });
    settings->setObjectName(QStringLiteral("rendererSettingsAction"));
    const PXR_NS::HdCommandDescriptors commands = m_viewport->rendererCommands();
    if (commands.empty()) return;
    menu->addSeparator();
    for (const PXR_NS::HdCommandDescriptor& command : commands) {
        QString text = QString::fromStdString(command.commandDescription.empty() ? command.commandName.GetString()
                                                                                 : command.commandDescription);
        QAction* action = menu->addAction(text.replace(QLatin1Char('&'), QStringLiteral("&&")));
        action->setObjectName(QStringLiteral("rendererCommand_%1").arg(QString::fromStdString(command.commandName.GetString())));
        const PXR_NS::TfToken name = command.commandName;
        connect(action, &QAction::triggered, this, [this, name]() { m_viewport->runRendererCommand(name); });
    }
}

void ViewportViewPanel::addShadingEntries(QMenu* menu) {
    // Only Storm draws the modes; for any other renderer they are greyed.
    const bool drawsShading = HydraViewportWindow::rendererDrawsShadingModes(m_viewport->currentRendererPlugin());
    const ShadingMode current = displayOptions().shading;
    auto* shadings = new QActionGroup(menu);
    for (ShadingMode shading : kShadingModes) {
        QAction* action = menu->addAction(tr(qPrintable(shadingModeLabel(shading))));
        action->setObjectName(QStringLiteral("shading_%1").arg(QLatin1String(shadingModeName(shading))));
        action->setCheckable(true);
        action->setChecked(shading == current);
        action->setEnabled(drawsShading);
        shadings->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, shading]() { editOverrides([shading](ViewportDisplayOverrides& o) { o.shading = shading; }); });
    }
}

void ViewportViewPanel::populateHudMenu(QMenu* menu, HudItem item) {
    switch (item) {
        case HudItem::Camera: {
            // Only the orbit camera so far; the stage's cameras will be listed here.
            QAction* persp = menu->addAction(tr("Persp"));
            persp->setObjectName(QStringLiteral("hudCameraPersp"));
            persp->setCheckable(true);
            persp->setChecked(true);
            menu->addSeparator();
            const QKeySequence key = usdcc::ui::settingShortcut(kFitToViewShortcutKey);
            const QString fitText = key.isEmpty() ? tr("Fit to View")
                                                  : tr("Fit to View") + QStringLiteral("\t") + key.toString(QKeySequence::NativeText);
            menu->addAction(fitText, this, [this]() { fitToView(); });
            menu->addAction(tr("Reset Camera"), this, [this]() { m_viewport->setCameraState({}); });
            break;
        }
        case HudItem::Renderer:
            addRendererEntries(menu);
            break;
        case HudItem::Shading: {
            addShadingEntries(menu);
            if (!HydraViewportWindow::rendererDrawsShadingModes(m_viewport->currentRendererPlugin())) {
                menu->addSeparator();
                menu->addAction(tr("Only Storm draws the shading modes"))->setEnabled(false);
            }
            break;
        }
        case HudItem::EditTarget: {
            // The stage's layers, as the Layer Editor lists them: picking one makes it the
            // edit target.
            const StageRefPtr stage = currentStage();
            if (!stage) break;
            auto* group = new QActionGroup(menu);
            std::function<void(const std::vector<LayerEntry>&, int)> addLayers = [&](const std::vector<LayerEntry>& entries,
                                                                                  int depth) {
                for (const LayerEntry& entry : entries) {
                    QString text = QString(depth * 4, QLatin1Char(' ')) + QString::fromStdString(entry.displayName);
                    if (entry.session) text += tr(" (session)");
                    QAction* action = menu->addAction(text);
                    action->setCheckable(true);
                    action->setChecked(entry.editTarget);
                    action->setEnabled(entry.layer && !entry.muted && !entry.missing);
                    group->addAction(action);
                    const PXR_NS::SdfLayerHandle layer = entry.layer;
                    connect(action, &QAction::triggered, this, [stage, layer]() { stage->setEditTargetLayer(layer); });
                    addLayers(entry.children, depth + 1);
                }
            };
            addLayers(layerTree(stage->usdStage()), 0);
            break;
        }
        default:
            break;
    }
}

void ViewportViewPanel::populateOptionsMenu(QMenu* menu) {
    QMenu* renderers = menu->addMenu(tr("Renderer"));
    addRendererEntries(renderers);

    menu->addSeparator();
    const HydraViewportWindow::DisplayOptions shown = displayOptions();
    // Changes one override, leaving the others as they are.
    auto edit = [this](auto change) {
        ViewportDisplayOverrides overrides = m_displayOverrides;
        change(overrides);
        setDisplayOverrides(overrides);
    };

    // Shading: only Storm draws the modes; for any other renderer the submenu says so.
    QMenu* shadingMenu = menu->addMenu(tr("Shading"));
    shadingMenu->setObjectName(QStringLiteral("shadingMenu"));
    const bool drawsShading = HydraViewportWindow::rendererDrawsShadingModes(m_viewport->currentRendererPlugin());
    shadingMenu->setEnabled(drawsShading);
    shadingMenu->menuAction()->setToolTip(drawsShading ? QString() : tr("Only Storm draws the shading modes"));
    addShadingEntries(shadingMenu);

    QMenu* complexityMenu = menu->addMenu(tr("Complexity"));
    complexityMenu->setObjectName(QStringLiteral("complexityMenu"));
    auto* complexities = new QActionGroup(complexityMenu);
    for (Complexity complexity : kComplexities) {
        QAction* action = complexityMenu->addAction(tr(qPrintable(complexityLabel(complexity))));
        action->setObjectName(QStringLiteral("complexity_%1").arg(QLatin1String(complexityName(complexity))));
        action->setCheckable(true);
        action->setChecked(complexity == shown.complexity);
        complexities->addAction(action);
        connect(action, &QAction::triggered, this,
                [edit, complexity]() { edit([complexity](ViewportDisplayOverrides& o) { o.complexity = complexity; }); });
    }

    // A checkable entry for one on/off option.
    auto addSwitch = [this, edit, &shown](QMenu* to, const QString& text, const char* name,
                                          bool HydraViewportWindow::DisplayOptions::*option,
                                          std::optional<bool> ViewportDisplayOverrides::*override) {
        QAction* action = to->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setCheckable(true);
        action->setChecked(shown.*option);
        connect(action, &QAction::triggered, this,
                [edit, override](bool on) { edit([on, override](ViewportDisplayOverrides& o) { o.*override = on; }); });
        return action;
    };
    using Options = HydraViewportWindow::DisplayOptions;
    using Overrides = ViewportDisplayOverrides;
    QMenu* purposes = menu->addMenu(tr("Purposes"));
    purposes->setObjectName(QStringLiteral("purposesMenu"));
    addSwitch(purposes, tr("Guide"), "showGuidesAction", &Options::showGuides, &Overrides::showGuides);
    addSwitch(purposes, tr("Proxy"), "showProxyAction", &Options::showProxy, &Overrides::showProxy);
    addSwitch(purposes, tr("Render"), "showRenderAction", &Options::showRender, &Overrides::showRender);
    QMenu* lights = menu->addMenu(tr("Lights"));
    lights->setObjectName(QStringLiteral("lightsMenu"));
    addSwitch(lights, tr("Scene Lights"), "sceneLightsAction", &Options::sceneLights, &Overrides::sceneLights);
    QAction* cameraLight =
        addSwitch(lights, tr("Camera Light"), "cameraLightAction", &Options::cameraLight, &Overrides::cameraLight);
    cameraLight->setToolTip(tr("Storm only"));
    addSwitch(lights, tr("Dome Light"), "domeLightAction", &Options::domeLight, &Overrides::domeLight);
    addSwitch(menu, tr("Scene Materials"), "sceneMaterialsAction", &Options::sceneMaterials, &Overrides::sceneMaterials);
    addSwitch(menu, tr("Cull Backfaces"), "cullBackfacesAction", &Options::cullBackfaces, &Overrides::cullBackfaces);
    addSwitch(menu, tr("Dome Light Textures"), "domeLightTexturesAction", &Options::domeLightTextures,
              &Overrides::domeLightTextures);

    menu->addSeparator();
    QAction* showGrid = menu->addAction(tr("Show Grid"));
    showGrid->setObjectName(QStringLiteral("showGridAction"));
    showGrid->setCheckable(true);
    showGrid->setChecked(shown.showGrid);
    connect(showGrid, &QAction::triggered, this, [this](bool on) {
        ViewportDisplayOverrides overrides = m_displayOverrides;
        overrides.showGrid = on;
        setDisplayOverrides(overrides);
    });
    QMenu* selection = menu->addMenu(tr("Selection"));
    selection->setObjectName(QStringLiteral("selectionStyleMenu"));
    auto* styles = new QActionGroup(selection);  // exclusive: one style checked
    const SelectionStyle currentStyle = shown.selectionStyle;
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
    // The heads-up display and its parts (docs/VIEWPORT_HUD.md §5).
    QMenu* hud = menu->addMenu(tr("HUD"));
    hud->setObjectName(QStringLiteral("hudMenu"));
    addSwitch(hud, tr("Show HUD"), "hudAction", &Options::hud, &Overrides::hud);
    hud->addSeparator();
    const struct {
        const char* text;
        const char* name;
        bool Options::*option;
        std::optional<bool> Overrides::*override;
    } hudParts[] = {
        {"Camera and Renderer", "hudCameraAction", &Options::hudCamera, &Overrides::hudCamera},
        {"Axes", "hudAxesAction", &Options::hudAxes, &Overrides::hudAxes},
        {"Scene Counts", "hudStatsAction", &Options::hudStats, &Overrides::hudStats},
        {"Performance", "hudPerformanceAction", &Options::hudPerformance, &Overrides::hudPerformance},
        {"Tool Readout", "hudToolReadoutAction", &Options::hudToolReadout, &Overrides::hudToolReadout},
        {"Prim Under Cursor", "hudRolloverAction", &Options::hudRollover, &Overrides::hudRollover},
    };
    for (const auto& part : hudParts) {
        addSwitch(hud, tr(part.text), part.name, part.option, part.override)->setEnabled(shown.hud);
    }

    QAction* dialog = menu->addAction(tr("Display..."), this, [this]() { showDisplayDialog(); });
    dialog->setObjectName(QStringLiteral("displayDialogAction"));
    QAction* defaults = menu->addAction(tr("Use Settings Defaults"), this, [this]() { setDisplayOverrides({}); });
    defaults->setObjectName(QStringLiteral("displayDefaultsAction"));
    defaults->setToolTip(tr("Drop all of this viewport's own display values"));
    defaults->setEnabled(!m_displayOverrides.isEmpty());
}

}  // namespace usdcc::usd
