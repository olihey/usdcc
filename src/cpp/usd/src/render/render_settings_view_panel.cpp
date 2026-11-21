#include "usdcc/usd/render/render_settings_view_panel.h"

#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_menu.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_range.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/var.h>

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::render {

namespace {

constexpr int kRefreshDelayMs = 100;

QString text(const std::string& s) { return QString::fromStdString(s); }

double asDouble(const VtValue& value, double fallback = 0.0) {
    if (value.IsHolding<double>()) return value.UncheckedGet<double>();
    const VtValue cast = VtValue::Cast<double>(value);
    return cast.IsHolding<double>() ? cast.UncheckedGet<double>() : fallback;
}

std::string asString(const VtValue& value) {
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    const VtValue cast = VtValue::Cast<std::string>(value);
    return cast.IsHolding<std::string>() ? cast.UncheckedGet<std::string>() : std::string();
}

// The USD type a new RenderVar of a Hydra AOV gets.
TfToken aovDataType(const std::string& source) {
    if (source == "color") return SdfValueTypeNames->Color4f.GetAsToken();
    if (source == "depth" || source.rfind("lpe", 0) == 0) return SdfValueTypeNames->Float.GetAsToken();
    if (source == "normal" || source == "Neye") return SdfValueTypeNames->Normal3f.GetAsToken();
    if (source == "primId" || source == "instanceId" || source == "elementId") return SdfValueTypeNames->Int.GetAsToken();
    return SdfValueTypeNames->Color3f.GetAsToken();
}

}  // namespace

RenderSettingsViewPanel::RenderSettingsViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage,
                                                 std::shared_ptr<usdcc::usd::UndoManager> undoManager, const QString& title,
                                                 QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undo(std::move(undoManager)) {
    defineFinalRenderSettings();
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* toolbar = new QHBoxLayout();
    addPanelControls(toolbar);
    m_settingsCombo = new QComboBox(content);
    m_settingsCombo->setObjectName(QStringLiteral("renderSettingsPrim"));
    m_settingsCombo->setToolTip(tr("The stage's RenderSettings prim these settings are in"));
    m_settingsCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    toolbar->addWidget(m_settingsCombo, 1);
    m_createButton = new QPushButton(tr("New"), content);
    m_createButton->setObjectName(QStringLiteral("renderSettingsCreate"));
    m_createButton->setToolTip(tr("Make a RenderSettings prim, with a product and its color AOV, under /Render"));
    toolbar->addWidget(m_createButton);
    layout->addLayout(toolbar);

    auto* rendererRow = new QHBoxLayout();
    rendererRow->setContentsMargins(6, 0, 6, 0);
    rendererRow->addWidget(new QLabel(tr("Renderer"), content));
    m_rendererCombo = new QComboBox(content);
    m_rendererCombo->setObjectName(QStringLiteral("renderSettingsRenderer"));
    m_rendererCombo->setToolTip(tr("What renders these settings (usdcc:render:renderer)"));
    rendererRow->addWidget(m_rendererCombo, 1);
    layout->addLayout(rendererRow);
    m_rendererProblem = new QLabel(content);
    m_rendererProblem->setWordWrap(true);
    m_rendererProblem->setContentsMargins(6, 0, 6, 0);
    m_rendererProblem->setStyleSheet(QStringLiteral("color: #e0a040;"));
    m_rendererProblem->hide();
    layout->addWidget(m_rendererProblem);

    m_stack = new QStackedWidget(content);
    // Page 0: no RenderSettings prim.
    auto* empty = new QWidget(m_stack);
    auto* emptyLayout = new QVBoxLayout(empty);
    auto* emptyLabel = new QLabel(tr("This stage has no RenderSettings prim. Renders use Settings > Final Render's "
                                     "defaults and the active viewport's camera.\n\nMake one to keep this stage's "
                                     "settings in the stage."),
                                  empty);
    emptyLabel->setWordWrap(true);
    emptyLayout->addWidget(emptyLabel);
    auto* emptyCreate = new QPushButton(tr("Create Render Settings"), empty);
    emptyCreate->setObjectName(QStringLiteral("renderSettingsCreateEmpty"));
    emptyLayout->addWidget(emptyCreate, 0, Qt::AlignLeft);
    emptyLayout->addStretch(1);
    m_stack->addWidget(empty);

    // Page 1: the tabs.
    m_tabs = new QTabWidget(m_stack);
    m_tabs->setObjectName(QStringLiteral("renderSettingsTabs"));
    m_stack->addWidget(m_tabs);

    // Image.
    {
        auto* page = new QWidget(m_tabs);
        auto* form = new QFormLayout(page);
        m_cameraCombo = new QComboBox(page);
        m_cameraCombo->setObjectName(QStringLiteral("renderSettingsCamera"));
        m_cameraCombo->setToolTip(tr("The camera rendered through; none renders through the active viewport's"));
        form->addRow(tr("Camera"), m_cameraCombo);
        auto* size = new QHBoxLayout();
        m_width = new QSpinBox(page);
        m_width->setObjectName(QStringLiteral("renderSettingsWidth"));
        m_width->setRange(1, 16384);
        m_height = new QSpinBox(page);
        m_height->setObjectName(QStringLiteral("renderSettingsHeight"));
        m_height->setRange(1, 16384);
        size->addWidget(m_width);
        size->addWidget(new QLabel(QStringLiteral("×"), page));
        size->addWidget(m_height);
        auto* presets = new QToolButton(page);
        presets->setText(QStringLiteral("…"));
        presets->setToolTip(tr("Common sizes"));
        presets->setPopupMode(QToolButton::InstantPopup);
        auto* presetMenu = new QMenu(presets);
        for (const auto& [w, h] : std::vector<std::pair<int, int>>{{1280, 720}, {1920, 1080}, {2048, 858}, {2048, 1080}, {3840, 2160}, {1080, 1080}, {1080, 1920}}) {
            presetMenu->addAction(QStringLiteral("%1 × %2").arg(w).arg(h), this, [this, w, h]() {
                edit(tr("Set Resolution"), [this, w, h]() {
                    const UsdPrim prim = imagePrim("resolution");
                    return prim && prim.CreateAttribute(TfToken("resolution"), SdfValueTypeNames->Int2, false).Set(GfVec2i(w, h));
                });
            });
        }
        presets->setMenu(presetMenu);
        size->addWidget(presets);
        size->addStretch(1);
        form->addRow(tr("Resolution"), size);
        m_pixelAspect = new QDoubleSpinBox(page);
        m_pixelAspect->setRange(0.1, 10.0);
        m_pixelAspect->setDecimals(3);
        m_pixelAspect->setSingleStep(0.1);
        form->addRow(tr("Pixel aspect"), m_pixelAspect);
        m_tabs->addTab(page, tr("Image"));
    }
    // Output.
    {
        auto* page = new QWidget(m_tabs);
        auto* form = new QFormLayout(page);
        m_stageRange = new QCheckBox(tr("The stage's"), page);
        m_stageRange->setObjectName(QStringLiteral("renderSettingsStageRange"));
        form->addRow(tr("Frame range"), m_stageRange);
        auto* frames = new QHBoxLayout();
        const auto frameBox = [page](const QString& name) {
            auto* box = new QDoubleSpinBox(page);
            box->setObjectName(name);
            box->setRange(-1e7, 1e7);
            box->setDecimals(2);
            return box;
        };
        m_frameStart = frameBox(QStringLiteral("renderSettingsFrameStart"));
        m_frameEnd = frameBox(QStringLiteral("renderSettingsFrameEnd"));
        m_frameStep = frameBox(QStringLiteral("renderSettingsFrameStep"));
        m_frameStep->setRange(0.01, 1e5);
        frames->addWidget(m_frameStart);
        frames->addWidget(new QLabel(tr("to"), page));
        frames->addWidget(m_frameEnd);
        frames->addWidget(new QLabel(tr("by"), page));
        frames->addWidget(m_frameStep);
        form->addRow(QString(), frames);
        m_output = new QLineEdit(page);
        m_output->setObjectName(QStringLiteral("renderSettingsOutput"));
        m_output->setPlaceholderText(tr("<stage>_<settings>.<frame>"));
        m_output->setToolTip(tr("The file each frame goes to. <stage>, <settings>, <product>, <aov>, <renderer> and "
                                "<frame> (or ####) are filled in; a relative path is in Settings > Final Render's output "
                                "folder. Empty: the product's productName, else <stage>_<settings>."));
        form->addRow(tr("Output"), m_output);
        m_outputExample = new QLabel(page);
        m_outputExample->setObjectName(QStringLiteral("renderSettingsOutputExample"));
        m_outputExample->setWordWrap(true);
        m_outputExample->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(QString(), m_outputExample);
        m_format = new QComboBox(page);
        m_format->setObjectName(QStringLiteral("renderSettingsFormat"));
        m_format->addItem(tr("OpenEXR, linear, AOVs as layers"), QStringLiteral("exr"));
        m_format->addItem(tr("PNG, through the view transform"), QStringLiteral("png"));
        form->addRow(tr("Format"), m_format);
        auto* tiles = new QHBoxLayout();
        m_tilesX = new QSpinBox(page);
        m_tilesX->setObjectName(QStringLiteral("renderSettingsTilesX"));
        m_tilesX->setRange(1, 64);
        m_tilesY = new QSpinBox(page);
        m_tilesY->setObjectName(QStringLiteral("renderSettingsTilesY"));
        m_tilesY->setRange(1, 64);
        tiles->addWidget(m_tilesX);
        tiles->addWidget(new QLabel(QStringLiteral("×"), page));
        tiles->addWidget(m_tilesY);
        tiles->addStretch(1);
        form->addRow(tr("Tiles"), tiles);
        m_timeLimit = new QDoubleSpinBox(page);
        m_timeLimit->setObjectName(QStringLiteral("renderSettingsTimeLimit"));
        m_timeLimit->setRange(0, 86400);
        m_timeLimit->setSuffix(tr(" s"));
        m_timeLimit->setSpecialValueText(tr("Until done"));
        m_timeLimit->setToolTip(tr("How long each frame or tile may refine"));
        form->addRow(tr("Time limit"), m_timeLimit);
        m_tabs->addTab(page, tr("Output"));
    }
    // Renderer.
    {
        auto* scroll = new QScrollArea(m_tabs);
        scroll->setWidgetResizable(true);
        m_rendererPage = new QWidget(scroll);
        m_rendererPage->setObjectName(QStringLiteral("renderSettingsRendererPage"));
        m_rendererForm = new QFormLayout(m_rendererPage);
        scroll->setWidget(m_rendererPage);
        m_tabs->addTab(scroll, tr("Renderer"));
    }
    // AOVs.
    {
        auto* page = new QWidget(m_tabs);
        auto* box = new QVBoxLayout(page);
        m_aovList = new QListWidget(page);
        m_aovList->setObjectName(QStringLiteral("renderSettingsAovs"));
        box->addWidget(m_aovList, 1);
        auto* row = new QHBoxLayout();
        m_aovName = new QComboBox(page);
        m_aovName->setEditable(true);
        m_aovName->addItems({QStringLiteral("color"), QStringLiteral("depth"), QStringLiteral("normal"), QStringLiteral("Neye"),
                             QStringLiteral("primId"), QStringLiteral("instanceId"), QStringLiteral("elementId"),
                             QStringLiteral("primvars:st")});
        m_aovName->setToolTip(tr("The renderer's name for the AOV (the RenderVar's sourceName)"));
        row->addWidget(m_aovName, 1);
        auto* add = new QPushButton(tr("Add"), page);
        auto* remove = new QPushButton(tr("Remove"), page);
        row->addWidget(add);
        row->addWidget(remove);
        box->addLayout(row);
        connect(add, &QPushButton::clicked, this, [this]() {
            const std::string source = m_aovName->currentText().trimmed().toStdString();
            if (source.empty()) return;
            edit(tr("Add AOV"), [this, source]() {
                UsdRenderSettings settings(settingsPrim());
                if (!settings) return false;
                const UsdStagePtr stage = settings.GetPrim().GetStage();
                SdfPathVector products;
                settings.GetProductsRel().GetForwardedTargets(&products);
                UsdRenderProduct product(products.empty() ? UsdPrim() : stage->GetPrimAtPath(products.front()));
                if (!product) {
                    product = UsdRenderProduct::Define(stage, m_settingsPath.AppendChild(TfToken("Product")));
                    settings.CreateProductsRel().SetTargets({product.GetPath()});
                }
                std::string name = TfMakeValidIdentifier(source);
                SdfPath path = product.GetPath().AppendChild(TfToken(name));
                for (int i = 2; stage->GetPrimAtPath(path); ++i) path = product.GetPath().AppendChild(TfToken(name + std::to_string(i)));
                UsdRenderVar var = UsdRenderVar::Define(stage, path);
                var.CreateSourceNameAttr().Set(source);
                var.CreateDataTypeAttr().Set(aovDataType(source));
                return product.CreateOrderedVarsRel().AddTarget(path);
            });
        });
        connect(remove, &QPushButton::clicked, this, [this]() {
            QListWidgetItem* item = m_aovList->currentItem();
            if (!item) return;
            const SdfPath var(item->data(Qt::UserRole).toString().toStdString());
            edit(tr("Remove AOV"), [this, var]() {
                UsdRenderSettings settings(settingsPrim());
                if (!settings) return false;
                const UsdStagePtr stage = settings.GetPrim().GetStage();
                SdfPathVector products;
                settings.GetProductsRel().GetForwardedTargets(&products);
                bool changed = false;
                for (const SdfPath& p : products) {
                    UsdRenderProduct product(stage->GetPrimAtPath(p));
                    if (product && product.GetOrderedVarsRel().RemoveTarget(var)) changed = true;
                }
                // The var itself, when it's the product's own.
                if (var.GetParentPath() != SdfPath() && stage->GetPrimAtPath(var) &&
                    std::find(products.begin(), products.end(), var.GetParentPath()) != products.end()) {
                    stage->RemovePrim(var);
                }
                return changed;
            });
        });
        m_tabs->addTab(page, tr("AOVs"));
    }
    layout->addWidget(m_stack, 1);

    // Where the job goes, and the buttons: the app's choice, not the stage's.
    auto* dispatch = new QGroupBox(tr("Render"), content);
    dispatch->setObjectName(QStringLiteral("renderSettingsDispatch"));
    auto* dispatchGrid = new QGridLayout(dispatch);
    m_distribute = new QComboBox(dispatch);
    m_distribute->setObjectName(QStringLiteral("renderSettingsDistribute"));
    m_distribute->addItem(tr("This instance"), QStringLiteral("local"));
    m_distribute->addItem(tr("This instance and peers"), QStringLiteral("both"));
    m_distribute->addItem(tr("Peers only"), QStringLiteral("peers"));
    m_distribute->setToolTip(tr("Who renders the frames and tiles: instances in your render queue rooms that take jobs "
                                "(Render > Swarm) can take a share, or a render queue server takes the job"));
    m_delivery = new QComboBox(dispatch);
    m_delivery->setObjectName(QStringLiteral("renderSettingsDelivery"));
    m_delivery->addItem(tr("Packaged: the scene and its assets, sent"), QStringLiteral("packaged"));
    m_delivery->addItem(tr("Shared: the same files, where peers have them"), QStringLiteral("shared"));
    m_delivery->setToolTip(tr("How peers get the scene. Shared is checked on each peer first: one without every "
                              "asset, the same, gets it packaged"));
    m_rooms = new QToolButton(dispatch);
    m_rooms->setObjectName(QStringLiteral("renderSettingsRooms"));
    m_rooms->setPopupMode(QToolButton::InstantPopup);
    m_rooms->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_rooms->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rooms->setToolTip(tr("The render queue rooms whose instances may take the job"));
    auto* roomsMenu = new QMenu(m_rooms);
    m_rooms->setMenu(roomsMenu);
    connect(roomsMenu, &QMenu::aboutToShow, this, [this]() { fillRoomsMenu(); });
    m_nodes = new QLabel(dispatch);
    m_nodes->setObjectName(QStringLiteral("renderSettingsNodes"));
    m_nodes->setWordWrap(true);
    dispatchGrid->addWidget(new QLabel(tr("Render on"), dispatch), 0, 0);
    dispatchGrid->addWidget(m_distribute, 0, 1);
    dispatchGrid->addWidget(new QLabel(tr("Scene"), dispatch), 1, 0);
    dispatchGrid->addWidget(m_delivery, 1, 1);
    dispatchGrid->addWidget(new QLabel(tr("Rooms"), dispatch), 2, 0);
    dispatchGrid->addWidget(m_rooms, 2, 1);
    dispatchGrid->addWidget(m_nodes, 3, 0, 1, 2);
    dispatchGrid->setColumnStretch(1, 1);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* renderFrame = new QPushButton(tr("Render Frame"), content);
    renderFrame->setObjectName(QStringLiteral("renderSettingsRenderFrame"));
    renderFrame->setToolTip(tr("Render the frame the timeline shows (F12)"));
    auto* renderAnimation = new QPushButton(tr("Render Animation"), content);
    renderAnimation->setObjectName(QStringLiteral("renderSettingsRenderAnimation"));
    renderAnimation->setToolTip(tr("Render the frame range (Ctrl+F12)"));
    buttons->addWidget(renderFrame);
    buttons->addWidget(renderAnimation);
    dispatchGrid->addLayout(buttons, 4, 0, 1, 2);
    auto* dispatchRow = new QHBoxLayout();
    dispatchRow->setContentsMargins(6, 0, 6, 6);
    dispatchRow->addWidget(dispatch);
    layout->addLayout(dispatchRow);
    setContentWidget(content);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(kRefreshDelayMs);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() { refresh(); });

    // --- Edits.
    connect(m_settingsCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_refreshing) return;
        setSettingsPath(SdfPath(m_settingsCombo->currentData().toString().toStdString()));
    });
    connect(m_createButton, &QPushButton::clicked, this, [this]() { createSettings(); });
    connect(emptyCreate, &QPushButton::clicked, this, [this]() { createSettings(); });
    connect(m_rendererCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_refreshing) return;
        const std::string id = m_rendererCombo->currentData().toString().toStdString();
        edit(tr("Set Renderer"), [this, id]() {
            return setJobOption(settingsPrim(), jobattr::renderer, id.empty() ? VtValue() : VtValue(id));
        });
        refreshRendererForm();
    });
    connect(m_cameraCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_refreshing) return;
        const std::string path = m_cameraCombo->currentData().toString().toStdString();
        edit(tr("Set Render Camera"), [this, path]() {
            const UsdPrim prim = imagePrim("camera");
            if (!prim) return false;
            UsdRelationship rel = prim.CreateRelationship(TfToken("camera"), false);
            return path.empty() ? rel.ClearTargets(true) : rel.SetTargets({SdfPath(path)});
        });
    });
    const auto setResolution = [this]() {
        if (m_refreshing) return;
        const GfVec2i size(m_width->value(), m_height->value());
        edit(tr("Set Resolution"), [this, size]() {
            const UsdPrim prim = imagePrim("resolution");
            return prim && prim.CreateAttribute(TfToken("resolution"), SdfValueTypeNames->Int2, false).Set(size);
        });
    };
    connect(m_width, &QSpinBox::editingFinished, this, setResolution);
    connect(m_height, &QSpinBox::editingFinished, this, setResolution);
    connect(m_pixelAspect, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_refreshing) return;
        const float aspect = static_cast<float>(m_pixelAspect->value());
        edit(tr("Set Pixel Aspect"), [this, aspect]() {
            UsdRenderSettings settings(settingsPrim());
            return settings && settings.CreatePixelAspectRatioAttr().Set(aspect);
        });
    });
    connect(m_stageRange, &QCheckBox::toggled, this, [this](bool stageRange) {
        if (m_refreshing) return;
        const double start = m_frameStart->value();
        const double end = m_frameEnd->value();
        edit(tr("Set Frame Range"), [this, stageRange, start, end]() {
            const UsdPrim prim = settingsPrim();
            const bool a = setJobOption(prim, jobattr::frameStart, stageRange ? VtValue() : VtValue(start));
            const bool b = setJobOption(prim, jobattr::frameEnd, stageRange ? VtValue() : VtValue(end));
            return a && b;
        });
    });
    const auto setFrames = [this]() {
        if (m_refreshing || m_stageRange->isChecked()) return;
        const double start = m_frameStart->value();
        const double end = std::max(start, m_frameEnd->value());
        edit(tr("Set Frame Range"), [this, start, end]() {
            const UsdPrim prim = settingsPrim();
            return setJobOption(prim, jobattr::frameStart, VtValue(start)) && setJobOption(prim, jobattr::frameEnd, VtValue(end));
        });
    };
    connect(m_frameStart, &QDoubleSpinBox::editingFinished, this, setFrames);
    connect(m_frameEnd, &QDoubleSpinBox::editingFinished, this, setFrames);
    connect(m_frameStep, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_refreshing) return;
        const double step = m_frameStep->value();
        edit(tr("Set Frame Step"), [this, step]() {
            return setJobOption(settingsPrim(), jobattr::frameStep, step == 1.0 ? VtValue() : VtValue(step));
        });
    });
    connect(m_output, &QLineEdit::editingFinished, this, [this]() {
        if (m_refreshing) return;
        const std::string output = m_output->text().trimmed().toStdString();
        edit(tr("Set Render Output"), [this, output]() {
            return setJobOption(settingsPrim(), jobattr::output, output.empty() ? VtValue() : VtValue(output));
        });
    });
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_refreshing) return;
        const std::string format = m_format->currentData().toString().toStdString();
        edit(tr("Set Render Format"), [this, format]() { return setJobOption(settingsPrim(), jobattr::format, VtValue(format)); });
    });
    const auto setTiles = [this]() {
        if (m_refreshing) return;
        const GfVec2i tiles(m_tilesX->value(), m_tilesY->value());
        edit(tr("Set Render Tiles"), [this, tiles]() {
            return setJobOption(settingsPrim(), jobattr::tiles, tiles == GfVec2i(1, 1) ? VtValue() : VtValue(tiles));
        });
    };
    connect(m_tilesX, &QSpinBox::editingFinished, this, setTiles);
    connect(m_tilesY, &QSpinBox::editingFinished, this, setTiles);
    connect(m_timeLimit, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_refreshing) return;
        const double limit = m_timeLimit->value();
        edit(tr("Set Render Time Limit"), [this, limit]() { return setJobOption(settingsPrim(), jobattr::timeLimit, VtValue(limit)); });
    });
    connect(m_distribute, &QComboBox::currentIndexChanged, this, [this](int) {
        RenderNetwork* network = RenderNetwork::instance();
        if (m_refreshing || !network) return;
        PeerDispatch dispatch = network->dispatch();
        const QString choice = m_distribute->currentData().toString();
        // A queue: "queue:<its name>".
        if (choice.startsWith(QStringLiteral("queue:"))) {
            dispatch.distribute = "queue";
            dispatch.queue = choice.mid(6).toStdString();
        } else {
            dispatch.distribute = choice.toStdString();
            dispatch.queue.clear();
        }
        network->setDispatch(dispatch);
    });
    connect(m_delivery, &QComboBox::currentIndexChanged, this, [this](int) {
        RenderNetwork* network = RenderNetwork::instance();
        if (m_refreshing || !network) return;
        PeerDispatch dispatch = network->dispatch();
        dispatch.delivery = m_delivery->currentData().toString().toStdString();
        network->setDispatch(dispatch);
    });
    if (RenderNetwork* network = RenderNetwork::instance()) {
        connect(network, &RenderNetwork::nodesChanged, this, [this]() { refreshNodes(); });
        connect(network, &RenderNetwork::roomsChanged, this, [this]() { refreshDispatch(); });
        connect(network, &RenderNetwork::dispatchChanged, this, [this]() { refreshDispatch(); });
    }
    if (RenderQueueClient* queues = RenderQueueClient::instance()) {
        connect(queues, &RenderQueueClient::queuesChanged, this, [this]() { refreshDispatch(); });
    }
    refreshDispatch();
    connect(renderFrame, &QPushButton::clicked, this, [this]() { render(false); });
    connect(renderAnimation, &QPushButton::clicked, this, [this]() { render(true); });

    m_renderersConnection = FinalRenderers::instance().changed.connect([this]() {
        m_formRenderer.clear();
        m_refreshTimer->start();
    });

    m_settingsPath = currentStage() ? defaultRenderSettings(currentStage()->usdStage()) : SdfPath();
    connectStage();
    refresh();
    refreshStageCombo();
}

RenderSettingsViewPanel::~RenderSettingsViewPanel() = default;

usdcc::ui::ViewPanel* RenderSettingsViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new RenderSettingsViewPanel(stageManager(), currentStage(), m_undo, windowTitle(), parent);
    copy->setSettingsPath(m_settingsPath);
    return copy;
}

void RenderSettingsViewPanel::setSettingsPath(const SdfPath& path) {
    if (path == m_settingsPath) return;
    m_settingsPath = path;
    m_formRenderer.clear();
    refresh();
}

QJsonObject RenderSettingsViewPanel::saveSessionState() const {
    QJsonObject state;
    if (!m_settingsPath.IsEmpty()) state.insert(QStringLiteral("settings"), text(m_settingsPath.GetString()));
    return state;
}

void RenderSettingsViewPanel::restoreSessionState(const QJsonObject& state) {
    const std::string path = state.value(QStringLiteral("settings")).toString().toStdString();
    if (SdfPath::IsValidPathString(path)) setSettingsPath(SdfPath(path));
}

void RenderSettingsViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Render Frame"), this, [this]() { render(false); });
    menu->addAction(tr("Render Animation"), this, [this]() { render(true); });
    menu->addSeparator();
    menu->addAction(tr("Create Render Settings"), this, [this]() { createSettings(); })->setEnabled(currentStage() != nullptr);
}

void RenderSettingsViewPanel::onStageChanged(const usdcc::usd::StageRefPtr& stage) {
    m_settingsPath = stage ? defaultRenderSettings(stage->usdStage()) : SdfPath();
    m_formRenderer.clear();
    connectStage();
    refresh();
}

void RenderSettingsViewPanel::connectStage() {
    m_stageConnection.disconnect();
    if (!currentStage()) return;
    m_stageConnection = currentStage()->changed.connect([this](const usdcc::usd::StageChanges&) { m_refreshTimer->start(); });
}

UsdPrim RenderSettingsViewPanel::settingsPrim() const {
    if (!currentStage() || m_settingsPath.IsEmpty()) return {};
    const UsdPrim prim = currentStage()->usdStage()->GetPrimAtPath(m_settingsPath);
    return prim && prim.IsA<UsdRenderSettings>() ? prim : UsdPrim();
}

UsdPrim RenderSettingsViewPanel::imagePrim(const char* attribute) const {
    const UsdRenderSettings settings(settingsPrim());
    if (!settings) return {};
    SdfPathVector products;
    settings.GetProductsRel().GetForwardedTargets(&products);
    if (!products.empty()) {
        const UsdPrim product = settings.GetPrim().GetStage()->GetPrimAtPath(products.front());
        if (product) {
            const TfToken name(attribute);
            const UsdAttribute attr = product.GetAttribute(name);
            const UsdRelationship rel = product.GetRelationship(name);
            if ((attr && attr.HasAuthoredValue()) || (rel && rel.HasAuthoredTargets())) return product;
        }
    }
    return settings.GetPrim();
}

std::string RenderSettingsViewPanel::rendererId() const {
    if (!currentStage()) return {};
    return resolvedRendererId(resolveRenderSpec(currentStage()->usdStage(), m_settingsPath, nullptr));
}

void RenderSettingsViewPanel::edit(const QString& description, const std::function<bool()>& editFunction) {
    if (!currentStage() || m_editing) return;
    m_editing = true;
    if (m_undo) {
        m_undo->apply(currentStage(), description, editFunction);
    } else {
        editFunction();
    }
    m_editing = false;
    m_refreshTimer->start();
}

void RenderSettingsViewPanel::createSettings() {
    if (!currentStage()) return;
    SdfPath made;
    edit(tr("Create Render Settings"), [this, &made]() {
        made = createRenderSettings(currentStage()->usdStage());
        return !made.IsEmpty();
    });
    if (!made.IsEmpty()) setSettingsPath(made);
}

void RenderSettingsViewPanel::render(bool animation) {
    // Spin boxes being typed in: what they show is what's rendered.
    if (QWidget* focus = QApplication::focusWidget(); focus && isAncestorOf(focus)) focus->clearFocus();
    submitRender(currentStage(), m_settingsPath, animation, this);
}

void RenderSettingsViewPanel::refresh() {
    m_refreshing = true;
    // A prim that went: the stage's next one.
    if (currentStage() && !m_settingsPath.IsEmpty() && !settingsPrim()) {
        m_settingsPath = defaultRenderSettings(currentStage()->usdStage());
        m_formRenderer.clear();
    }
    fillSettingsCombo();
    fillRendererCombo();
    m_stack->setCurrentIndex(settingsPrim() ? 1 : 0);
    m_createButton->setEnabled(currentStage() != nullptr);
    m_refreshing = false;
    refreshValues();
    refreshRendererForm();
}

void RenderSettingsViewPanel::fillSettingsCombo() {
    m_settingsCombo->clear();
    if (!currentStage()) {
        m_settingsCombo->addItem(tr("(no stage)"), QString());
        m_settingsCombo->setEnabled(false);
        return;
    }
    m_settingsCombo->setEnabled(true);
    const std::vector<SdfPath> prims = renderSettingsPrims(currentStage()->usdStage());
    if (prims.empty()) m_settingsCombo->addItem(tr("(none: defaults)"), QString());
    for (const SdfPath& path : prims) m_settingsCombo->addItem(text(path.GetString()), text(path.GetString()));
    const int index = m_settingsCombo->findData(text(m_settingsPath.GetString()));
    m_settingsCombo->setCurrentIndex(std::max(0, index));
}

void RenderSettingsViewPanel::fillRendererCombo() {
    m_rendererCombo->clear();
    const ResolvedSpec spec =
        currentStage() ? resolveRenderSpec(currentStage()->usdStage(), m_settingsPath, nullptr) : ResolvedSpec();
    auto* model = new QStandardItemModel(m_rendererCombo);
    const auto& renderers = FinalRenderers::instance().all();
    std::string fallback;
    {
        ResolvedSpec unauthored = spec;
        unauthored.options.renderer.clear();
        fallback = resolvedRendererId(unauthored);
    }
    const std::shared_ptr<FinalRenderer> fallbackRenderer = FinalRenderers::instance().find(fallback);
    auto* defaultItem = new QStandardItem(tr("Default (%1)").arg(fallbackRenderer ? text(fallbackRenderer->name()) : tr("none")));
    defaultItem->setData(QString(), Qt::UserRole);
    model->appendRow(defaultItem);
    for (const auto& renderer : renderers) {
        auto* item = new QStandardItem(text(renderer->name()));
        item->setData(text(renderer->id()), Qt::UserRole);
        const std::string problem = renderer->setupProblem();
        if (!problem.empty()) {
            item->setToolTip(text(problem));
            item->setText(tr("%1 (not available)").arg(text(renderer->name())));
        }
        model->appendRow(item);
    }
    // One the prim names that isn't here.
    if (!spec.options.renderer.empty() && !FinalRenderers::instance().find(spec.options.renderer)) {
        auto* item = new QStandardItem(tr("%1 (not here)").arg(text(spec.options.renderer)));
        item->setData(text(spec.options.renderer), Qt::UserRole);
        model->appendRow(item);
    }
    m_rendererCombo->setModel(model);
    const int index = m_rendererCombo->findData(text(spec.options.renderer), Qt::UserRole);
    m_rendererCombo->setCurrentIndex(std::max(0, index));
    m_rendererCombo->setEnabled(settingsPrim().IsValid());

    const std::shared_ptr<FinalRenderer> current = FinalRenderers::instance().find(resolvedRendererId(spec));
    const std::string problem = current ? current->setupProblem() : std::string("No renderer here: ") + spec.options.renderer;
    m_rendererProblem->setText(text(problem));
    m_rendererProblem->setVisible(!problem.empty() && currentStage());
}

void RenderSettingsViewPanel::fillCameraCombo() {
    m_cameraCombo->clear();
    m_cameraCombo->addItem(tr("Active viewport's camera"), QString());
    for (const SdfPath& path : usdcc::usd::cameraPrims(currentStage())) m_cameraCombo->addItem(text(path.GetString()), text(path.GetString()));
}

void RenderSettingsViewPanel::refreshValues() {
    if (!currentStage()) return;
    m_refreshing = true;
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(rendererId());
    const ResolvedSpec spec = resolveRenderSpec(currentStage()->usdStage(), m_settingsPath, renderer.get());

    fillCameraCombo();
    const int camera = m_cameraCombo->findData(text(spec.camera.GetString()));
    if (camera < 0 && !spec.camera.IsEmpty()) {
        m_cameraCombo->addItem(tr("%1 (not a camera)").arg(text(spec.camera.GetString())), text(spec.camera.GetString()));
        m_cameraCombo->setCurrentIndex(m_cameraCombo->count() - 1);
    } else {
        m_cameraCombo->setCurrentIndex(std::max(0, camera));
    }
    m_width->setValue(spec.resolution[0]);
    m_height->setValue(spec.resolution[1]);
    m_pixelAspect->setValue(spec.pixelAspect);

    const usdcc::usd::TimeRange range = usdcc::usd::stageTimeRange(currentStage()->usdStage());
    const bool stageRange = !spec.options.frameStart && !spec.options.frameEnd;
    m_stageRange->setChecked(stageRange);
    m_frameStart->setValue(spec.options.frameStart.value_or(range.start));
    m_frameEnd->setValue(spec.options.frameEnd.value_or(range.end));
    m_frameStart->setEnabled(!stageRange);
    m_frameEnd->setEnabled(!stageRange);
    m_frameStep->setValue(spec.options.frameStep);
    m_output->setText(text(spec.options.output));
    m_format->setCurrentIndex(std::max(0, m_format->findData(text(spec.options.format))));
    m_tilesX->setValue(spec.options.tilesX);
    m_tilesY->setValue(spec.options.tilesY);
    const bool tiles = renderer && renderer->capabilities().tiles;
    m_tilesX->setEnabled(tiles);
    m_tilesY->setEnabled(tiles);
    if (!tiles) m_tilesX->setToolTip(tr("%1 renders whole frames only").arg(renderer ? text(renderer->name()) : tr("This renderer")));
    m_timeLimit->setValue(spec.options.timeLimit);

    // An example of the output's name.
    RenderJob example;
    example.spec = spec;
    example.stageName = QFileInfo(text(currentStage()->usdStage()->GetRootLayer()->GetDisplayName())).completeBaseName().toStdString();
    if (example.stageName.empty()) example.stageName = "untitled";
    example.settingsName = m_settingsPath.IsEmpty() ? "default" : m_settingsPath.GetName();
    example.rendererName = renderer ? renderer->name() : std::string();
    example.frames = {m_frameStart->value(), m_frameEnd->value()};
    if (!spec.products.empty()) {
        const ProductSpec& product = spec.products.front();
        m_outputExample->setText(tr("e.g. %1").arg(text(outputPath(example, product, product.aovs.empty() ? std::string() : product.aovs.front().name,
                                                                   example.frames.front()))));
    }
    m_refreshing = false;
    fillAovs();
    refreshNodes();
}

void RenderSettingsViewPanel::refreshNodes() {
    RenderNetwork* network = RenderNetwork::instance();
    if (!network || network->rooms().isEmpty()) {
        m_nodes->setText(tr("No render queue rooms: host or join one in Render > Swarm to render with others."));
        return;
    }
    const PeerDispatch dispatch = network->dispatch();
    if (dispatch.distribute == "local") {
        m_nodes->setText(tr("Only this instance renders."));
        return;
    }
    if (dispatch.distribute == "queue") {
        const RenderQueueClient* queues = RenderQueueClient::instance();
        const std::optional<RenderQueueInfo> queue = queues ? queues->queue(text(dispatch.queue)) : std::nullopt;
        if (!queue) {
            m_nodes->setText(tr("The queue %1 isn't in your rooms now.").arg(text(dispatch.queue)));
        } else {
            m_nodes->setText(queue->outputRoot ? tr("The job is %1's: it renders it on its nodes and writes the frames to its "
                                                    "shared folder. You may close the app.").arg(queue->name)
                                               : tr("The job is %1's: it renders it on its nodes, and the frames come back here "
                                                    "when you're in its room.").arg(queue->name));
        }
        return;
    }
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(rendererId());
    const bool tiles = m_tilesX->value() * m_tilesY->value() > 1;
    QStringList names;
    for (const RenderNode& node : network->nodesFor(rendererId(), tiles, dispatch.rooms)) {
        const QString name = node.peer.name + (node.busy ? tr(" (busy)") : QString());
        if (!names.contains(name)) names.append(name);  // in two of the rooms: once
    }
    if (names.isEmpty()) {
        m_nodes->setText(tr("Nobody in these rooms renders with %1 for others now.")
                             .arg(renderer ? text(renderer->name()) : tr("this renderer")));
    } else {
        m_nodes->setText(tr("Can take a share: %1").arg(names.join(QStringLiteral(", "))));
    }
}

void RenderSettingsViewPanel::refreshDispatch() {
    RenderNetwork* network = RenderNetwork::instance();
    const PeerDispatch dispatch = network ? network->dispatch() : PeerDispatch();
    const bool wasRefreshing = m_refreshing;
    m_refreshing = true;
    fillDistribute();
    m_distribute->setCurrentIndex(std::max(0, m_distribute->findData(
        dispatch.distribute == "queue" ? QStringLiteral("queue:") + text(dispatch.queue) : text(dispatch.distribute))));
    m_delivery->setCurrentIndex(std::max(0, m_delivery->findData(text(dispatch.delivery))));
    m_refreshing = wasRefreshing;
    const bool peers = dispatch.distribute == "both" || dispatch.distribute == "peers";
    m_distribute->setEnabled(network != nullptr);
    m_delivery->setEnabled(network && peers);
    const QList<usdcc::peers::Room*> rooms = network ? network->rooms() : QList<usdcc::peers::Room*>();
    int chosen = 0;
    for (usdcc::peers::Room* room : rooms) {
        if (dispatch.rooms.empty() ||
            std::find(dispatch.rooms.begin(), dispatch.rooms.end(), RenderNetwork::roomKey(room)) != dispatch.rooms.end()) {
            ++chosen;
        }
    }
    if (rooms.isEmpty()) {
        m_rooms->setText(tr("No render queue rooms: open Render > Swarm"));
    } else if (dispatch.rooms.empty() || chosen == rooms.size()) {
        m_rooms->setText(rooms.size() == 1 ? rooms.first()->name() : tr("All %1 rooms").arg(rooms.size()));
    } else if (chosen == 1) {
        for (usdcc::peers::Room* room : rooms) {
            if (std::find(dispatch.rooms.begin(), dispatch.rooms.end(), RenderNetwork::roomKey(room)) != dispatch.rooms.end()) {
                m_rooms->setText(room->name());
            }
        }
    } else {
        m_rooms->setText(tr("%1 of %2 rooms").arg(chosen).arg(rooms.size()));
    }
    m_rooms->setEnabled(peers && !rooms.isEmpty());
    refreshNodes();
}

void RenderSettingsViewPanel::fillDistribute() {
    while (m_distribute->count() > 3) m_distribute->removeItem(m_distribute->count() - 1);
    QStringList names;
    if (const RenderQueueClient* queues = RenderQueueClient::instance()) {
        for (const RenderQueueInfo& queue : queues->queues()) {
            if (!names.contains(queue.name)) names.append(queue.name);
        }
    }
    // The one chosen before, while it isn't here.
    const RenderNetwork* network = RenderNetwork::instance();
    const QString chosen = network && network->dispatch().distribute == "queue" ? text(network->dispatch().queue) : QString();
    for (const QString& name : names) m_distribute->addItem(tr("Queue: %1").arg(name), QStringLiteral("queue:") + name);
    if (!chosen.isEmpty() && !names.contains(chosen)) {
        m_distribute->addItem(tr("Queue: %1 (not here)").arg(chosen), QStringLiteral("queue:") + chosen);
    }
}

void RenderSettingsViewPanel::fillRoomsMenu() {
    QMenu* menu = m_rooms->menu();
    menu->clear();
    RenderNetwork* network = RenderNetwork::instance();
    if (!network) return;
    const QList<usdcc::peers::Room*> rooms = network->rooms();
    const PeerDispatch dispatch = network->dispatch();
    std::vector<std::string> keys;
    for (usdcc::peers::Room* room : rooms) keys.push_back(RenderNetwork::roomKey(room));
    const auto chosen = [&](const std::string& key) {
        return dispatch.rooms.empty() || std::find(dispatch.rooms.begin(), dispatch.rooms.end(), key) != dispatch.rooms.end();
    };
    QAction* all = menu->addAction(tr("All Rooms"));
    all->setCheckable(true);
    all->setChecked(std::all_of(keys.begin(), keys.end(), chosen));
    connect(all, &QAction::triggered, this, []() {
        if (RenderNetwork* n = RenderNetwork::instance()) {
            PeerDispatch d = n->dispatch();
            d.rooms.clear();
            n->setDispatch(d);
        }
    });
    menu->addSeparator();
    for (usdcc::peers::Room* room : rooms) {
        const std::string key = RenderNetwork::roomKey(room);
        int takers = 0;
        for (const RenderNode& node : network->nodes()) {
            if (node.room == room && node.node) ++takers;
        }
        QAction* action = menu->addAction(tr("%1  (%n taking jobs)", nullptr, takers).arg(room->name()));
        action->setCheckable(true);
        action->setChecked(chosen(key));
        connect(action, &QAction::toggled, this, [key, keys](bool on) {
            RenderNetwork* n = RenderNetwork::instance();
            if (!n) return;
            PeerDispatch d = n->dispatch();
            std::vector<std::string> now = d.rooms.empty() ? keys : d.rooms;
            now.erase(std::remove(now.begin(), now.end(), key), now.end());
            if (on) now.push_back(key);
            if (now.empty()) return;  // at least one: none would be every room
            const bool every = std::all_of(keys.begin(), keys.end(), [&](const std::string& k) {
                return std::find(now.begin(), now.end(), k) != now.end();
            });
            d.rooms = every ? std::vector<std::string>() : now;
            n->setDispatch(d);
        });
    }
}

void RenderSettingsViewPanel::fillAovs() {
    m_aovList->clear();
    const UsdRenderSettings settings(settingsPrim());
    if (!settings) return;
    const UsdStagePtr stage = settings.GetPrim().GetStage();
    SdfPathVector products;
    settings.GetProductsRel().GetForwardedTargets(&products);
    for (const SdfPath& p : products) {
        const UsdRenderProduct product(stage->GetPrimAtPath(p));
        if (!product) continue;
        SdfPathVector vars;
        product.GetOrderedVarsRel().GetForwardedTargets(&vars);
        for (const SdfPath& v : vars) {
            const UsdRenderVar var(stage->GetPrimAtPath(v));
            std::string source;
            TfToken type;
            if (var) {
                var.GetSourceNameAttr().Get(&source);
                var.GetDataTypeAttr().Get(&type);
            }
            auto* item = new QListWidgetItem(tr("%1 — %2 (%3)  in %4")
                                                 .arg(text(v.GetName()), text(source.empty() ? v.GetName() : source),
                                                      text(type.GetString()), text(p.GetName())),
                                             m_aovList);
            item->setData(Qt::UserRole, text(v.GetString()));
        }
    }
    if (m_aovList->count() == 0) new QListWidgetItem(tr("(none: the color AOV)"), m_aovList);
}

void RenderSettingsViewPanel::refreshRendererForm() {
    const std::string id = rendererId();
    if (id != m_formRenderer) buildRendererForm();
    for (const auto& refresher : m_fieldRefreshers) refresher();
}

void RenderSettingsViewPanel::buildRendererForm() {
    m_formRenderer = rendererId();
    m_fieldRefreshers.clear();
    m_fields.clear();
    while (m_rendererForm->rowCount() > 0) m_rendererForm->removeRow(0);
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(m_formRenderer);
    if (!renderer) {
        m_rendererForm->addRow(new QLabel(tr("No renderer."), m_rendererPage));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_fields = renderer->settingsSchema();
    QApplication::restoreOverrideCursor();
    if (m_fields.empty()) {
        m_rendererForm->addRow(new QLabel(tr("%1 has no settings of its own.").arg(text(renderer->name())), m_rendererPage));
        return;
    }
    std::string group;
    for (size_t i = 0; i < m_fields.size(); ++i) {
        const RenderSettingField field = m_fields[i];
        if (field.group != group) {
            group = field.group;
            auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(text(group).toHtmlEscaped()), m_rendererPage);
            m_rendererForm->addRow(heading);
        }
        auto* label = new QLabel(text(field.label), m_rendererPage);
        const QString tip = text(field.attribute) + (field.help.empty() ? QString() : QStringLiteral("\n") + text(field.help));
        label->setToolTip(tip);
        auto* row = new QWidget(m_rendererPage);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* reset = new QToolButton(row);
        reset->setText(QStringLiteral("↺"));
        reset->setToolTip(tr("Back to the default: clears %1 on the prim").arg(text(field.attribute)));
        reset->setAutoRaise(true);

        const auto setValue = [this, field](const VtValue& value) {
            if (m_refreshing) return;
            edit(tr("Set %1").arg(text(field.label)), [this, field, value]() { return setFieldValue(settingsPrim(), field, value); });
        };
        const auto current = [this, field]() {
            const VtValue authoredValue = fieldValue(settingsPrim(), field);
            return std::make_pair(!authoredValue.IsEmpty(), authoredValue.IsEmpty() ? field.defaultValue : authoredValue);
        };
        std::function<void(const VtValue&)> show;
        using Type = RenderSettingField::Type;
        switch (field.type) {
        case Type::Bool: {
            auto* box = new QCheckBox(row);
            connect(box, &QCheckBox::toggled, this, [setValue](bool on) { setValue(VtValue(on)); });
            show = [box](const VtValue& v) { box->setChecked(VtValue::Cast<bool>(v).IsHolding<bool>() && VtValue::Cast<bool>(v).UncheckedGet<bool>()); };
            rowLayout->addWidget(box);
            break;
        }
        case Type::Int: {
            auto* box = new QSpinBox(row);
            box->setRange(field.hasRange ? static_cast<int>(field.minimum) : -1000000000,
                          field.hasRange ? static_cast<int>(field.maximum) : 1000000000);
            connect(box, &QSpinBox::editingFinished, this, [setValue, box]() { setValue(VtValue(box->value())); });
            show = [box](const VtValue& v) { box->setValue(static_cast<int>(std::lround(asDouble(v)))); };
            rowLayout->addWidget(box, 1);
            break;
        }
        case Type::Float: {
            auto* box = new QDoubleSpinBox(row);
            box->setDecimals(4);
            box->setRange(field.hasRange ? field.minimum : -1e9, field.hasRange ? field.maximum : 1e9);
            box->setSingleStep(0.1);
            connect(box, &QDoubleSpinBox::editingFinished, this, [setValue, box]() { setValue(VtValue(box->value())); });
            show = [box](const VtValue& v) { box->setValue(asDouble(v)); };
            rowLayout->addWidget(box, 1);
            break;
        }
        case Type::Choice: {
            auto* box = new QComboBox(row);
            for (const std::string& choice : field.choices) box->addItem(text(choice));
            connect(box, &QComboBox::activated, this, [setValue, box](int) { setValue(VtValue(box->currentText().toStdString())); });
            show = [box](const VtValue& v) { box->setCurrentIndex(std::max(0, box->findText(text(asString(v))))); };
            rowLayout->addWidget(box, 1);
            break;
        }
        case Type::Color: {
            auto* button = new QPushButton(row);
            button->setFixedWidth(48);
            auto color = std::make_shared<GfVec3f>(1.0f, 1.0f, 1.0f);
            connect(button, &QPushButton::clicked, this, [setValue, button, color]() {
                const QColor chosen = QColorDialog::getColor(QColor::fromRgbF((*color)[0], (*color)[1], (*color)[2]), button);
                if (chosen.isValid()) setValue(VtValue(GfVec3f(chosen.redF(), chosen.greenF(), chosen.blueF())));
            });
            show = [button, color](const VtValue& v) {
                const VtValue cast = VtValue::Cast<GfVec3f>(v);
                if (cast.IsHolding<GfVec3f>()) *color = cast.UncheckedGet<GfVec3f>();
                button->setStyleSheet(QStringLiteral("background-color: %1;")
                                          .arg(QColor::fromRgbF(std::clamp((*color)[0], 0.0f, 1.0f), std::clamp((*color)[1], 0.0f, 1.0f),
                                                                std::clamp((*color)[2], 0.0f, 1.0f)).name()));
            };
            rowLayout->addWidget(button);
            rowLayout->addStretch(1);
            break;
        }
        case Type::String:
        default: {
            auto* line = new QLineEdit(row);
            connect(line, &QLineEdit::editingFinished, this, [setValue, line]() { setValue(VtValue(line->text().toStdString())); });
            show = [line](const VtValue& v) { line->setText(text(asString(v))); };
            rowLayout->addWidget(line, 1);
            break;
        }
        }
        rowLayout->addWidget(reset);
        connect(reset, &QToolButton::clicked, this, [this, field]() {
            edit(tr("Reset %1").arg(text(field.label)), [this, field]() { return setFieldValue(settingsPrim(), field, VtValue()); });
        });
        row->setToolTip(tip);
        m_rendererForm->addRow(label, row);
        m_fieldRefreshers.push_back([this, current, show, label, reset, row]() {
            const auto [isAuthored, value] = current();
            const bool wasRefreshing = m_refreshing;
            m_refreshing = true;
            show(value);
            m_refreshing = wasRefreshing;
            QFont font = label->font();
            font.setBold(isAuthored);
            label->setFont(font);
            reset->setEnabled(isAuthored);
            row->setEnabled(settingsPrim().IsValid());
        });
    }
}

}  // namespace usdcc::render
