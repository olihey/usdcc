#include "usdcc/usd/material_editor_view_panel.h"

#include "usdcc/ui/find_line_edit.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/theme.h"
#include "usdcc/usd/add_shade_input_dialog.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/property_sheet.h"
#include "usdcc/usd/shade_actions.h"
#include "usdcc/usd/shader_node_catalog.h"
#include "usdcc/usd/shader_node_graphics.h"
#include "usdcc/usd/shader_node_palette.h"
#include "usdcc/usd/shading_preview_widget.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_shade_graph_model.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsView>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdUI/backdrop.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

using QtNodes::NodeId;
using QtNodes::PortIndex;
using QtNodes::PortType;

namespace usdcc::usd {

namespace {
constexpr double kSmallestToOpenAt = 0.6;  // smaller than this can't be read
constexpr double kSmallestToFrame = 0.2;
constexpr auto kClipboardMime = "application/x-usdcc-shade-nodes";
constexpr float kPasteOffset = 0.25f;  // of a node's size, so a copy doesn't hide what it's of
// Whether a new panel shows the preview and the parameters: as the last one changed them.
constexpr auto kShowPreviewKey = "MaterialEditor/showPreview";
constexpr auto kShowParametersKey = "MaterialEditor/showParameters";
constexpr int kSideWidth = 320;
// What the Parameters keep below the preview, however wide the pane on the right.
constexpr int kMinParametersHeight = 120;

// The app's theme, for what QtNodes draws itself. Its styles are process-wide.
void applyTheme() {
    using usdcc::ui::theme::Token;
    const auto name = [](Token token) { return usdcc::ui::theme::color(token).name(); };
    const QString accent = usdcc::ui::theme::accent().name();
    QtNodes::GraphicsViewStyle::setStyle(QStringLiteral(R"({"GraphicsViewStyle": {
        "BackgroundColor": "%1", "FineGridColor": "%2", "CoarseGridColor": "%3"}})")
                                              .arg(name(Token::SurfacePanel), name(Token::GraphGridFine), name(Token::GraphGridCoarse)));
    // ShaderNodePainter draws the nodes: these are what QtNodes still reads, such as the opacity.
    QtNodes::NodeStyle::setNodeStyle(QStringLiteral(R"({"NodeStyle": {
        "NormalBoundaryColor": [70, 70, 74], "SelectedBoundaryColor": "%1",
        "GradientColor0": [50, 50, 54], "GradientColor1": [46, 46, 50], "GradientColor2": [44, 44, 48], "GradientColor3": [42, 42, 46],
        "ShadowColor": [10, 10, 10], "ShadowEnabled": false, "FontColor": "%2", "FontColorFaded": "%3",
        "ConnectionPointColor": [160, 160, 164], "FilledConnectionPointColor": "%1", "ErrorColor": [255, 95, 86], "WarningColor": [255, 189, 46],
        "ToolTipIconColor": "white", "PenWidth": 1.0, "HoveredPenWidth": 1.5, "ConnectionPointDiameter": 9.0, "Opacity": 1.0}})")
                                      .arg(accent, name(Token::NodeText), name(Token::NodeTextMuted)));
    QtNodes::ConnectionStyle::setConnectionStyle(QStringLiteral(R"({"ConnectionStyle": {
        "ConstructionColor": "%2", "NormalColor": "%3", "SelectedColor": "%1", "SelectedHaloColor": "%1", "HoveredColor": "%1",
        "LineWidth": 2.0, "ConstructionLineWidth": 2.0, "PointDiameter": 8.0, "UseDataDefinedColors": false}})")
                                                     .arg(accent, name(Token::Icon), name(Token::GraphConnection)));
}

QtNodes::NodeGraphicsObject* nodeAt(QtNodes::GraphicsView* view, QPoint viewportPoint) {
    for (QGraphicsItem* item = view->itemAt(viewportPoint); item; item = item->parentItem()) {
        if (auto* node = dynamic_cast<QtNodes::NodeGraphicsObject*>(item)) return node;
    }
    return nullptr;
}

ShadeBackdropItem* backdropAt(QtNodes::GraphicsView* view, QPoint viewportPoint) {
    return dynamic_cast<ShadeBackdropItem*>(view->itemAt(viewportPoint));
}

GfVec2f stored(QPointF scenePosition) { return GfVec2f(float(scenePosition.x()) / kShadeNodePixels, float(scenePosition.y()) / kShadeNodePixels); }
}  // namespace

MaterialEditorViewPanel::MaterialEditorViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo, const QString& title,
                                                 QWidget* parent)
    : ViewPanel(stageManager, std::move(stage), title, parent), m_undo(std::move(undo)) {
    applyTheme();

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    m_pin = new QToolButton(toolbar);
    m_pin->setObjectName(QStringLiteral("materialEditorPin"));
    m_pin->setText(tr("Pin"));
    m_pin->setCheckable(true);
    m_pin->setToolTip(tr("Stay on this material when the selection changes"));
    toolbarLayout->addWidget(m_pin);
    m_paletteButton = new QToolButton(toolbar);
    m_paletteButton->setObjectName(QStringLiteral("materialEditorPaletteButton"));
    m_paletteButton->setText(tr("Nodes"));
    m_paletteButton->setCheckable(true);
    m_paletteButton->setToolTip(tr("Show the shader nodes to add. Tab searches them from the graph."));
    toolbarLayout->addWidget(m_paletteButton);
    m_previewButton = new QToolButton(toolbar);
    m_previewButton->setObjectName(QStringLiteral("materialEditorPreviewButton"));
    m_previewButton->setText(tr("Preview"));
    m_previewButton->setCheckable(true);
    m_previewButton->setToolTip(tr("Show the material on a shape"));
    m_paramsButton = new QToolButton(toolbar);
    m_paramsButton->setObjectName(QStringLiteral("materialEditorParametersButton"));
    m_paramsButton->setText(tr("Parameters"));
    m_paramsButton->setCheckable(true);
    m_paramsButton->setToolTip(tr("Show the selected node's inputs, or the material's interface"));
    m_breadcrumb = new QHBoxLayout;
    m_breadcrumb->setContentsMargins(6, 0, 0, 0);
    m_breadcrumb->setSpacing(2);
    toolbarLayout->addLayout(m_breadcrumb);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(m_previewButton);
    toolbarLayout->addWidget(m_paramsButton);

    m_split = new QSplitter(Qt::Horizontal, container);
    m_palette = new ShaderNodePalette(m_split);
    m_palette->hide();
    m_stack = new QStackedWidget(m_split);
    m_view = new QtNodes::GraphicsView(m_stack);
    m_view->setScaleRange(kSmallestToFrame, 2.0);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);
    m_view->viewport()->setAcceptDrops(true);
    m_view->setFocusPolicy(Qt::StrongFocus);
    m_empty = new QLabel(tr("Select a Material, or a prim with one bound."), m_stack);
    m_empty->setAlignment(Qt::AlignCenter);
    m_stack->addWidget(m_view);
    const auto recolor = [this]() {
        applyTheme();
        m_view->setBackgroundBrush(usdcc::ui::theme::color(usdcc::ui::theme::Token::SurfacePanel));
        m_view->resetCachedContent();
        if (m_scene) {
            for (QGraphicsItem* item : m_scene->items()) item->update();  // they are cached
        }
        m_view->viewport()->update();
    };
    usdcc::ui::theme::onChanged(this, recolor);
    m_stack->addWidget(m_empty);
    m_side = new QSplitter(Qt::Vertical, m_split);
    m_side->setObjectName(QStringLiteral("materialEditorSide"));
    m_preview = new ShadingPreviewWidget(m_side);
    m_params = new PropertySheet(this->stageManager(), m_undo, PropertySheet::Mode::Shading, m_side);
    m_params->setEmptyText(tr("Select a material, or a node in one."));
    m_side->addWidget(m_preview);
    m_side->addWidget(m_params);
    m_side->setStretchFactor(0, 0);
    m_side->setStretchFactor(1, 1);
    m_side->setChildrenCollapsible(false);
    // The preview's height follows the pane's width (fitPreview()), so the handle between it
    // and the Parameters doesn't move.
    m_side->handle(1)->setEnabled(false);
    m_side->handle(1)->setCursor(Qt::ArrowCursor);
    m_side->installEventFilter(this);
    connect(m_preview, &ShadingPreviewWidget::squareHeightChanged, this, [this]() { fitPreview(); });
    m_split->addWidget(m_palette);
    m_split->addWidget(m_stack);
    m_split->addWidget(m_side);
    m_split->setStretchFactor(0, 0);
    m_split->setStretchFactor(1, 1);
    m_split->setStretchFactor(2, 0);
    // Find: under the toolbar, shown by Ctrl+F (§4.1).
    m_findBar = new QWidget(container);
    m_findBar->setObjectName(QStringLiteral("panelToolbar"));
    m_findBar->setAttribute(Qt::WA_StyledBackground);
    auto* findLayout = new QHBoxLayout(m_findBar);
    usdcc::ui::scaled::margins(findLayout, 8, 3, 8, 3);
    m_findField = new QComboBox(m_findBar);
    m_findField->setObjectName(QStringLiteral("materialEditorFindField"));
    m_findField->setToolTip(tr("What to look at in each node"));
    const auto addField = [this](const QString& text, ShadeFindField field, const QString& toolTip) {
        m_findField->addItem(text, static_cast<int>(field));
        m_findField->setItemData(m_findField->count() - 1, toolTip, Qt::ToolTipRole);
    };
    addField(tr("Any"), ShadeFindField::Any, tr("All of these"));
    addField(tr("Name"), ShadeFindField::Name, tr("The node's prim name"));
    addField(tr("Type"), ShadeFindField::Type, tr("What it is: its definition's label, name, identifier or function, or NodeGraph"));
    addField(tr("Shading System"), ShadeFindField::ShadingSystem, tr("glslfx, mtlx and so on, or their names, such as MaterialX"));
    addField(tr("Context"), ShadeFindField::Context, tr("Surface, displacement, volume, pattern and so on"));
    auto* findEdit = new usdcc::ui::FindLineEdit(m_findBar);
    m_findEdit = findEdit;
    m_findEdit->setObjectName(QStringLiteral("materialEditorFind"));
    m_findEdit->setPlaceholderText(tr("Find nodes"));
    m_findEdit->setClearButtonEnabled(true);
    findEdit->onEnter = [this](bool backward) { findNext(backward); };
    findEdit->onEscape = [this]() { closeFind(); };
    m_findCount = new QLabel(m_findBar);
    m_findCount->setObjectName(QStringLiteral("propertyNote"));
    const auto barButton = [this](const QString& text, const QString& toolTip) {
        auto* button = new QPushButton(text, m_findBar);
        button->setAutoDefault(false);
        button->setToolTip(toolTip);
        return button;
    };
    QPushButton* previous = barButton(tr("Previous"), tr("The previous match (Shift+Enter, Shift+F3)"));
    QPushButton* next = barButton(tr("Next"), tr("The next match (Enter, F3)"));
    QToolButton* close = properties::iconButton(QStringLiteral(":/usdcc/theme/close.png"), tr("Close (Esc)"), m_findBar);
    findLayout->addWidget(m_findField);
    findLayout->addWidget(m_findEdit, 1);
    findLayout->addWidget(m_findCount);
    findLayout->addWidget(previous);
    findLayout->addWidget(next);
    findLayout->addWidget(close);
    m_findBar->setVisible(false);
    connect(previous, &QPushButton::clicked, this, [this]() { findNext(true); });
    connect(next, &QPushButton::clicked, this, [this]() { findNext(false); });
    connect(close, &QToolButton::clicked, this, [this]() { closeFind(); });
    connect(m_findEdit, &QLineEdit::textChanged, this, [this]() { updateFind(true); });
    connect(m_findField, &QComboBox::currentIndexChanged, this, [this]() { updateFind(true); });
    for (const auto& [keys, backward] : {std::pair{QKeySequence(QKeySequence::FindNext), false}, std::pair{QKeySequence(QKeySequence::FindPrevious), true}}) {
        auto* step = new QShortcut(keys, m_findBar, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
        connect(step, &QShortcut::activated, this, [this, backward = backward]() { findNext(backward); });
    }
    // An edit anywhere may change what's found: once it's settled, without moving the view.
    m_findRefresh = new QTimer(this);
    m_findRefresh->setSingleShot(true);
    m_findRefresh->setInterval(50);
    connect(m_findRefresh, &QTimer::timeout, this, [this]() { updateFind(false); });

    layout->addWidget(toolbar);
    layout->addWidget(m_findBar);
    layout->addWidget(m_split, 1);
    setContentWidget(container);

    connect(m_paletteButton, &QToolButton::toggled, this, [this](bool shown) { setPaletteShown(shown); });
    {
        const QSettings settings = usdcc::ui::appSettings();
        m_previewButton->setChecked(settings.value(kShowPreviewKey, true).toBool());
        m_paramsButton->setChecked(settings.value(kShowParametersKey, true).toBool());
    }
    connect(m_previewButton, &QToolButton::toggled, this, [this](bool shown) {
        usdcc::ui::appSettings().setValue(kShowPreviewKey, shown);
        updateSideVisibility();
    });
    connect(m_paramsButton, &QToolButton::toggled, this, [this](bool shown) {
        usdcc::ui::appSettings().setValue(kShowParametersKey, shown);
        updateSideVisibility();
    });
    updateSideVisibility();
    connect(m_palette, &ShaderNodePalette::nodeChosen, this, [this](const QString& identifier) {
        addNode(identifier, m_view->mapToScene(m_view->viewport()->rect().center()));
    });

    auto* frame = new QShortcut(QKeySequence(Qt::Key_F), m_view);
    frame->setContext(Qt::WidgetWithChildrenShortcut);
    connect(frame, &QShortcut::activated, this, [this]() { fitGraph(kSmallestToFrame); });

    m_selectionChanged = this->stageManager()->selectionChanged.connect([this](StageRefPtr changed, std::vector<SdfPath> paths) {
        if (changed != currentStage()) return;
        selectionFromStage(paths);
        updateSidePane();
    });

    refreshStageCombo();
    attachStage();
}

MaterialEditorViewPanel::~MaterialEditorViewPanel() {
    m_view->setScene(nullptr);
    m_backdropItems.clear();
    m_scene.reset();
    m_model.reset();
}

usdcc::ui::ViewPanel* MaterialEditorViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new MaterialEditorViewPanel(stageManager(), currentStage(), m_undo, windowTitle(), parent);
    copy->showLevel(m_level);
    copy->setPinned(isPinned());
    copy->restoreSessionState(saveSessionState());
    return copy;
}

QJsonObject MaterialEditorViewPanel::saveSessionState() const {
    return {{QStringLiteral("preview"), isPreviewShown()},
            {QStringLiteral("parameters"), areParametersShown()},
            {QStringLiteral("previewState"), m_preview->saveState()},
            {QStringLiteral("findField"), m_findField->currentIndex()}};
}

void MaterialEditorViewPanel::restoreSessionState(const QJsonObject& state) {
    if (state.contains(QStringLiteral("preview"))) setPreviewShown(state.value(QStringLiteral("preview")).toBool());
    if (state.contains(QStringLiteral("parameters"))) setParametersShown(state.value(QStringLiteral("parameters")).toBool());
    if (state.contains(QStringLiteral("previewState"))) m_preview->restoreState(state.value(QStringLiteral("previewState")).toObject());
    const int field = state.value(QStringLiteral("findField")).toInt(-1);
    if (field >= 0 && field < m_findField->count()) m_findField->setCurrentIndex(field);
}

void MaterialEditorViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Find..."), QKeySequence(QKeySequence::Find), this, [this]() { showFind(); });
    menu->addAction(tr("Frame All"), this, [this]() { fitGraph(kSmallestToFrame); });
    menu->addAction(tr("Auto Layout"), this, [this]() { autoLayout(); });
    QAction* selected = menu->addAction(tr("Layout Selected"), this, [this]() { layoutSelected(); });
    selected->setEnabled(m_scene && !m_scene->selectedNodes().empty());
}

void MaterialEditorViewPanel::onStageChanged(const StageRefPtr&) { attachStage(); }

void MaterialEditorViewPanel::attachStage() {
    m_view->setScene(nullptr);
    m_backdropItems.clear();
    m_scene.reset();
    m_model.reset();
    m_primsMoved = {};
    m_findStageChanged = {};
    m_level = SdfPath();
    m_findRoot = SdfPath();
    const StageRefPtr& stage = currentStage();
    m_params->setStage(stage);
    m_preview->setStage(stage);
    if (stage) {
        m_model = std::make_unique<UsdShadeGraphModel>(stage, m_undo);
        m_scene = std::make_unique<QtNodes::BasicGraphicsScene>(*m_model);
        m_scene->setNodeGeometry(std::make_unique<ShaderNodeGeometry>(*m_model));
        m_scene->setNodePainter(std::make_unique<ShaderNodePainter>());
        m_view->setScene(m_scene.get());
        m_scene->installEventFilter(this);
        // QtNodes' own Undo, Redo, Delete, Cut, Copy and Paste work on a QUndoStack of its
        // own that saves its model as JSON. The stage is the only data here, and Edit > Undo
        // is its undo, so none of them can be left to catch those keys.
        for (QAction* action : m_view->actions()) m_view->removeAction(action);

        connect(m_scene.get(), &QGraphicsScene::selectionChanged, this, [this]() { selectionToStage(); });
        // Not from inside the node's own event: entering replaces it.
        connect(m_scene.get(), &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this, [this](NodeId id) {
            const ShadeGraphNode* node = m_model->shadeNode(id);
            if (!node || node->kind != ShadeNodeKind::NodeGraph) return;
            QTimer::singleShot(0, this, [this, path = node->path]() { showLevel(path); });
        });
        // The scene's own slot for this came first, so the items are new here.
        connect(m_model.get(), &UsdShadeGraphModel::aboutToReset, this, [this]() {
            m_syncingSelection = true;
            m_backdropItems.clear();  // the scene deletes them with the rest
            m_nodesOnBackdrop.clear();
        });
        connect(m_model.get(), &UsdShadeGraphModel::modelReset, this, [this]() {
            m_syncingSelection = false;
            rebuildBackdropItems();
            showSelectionInGraph();
            std::vector<std::string> systems;
            for (NodeId id : m_model->allNodeIds()) {
                const std::string& system = m_model->shadeNode(id)->shadingSystem;
                if (!system.empty() && std::find(systems.begin(), systems.end(), system) == systems.end()) systems.push_back(system);
            }
            std::sort(systems.begin(), systems.end());
            m_palette->setPreferredSystems(systems);
        });
        connect(m_model.get(), &UsdShadeGraphModel::backdropsChanged, this, [this]() {
            for (ShadeBackdropItem* item : m_backdropItems) {
                for (const ShadeGraphBackdrop& backdrop : m_model->backdrops()) {
                    if (backdrop.path == item->path()) item->setShown(backdrop);
                }
            }
        });
        connect(m_model.get(), &UsdShadeGraphModel::levelLost, this, [this]() {
            // After the move's own signal, which knows where it went.
            QTimer::singleShot(0, this, [this]() {
                if (!currentStage() || m_level.IsEmpty() || currentStage()->usdStage()->GetPrimAtPath(m_level)) return;
                pickLevel();
            });
        });
        m_primsMoved = stage->primsMoved.connect([this](const PrimMoves& moves) {
            const SdfPath moved = pathAfterMoves(m_level, moves);
            if (moved != m_level) showLevel(moved);
        });
        m_findStageChanged = stage->changed.connect([this](const StageChanges&) {
            if (isFindShown() && !m_findEdit->text().trimmed().isEmpty()) m_findRefresh->start();
        });
    }
    pickLevel();
}

void MaterialEditorViewPanel::pickLevel() {
    SdfPath found;
    const StageRefPtr& stage = currentStage();
    if (stage) {
        const UsdStageRefPtr usd = stage->usdStage();
        for (const SdfPath& path : stageManager()->selectedPaths(stage)) {
            if (const UsdPrim level = shadeLevelFor(usd->GetPrimAtPath(path))) {
                found = level.GetPath();
                break;
            }
        }
        if (found.IsEmpty()) {
            for (const UsdPrim& prim : usd->Traverse()) {
                if (prim.IsA<UsdShadeMaterial>()) {
                    found = prim.GetPath();
                    break;
                }
            }
        }
    }
    showLevel(found);
}

void MaterialEditorViewPanel::showLevel(const SdfPath& level) {
    const bool changed = level != m_level;
    m_level = level;
    if (m_model) m_model->setLevel(level);
    m_stack->setCurrentWidget(level.IsEmpty() ? static_cast<QWidget*>(m_empty) : m_view);
    rebuildBreadcrumb();
    if (changed) fitGraph(kSmallestToOpenAt);
    showSelectionInGraph();
    updateSidePane();
    // Another material is another search; within this one, only the marks are drawn anew.
    if (isFindShown()) {
        const UsdPrim root = currentStage() && !level.IsEmpty() ? shadeFindRoot(currentStage()->usdStage()->GetPrimAtPath(level)) : UsdPrim();
        if ((root ? root.GetPath() : SdfPath()) != m_findRoot) {
            updateFind(false);
        } else {
            updateFindMarks();
        }
    }
}

bool MaterialEditorViewPanel::isFindShown() const { return !m_findBar->isHidden(); }

void MaterialEditorViewPanel::showFind() {
    const bool wasShown = isFindShown();
    m_findBar->setVisible(true);
    m_findEdit->setFocus();
    m_findEdit->selectAll();
    if (!wasShown) updateFind(true);
}

void MaterialEditorViewPanel::closeFind() {
    if (!isFindShown()) return;
    m_findBar->setVisible(false);
    m_findRefresh->stop();
    m_findMatches.clear();
    m_findIndex = -1;
    updateFindMarks();
    updateFindCount();
    m_view->setFocus();
}

void MaterialEditorViewPanel::updateFind(bool frame) {
    const SdfPath was = m_findIndex >= 0 ? m_findMatches[static_cast<std::size_t>(m_findIndex)].path : SdfPath();
    m_findMatches.clear();
    m_findIndex = -1;
    m_findRoot = SdfPath();
    if (isFindShown() && currentStage() && !m_level.IsEmpty()) {
        if (const UsdPrim root = shadeFindRoot(currentStage()->usdStage()->GetPrimAtPath(m_level))) {
            m_findRoot = root.GetPath();
            const auto field = static_cast<ShadeFindField>(m_findField->currentData().toInt());
            m_findMatches = findShadeNodes(root, m_findEdit->text().toStdString(), field);
        }
    }
    const auto indexWhere = [this](const std::function<bool(const ShadeFindMatch&)>& test) {
        for (std::size_t i = 0; i < m_findMatches.size(); ++i) {
            if (test(m_findMatches[i])) return static_cast<int>(i);
        }
        return -1;
    };
    if (frame && !m_findMatches.empty()) {
        // The first on the level that's shown, else the first anywhere.
        m_findIndex = std::max(0, indexWhere([this](const ShadeFindMatch& match) { return match.level == m_level; }));
        showFindMatch();
        return;
    }
    // Still on the one it was on, if that's still found, and the view stays where it is.
    if (!was.IsEmpty()) m_findIndex = indexWhere([&was](const ShadeFindMatch& match) { return match.path == was; });
    updateFindMarks();
    updateFindCount();
}

void MaterialEditorViewPanel::findNext(bool backward) {
    if (!isFindShown()) {
        showFind();
        return;
    }
    if (m_findMatches.empty()) return;
    const int count = static_cast<int>(m_findMatches.size());
    if (m_findIndex < 0) {
        m_findIndex = backward ? count - 1 : 0;
    } else {
        m_findIndex = (m_findIndex + (backward ? count - 1 : 1)) % count;
    }
    showFindMatch();
}

void MaterialEditorViewPanel::showFindMatch() {
    if (m_findIndex < 0 || m_findIndex >= static_cast<int>(m_findMatches.size())) return;
    const ShadeFindMatch match = m_findMatches[static_cast<std::size_t>(m_findIndex)];
    // Pinned or not: the pin is about the selection, and this doesn't change it.
    if (match.level != m_level) showLevel(match.level);
    updateFindMarks();
    updateFindCount();
    if (!m_model || !m_scene) return;
    for (NodeId id : m_model->nodesFor(match.path)) {
        auto* node = m_scene->nodeGraphicsObject(id);
        if (!node) continue;
        // Framed, as near the middle as the view goes, and big enough to read.
        if (m_view->transform().m11() < kSmallestToOpenAt) m_view->setTransform(QTransform::fromScale(kSmallestToOpenAt, kSmallestToOpenAt));
        m_view->centerOn(node);
        break;
    }
}

void MaterialEditorViewPanel::updateFindMarks() {
    if (!m_model) return;
    if (!isFindShown() || m_findEdit->text().trimmed().isEmpty()) {
        m_model->clearFindMarks();
    } else {
        std::vector<SdfPath> found;
        found.reserve(m_findMatches.size());
        for (const ShadeFindMatch& match : m_findMatches) found.push_back(match.path);
        m_model->setFindMarks(std::move(found), m_findIndex >= 0 ? m_findMatches[static_cast<std::size_t>(m_findIndex)].path : SdfPath());
    }
    if (!m_scene) return;
    for (NodeId id : m_model->allNodeIds()) {
        if (auto* node = m_scene->nodeGraphicsObject(id)) node->update();
    }
}

void MaterialEditorViewPanel::updateFindCount() {
    const int count = static_cast<int>(m_findMatches.size());
    if (!isFindShown() || m_findEdit->text().trimmed().isEmpty()) {
        m_findCount->clear();
    } else if (count == 0) {
        m_findCount->setText(tr("No matches"));
    } else if (m_findIndex >= 0) {
        m_findCount->setText(tr("%1 of %2").arg(m_findIndex + 1).arg(count));
    } else {
        m_findCount->setText(count == 1 ? tr("1 match") : tr("%1 matches").arg(count));
    }
}

void MaterialEditorViewPanel::setPreviewShown(bool shown) { m_previewButton->setChecked(shown); }
bool MaterialEditorViewPanel::isPreviewShown() const { return m_previewButton->isChecked(); }
void MaterialEditorViewPanel::setParametersShown(bool shown) { m_paramsButton->setChecked(shown); }
bool MaterialEditorViewPanel::areParametersShown() const { return m_paramsButton->isChecked(); }

void MaterialEditorViewPanel::updateSideVisibility() {
    const bool preview = m_previewButton->isChecked();
    const bool params = m_paramsButton->isChecked();
    const bool wasShown = !m_side->isHidden();
    m_preview->setVisible(preview);
    m_params->setVisible(params);
    m_side->setVisible(preview || params);
    if ((preview || params) && !wasShown) {
        const QList<int> sizes = m_split->sizes();
        const int palette = m_palette->isHidden() ? 0 : sizes.value(0);
        m_split->setSizes({palette, std::max(1, m_split->width() - palette - kSideWidth), kSideWidth});
    }
    fitPreview();
}

void MaterialEditorViewPanel::fitPreview() {
    // Alone, the preview fills the pane and keeps its picture square itself.
    if (m_preview->isHidden() || m_params->isHidden()) return;
    const int square = m_preview->heightForSquare(m_side->width());
    // In a short pane, the Parameters keep some room and the picture is wider than tall.
    const int room = m_side->height() - m_side->handleWidth();
    const int height = std::clamp(square, 1, std::max(1, room - usdcc::ui::scaled::pixels(kMinParametersHeight)));
    if (m_side->sizes().value(0) != height) m_side->setSizes({height, std::max(1, room - height)});
}

void MaterialEditorViewPanel::updateSidePane() {
    const StageRefPtr& stage = currentStage();
    SdfPath shown = m_level;
    SdfPath material;
    if (stage && !m_level.IsEmpty()) {
        const UsdStageRefPtr usd = stage->usdStage();
        // A node of this level that's selected; the level's own interface otherwise.
        for (const SdfPath& path : stageManager()->selectedPaths(stage)) {
            const UsdPrim prim = usd->GetPrimAtPath(path);
            if (prim && path.HasPrefix(m_level) && (prim.IsA<UsdShadeShader>() || prim.IsA<UsdShadeNodeGraph>())) {
                shown = path;
                break;
            }
        }
        for (const UsdPrim& level : shadeLevelChain(usd->GetPrimAtPath(m_level))) {
            if (level.IsA<UsdShadeMaterial>()) {
                material = level.GetPath();
                break;
            }
        }
    }
    m_params->setPrimPath(shown);
    m_preview->setMaterial(material);
    const UsdPrim node = stage && !shown.IsEmpty() ? stage->usdStage()->GetPrimAtPath(shown) : UsdPrim();
    m_preview->setSelectedNode(node && node.IsA<UsdShadeShader>() ? shown : SdfPath());
}

void MaterialEditorViewPanel::setPinned(bool pinned) { m_pin->setChecked(pinned); }
bool MaterialEditorViewPanel::isPinned() const { return m_pin->isChecked(); }
bool MaterialEditorViewPanel::isEmpty() const { return m_stack->currentWidget() == m_empty; }

void MaterialEditorViewPanel::setPaletteShown(bool shown) {
    if (m_paletteButton->isChecked() != shown) m_paletteButton->setChecked(shown);
    m_palette->setVisible(shown);
    if (shown) {
        // The graph makes room; the pane on the right keeps its width.
        const int side = m_side->isHidden() ? 0 : std::max(m_split->sizes().value(2), kSideWidth);
        m_split->setSizes({260, std::max(1, m_split->width() - 260 - side), side});
    }
}

QStringList MaterialEditorViewPanel::breadcrumb() const {
    QStringList names;
    if (!currentStage() || m_level.IsEmpty()) return names;
    for (const UsdPrim& prim : shadeLevelChain(currentStage()->usdStage()->GetPrimAtPath(m_level))) {
        names << QString::fromStdString(prim.GetName().GetString());
    }
    return names;
}

void MaterialEditorViewPanel::rebuildBreadcrumb() {
    while (QLayoutItem* item = m_breadcrumb->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            // It may be the button being clicked.
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    if (!currentStage() || m_level.IsEmpty()) return;
    const std::vector<UsdPrim> chain = shadeLevelChain(currentStage()->usdStage()->GetPrimAtPath(m_level));
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i > 0) m_breadcrumb->addWidget(new QLabel(QStringLiteral("›"), m_breadcrumb->parentWidget()));
        auto* button = new QToolButton(m_breadcrumb->parentWidget());
        button->setObjectName(QStringLiteral("materialEditorCrumb"));
        button->setText(QString::fromStdString(chain[i].GetName().GetString()));
        button->setAutoRaise(true);
        button->setToolTip(QString::fromStdString(chain[i].GetPath().GetString()));
        button->setEnabled(i + 1 < chain.size());
        const SdfPath path = chain[i].GetPath();
        connect(button, &QToolButton::clicked, this, [this, path]() { showLevel(path); });
        m_breadcrumb->addWidget(button);
    }
}

void MaterialEditorViewPanel::fitGraph(double smallest) {
    if (!m_scene) return;
    const QRectF all = m_scene->itemsBoundingRect();
    if (all.isEmpty()) return;
    m_view->fitInView(all.adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    // A short panel mustn't make it unreadable, and a small graph needs no enlarging.
    const double scale = m_view->transform().m11();
    const double wanted = qBound(smallest, scale, 1.0);
    if (wanted != scale) m_view->setTransform(QTransform::fromScale(wanted, wanted));
    m_view->centerOn(all.center());
}

void MaterialEditorViewPanel::selectionFromStage(const std::vector<SdfPath>& paths) {
    if (m_syncingSelection) return;
    if (!isPinned()) {
        const UsdStageRefPtr usd = currentStage()->usdStage();
        SdfPath target;
        bool alreadyShown = false;
        for (const SdfPath& path : paths) {
            const UsdPrim prim = usd->GetPrimAtPath(path);
            const UsdPrim level = shadeLevelFor(prim);
            if (!level) continue;
            // It's here, or it's this level or one around it, which entering doesn't undo.
            if (level.GetPath() == m_level || (!m_level.IsEmpty() && m_level.HasPrefix(path))) {
                alreadyShown = true;
                break;
            }
            if (target.IsEmpty()) target = level.GetPath();
        }
        if (!alreadyShown && !target.IsEmpty()) {
            showLevel(target);
            return;  // which shows the selection too
        }
    }
    showSelectionInGraph();
}

void MaterialEditorViewPanel::showSelectionInGraph() {
    if (!m_scene || !m_model || !currentStage() || m_syncingSelection) return;
    m_syncingSelection = true;
    m_scene->clearSelection();
    QGraphicsItem* last = nullptr;
    for (const SdfPath& path : stageManager()->selectedPaths(currentStage())) {
        for (NodeId id : m_model->nodesFor(path)) {
            if (auto* node = m_scene->nodeGraphicsObject(id)) {
                node->setSelected(true);
                last = node;
            }
        }
        for (ShadeBackdropItem* item : m_backdropItems) {
            if (item->path() == path) item->setSelected(true);
        }
    }
    if (last) m_view->ensureVisible(last);
    m_syncingSelection = false;
}

void MaterialEditorViewPanel::selectionToStage() {
    if (m_syncingSelection || !m_scene || !m_model) return;
    std::vector<SdfPath> paths;
    for (QtNodes::NodeGraphicsObject* node : m_scene->selectedNodes()) {
        const ShadeGraphNode* shade = m_model->shadeNode(node->nodeId());
        if (shade && std::find(paths.begin(), paths.end(), shade->path) == paths.end()) paths.push_back(shade->path);
    }
    for (ShadeBackdropItem* item : m_backdropItems) {
        if (item->isSelected()) paths.push_back(item->path());
    }
    m_syncingSelection = true;
    stageManager()->setSelectedPaths(currentStage(), paths);
    m_syncingSelection = false;
}

std::vector<SdfPath> MaterialEditorViewPanel::selectedPrims() const {
    std::vector<SdfPath> paths;
    if (!m_scene || !m_model) return paths;
    for (QtNodes::NodeGraphicsObject* node : m_scene->selectedNodes()) {
        const ShadeGraphNode* shade = m_model->shadeNode(node->nodeId());
        if (shade && (shade->kind == ShadeNodeKind::Shader || shade->kind == ShadeNodeKind::NodeGraph)) paths.push_back(shade->path);
    }
    return paths;
}

void MaterialEditorViewPanel::selectAfterEdit(const std::vector<SdfPath>& paths) {
    if (!paths.empty()) stageManager()->setSelectedPaths(currentStage(), paths);
}

QPointF MaterialEditorViewPanel::mouseScenePosition() const {
    const QPoint inside = m_view->viewport()->mapFromGlobal(QCursor::pos());
    const QPoint at = m_view->viewport()->rect().contains(inside) ? inside : m_view->viewport()->rect().center();
    return m_view->mapToScene(at);
}

SdfPath MaterialEditorViewPanel::addNode(const QString& identifier, QPointF scenePosition) {
    if (!currentStage() || m_level.IsEmpty()) return SdfPath();
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    const SdfPath path = shade_actions::createShader(*m_undo, currentStage(), m_level, identifier.toStdString(), stored(scenePosition));
    if (!path.IsEmpty()) selectAfterEdit({path});
    return path;
}

SdfPath MaterialEditorViewPanel::addNodeAtMouse(const QString& identifier) { return addNode(identifier, mouseScenePosition()); }

bool MaterialEditorViewPanel::deleteSelected() {
    if (!m_scene || !m_model) return false;
    // Selected lines, when there's no node going with them.
    std::vector<QtNodes::ConnectionId> lines;
    for (QGraphicsItem* item : m_scene->selectedItems()) {
        if (auto* line = dynamic_cast<QtNodes::ConnectionGraphicsObject*>(item)) lines.push_back(line->connectionId());
    }
    std::vector<SdfPath> paths = selectedPrims();
    for (ShadeBackdropItem* item : m_backdropItems) {
        if (item->isSelected()) paths.push_back(item->path());
    }
    if (paths.empty()) {
        bool any = false;
        for (const QtNodes::ConnectionId& line : lines) any = m_model->deleteConnection(line) || any;
        return any;
    }
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    const bool done = shade_actions::deleteNodes(*m_undo, currentStage(), paths);
    if (done) stageManager()->setSelectedPaths(currentStage(), {});
    return done;
}

void MaterialEditorViewPanel::duplicateSelected() {
    const std::vector<SdfPath> paths = selectedPrims();
    if (paths.empty()) return;
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    selectAfterEdit(shade_actions::duplicate(*m_undo, currentStage(), paths, GfVec2f(kPasteOffset, kPasteOffset)));
}

void MaterialEditorViewPanel::copySelected() {
    const std::vector<SdfPath> paths = selectedPrims();
    if (paths.empty()) return;
    const std::string text = shade_actions::copyToText(currentStage(), paths);
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(kClipboardMime), QByteArray::fromStdString(text));
    data->setText(QString::fromStdString(text));
    QApplication::clipboard()->setMimeData(data);
}

void MaterialEditorViewPanel::cutSelected() {
    copySelected();
    deleteSelected();
}

void MaterialEditorViewPanel::paste() {
    const QMimeData* data = QApplication::clipboard()->mimeData();
    if (!data || !data->hasFormat(QString::fromLatin1(kClipboardMime)) || m_level.IsEmpty()) return;
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    selectAfterEdit(shade_actions::pasteFromText(*m_undo, currentStage(), m_level, data->data(QString::fromLatin1(kClipboardMime)).toStdString(),
                                                 GfVec2f(kPasteOffset, kPasteOffset)));
}

void MaterialEditorViewPanel::groupSelected() {
    const std::vector<SdfPath> paths = selectedPrims();
    if (paths.empty()) return;
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    const SdfPath graph = shade_actions::group(*m_undo, currentStage(), paths);
    if (!graph.IsEmpty()) selectAfterEdit({graph});
}

void MaterialEditorViewPanel::ungroupSelected() {
    if (!currentStage()) return;
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    std::vector<SdfPath> all;
    for (const SdfPath& path : selectedPrims()) {
        const UsdPrim prim = currentStage()->usdStage()->GetPrimAtPath(path);
        if (!prim || !prim.IsA<UsdShadeNodeGraph>()) continue;
        const std::vector<SdfPath> out = shade_actions::ungroup(*m_undo, currentStage(), path);
        all.insert(all.end(), out.begin(), out.end());
    }
    selectAfterEdit(all);
}

void MaterialEditorViewPanel::autoLayout() {
    if (!m_model) return;
    m_model->autoLayout();
    QTimer::singleShot(0, this, [this]() { fitGraph(kSmallestToOpenAt); });
}

void MaterialEditorViewPanel::layoutSelected() {
    if (!m_model || !m_scene) return;
    std::vector<NodeId> ids;
    for (QtNodes::NodeGraphicsObject* node : m_scene->selectedNodes()) ids.push_back(node->nodeId());
    // The view stays: they're laid out where they are.
    m_model->layoutNodes(ids);
}

SdfPath MaterialEditorViewPanel::addBackdrop(QPointF scenePosition) {
    if (!currentStage() || m_level.IsEmpty()) return SdfPath();
    // Around what's selected, if anything is.
    QRectF around;
    if (m_scene) {
        for (QtNodes::NodeGraphicsObject* node : m_scene->selectedNodes()) around = around.united(node->sceneBoundingRect());
    }
    QPointF at = scenePosition;
    QSizeF size(400, 300);
    if (!around.isEmpty()) {
        around.adjust(-30, -50, 30, 30);
        at = around.topLeft();
        size = around.size();
    }
    const UndoManager::SelectionScope selection(*m_undo, currentStage());  // the selection it makes is part of the step
    const SdfPath path = shade_actions::createBackdrop(*m_undo, currentStage(), m_level, stored(at), GfVec2f(float(size.width()) / kShadeNodePixels, float(size.height()) / kShadeNodePixels));
    if (!path.IsEmpty()) selectAfterEdit({path});
    return path;
}

void MaterialEditorViewPanel::rebuildBackdropItems() {
    if (!m_scene || !m_model) return;
    for (const ShadeGraphBackdrop& backdrop : m_model->backdrops()) {
        auto* item = new ShadeBackdropItem(backdrop);
        m_scene->addItem(item);
        m_backdropItems.push_back(item);
        connect(item, &ShadeBackdropItem::pressed, this, [this](ShadeBackdropItem* pressed) {
            // What's on it now goes with it, not what it moves over.
            m_nodesOnBackdrop.clear();
            const QRectF area = pressed->sceneRectOf();
            for (NodeId id : m_model->allNodeIds()) {
                const auto* node = m_scene->nodeGraphicsObject(id);
                if (node && area.contains(node->sceneBoundingRect().center())) m_nodesOnBackdrop.push_back(id);
            }
        });
        connect(item, &ShadeBackdropItem::movedBy, this, [this](ShadeBackdropItem* moved, QPointF delta) {
            m_model->setBackdropRect(moved->path(), moved->sceneRectOf());
            for (NodeId id : m_nodesOnBackdrop) {
                m_model->setNodeData(id, QtNodes::NodeRole::Position, m_model->nodeData(id, QtNodes::NodeRole::Position).toPointF() + delta);
            }
        });
        connect(item, &ShadeBackdropItem::resizedTo, this, [this](ShadeBackdropItem* moved, QSizeF) { m_model->setBackdropRect(moved->path(), moved->sceneRectOf()); });
    }
}

void MaterialEditorViewPanel::startInlineEdit(const QString& text, const QRectF& sceneRect, const std::function<void(const QString&)>& commit) {
    delete m_inlineEdit;
    m_inlineEdit = new QLineEdit(text, m_view->viewport());
    m_inlineEdit->setObjectName(QStringLiteral("materialEditorInlineEdit"));
    const QRect rect = m_view->mapFromScene(sceneRect).boundingRect();
    m_inlineEdit->setGeometry(rect.x(), rect.y(), std::max(rect.width(), 120), m_inlineEdit->sizeHint().height());
    m_inlineEdit->selectAll();
    m_inlineEdit->installEventFilter(this);
    m_inlineEdit->show();
    m_inlineEdit->setFocus();
    connect(m_inlineEdit, &QLineEdit::returnPressed, this, [this, commit]() {
        const QString value = m_inlineEdit->text();
        m_inlineEdit->hide();
        m_inlineEdit->deleteLater();
        m_inlineEdit = nullptr;
        m_view->setFocus();
        commit(value);
    });
}

void MaterialEditorViewPanel::startRename() {
    if (!m_scene || !m_model) return;
    for (ShadeBackdropItem* item : m_backdropItems) {
        if (!item->isSelected()) continue;
        std::string current;
        for (const ShadeGraphBackdrop& backdrop : m_model->backdrops()) {
            if (backdrop.path == item->path()) current = backdrop.description;
        }
        startInlineEdit(QString::fromStdString(current), item->sceneRectOf(), [this, path = item->path()](const QString& value) {
            shade_actions::setBackdropDescription(*m_undo, currentStage(), path, value.toStdString());
        });
        return;
    }
    for (QtNodes::NodeGraphicsObject* node : m_scene->selectedNodes()) {
        const ShadeGraphNode* shade = m_model->shadeNode(node->nodeId());
        if (!shade || (shade->kind != ShadeNodeKind::Shader && shade->kind != ShadeNodeKind::NodeGraph)) continue;
        const UsdPrim prim = currentStage()->usdStage()->GetPrimAtPath(shade->path);
        if (!prim || !prim_actions::canRename(prim)) return;
        startInlineEdit(QString::fromStdString(shade->caption), node->sceneBoundingRect(), [this, path = shade->path](const QString& value) {
            prim_actions::rename(*m_undo, currentStage(), path, value.toStdString(), stageManager()->stages());
        });
        return;
    }
}

ShaderNodeSearchPopup* MaterialEditorViewPanel::showNodeSearch() {
    if (m_level.IsEmpty()) return nullptr;
    const QPointF at = mouseScenePosition();
    auto* popup = new ShaderNodeSearchPopup(this);
    std::vector<std::string> systems;
    for (NodeId id : m_model->allNodeIds()) {
        const std::string& system = m_model->shadeNode(id)->shadingSystem;
        if (!system.empty() && std::find(systems.begin(), systems.end(), system) == systems.end()) systems.push_back(system);
    }
    std::sort(systems.begin(), systems.end());
    popup->palette()->setPreferredSystems(systems);
    connect(popup, &ShaderNodeSearchPopup::nodeChosen, this, [this, at](const QString& identifier) { addNode(identifier, at); });
    QPoint global = m_view->viewport()->mapToGlobal(m_view->mapFromScene(at));
    popup->move(global);
    popup->show();
    popup->palette()->search()->setFocus();
    return popup;
}

void MaterialEditorViewPanel::populateGraphMenu(QMenu* menu, QPointF scenePosition) {
    if (!m_model || !m_scene || m_level.IsEmpty()) return;
    const QPoint viewportPoint = m_view->mapFromScene(scenePosition);
    auto* node = nodeAt(m_view, viewportPoint);
    auto* backdrop = node ? nullptr : backdropAt(m_view, viewportPoint);
    const bool editable = m_model->isEditable();

    if (backdrop) {
        if (!backdrop->isSelected()) {
            m_scene->clearSelection();
            backdrop->setSelected(true);
        }
        menu->addAction(tr("Describe..."), this, [this]() { startRename(); })->setEnabled(editable);
        menu->addAction(tr("Delete"), this, [this]() { deleteSelected(); })->setEnabled(editable);
        return;
    }
    if (!node) {
        menu->addAction(tr("Add Node..."), this, [this]() { showNodeSearch(); })->setEnabled(editable);
        menu->addAction(tr("Add Backdrop"), this, [this, scenePosition]() { addBackdrop(scenePosition); })->setEnabled(editable);
        QAction* pasteAction = menu->addAction(tr("Paste"), this, [this]() { paste(); });
        const QMimeData* data = QApplication::clipboard()->mimeData();
        pasteAction->setEnabled(editable && data && data->hasFormat(QString::fromLatin1(kClipboardMime)));
        menu->addSeparator();
        menu->addAction(tr("Auto Layout"), this, [this]() { autoLayout(); })->setEnabled(editable);
        menu->addAction(tr("Layout Selected"), this, [this]() { layoutSelected(); })->setEnabled(editable && !m_scene->selectedNodes().empty());
        menu->addAction(tr("Frame All"), this, [this]() { fitGraph(kSmallestToFrame); });
        menu->addAction(tr("Find..."), QKeySequence(QKeySequence::Find), this, [this]() { showFind(); });
        return;
    }

    const NodeId id = node->nodeId();
    const ShadeGraphNode shade = *m_model->shadeNode(id);
    if (!node->isSelected()) {
        m_scene->clearSelection();
        node->setSelected(true);
    }

    // On a port's row: what's for that port.
    const auto& geometry = static_cast<const ShaderNodeGeometry&>(m_scene->nodeGeometry());
    if (const std::optional<PortIndex> input = geometry.inputAt(id, node->mapFromScene(scenePosition)); input && *input < shade.in.size()) {
        const ShadeGraphPort& port = shade.in[*input];
        const std::optional<shade_actions::PortRef> ref = m_model->portRef(id, PortType::In, *input);
        if (port.connected) {
            menu->addAction(tr("Disconnect %1").arg(QString::fromStdString(port.label)), this, [this, ref]() {
                prim_actions::disconnectSource(*m_undo, currentStage(), ref->prim.AppendProperty(ref->attribute));
            })->setEnabled(editable);
        } else if (shade.kind == ShadeNodeKind::Shader || shade.kind == ShadeNodeKind::NodeGraph) {
            menu->addAction(tr("Promote %1 to the Interface").arg(QString::fromStdString(port.label)), this, [this, ref]() {
                shade_actions::promote(*m_undo, currentStage(), *ref, m_level);
            })->setEnabled(editable);
        }
        if (shade.kind == ShadeNodeKind::Outputs && port.authored) {
            menu->addAction(tr("Remove Terminal %1").arg(QString::fromStdString(port.label)), this, [this, ref]() {
                shade_actions::removeTerminal(*m_undo, currentStage(), ref->prim, ref->attribute);
            })->setEnabled(editable);
        }
        menu->addSeparator();
    }

    // A new input of the Material's or a NodeGraph's interface.
    const UsdPrim shadePrim = currentStage()->usdStage()->GetPrimAtPath(shade.path);
    const auto addInputAction = [&]() {
        menu->addAction(tr("Add Input..."), this, [this, path = shade.path]() {
            addShadeInputInteractively(*m_undo, currentStage(), path, this);
        })->setEnabled(editable && prim_actions::isEditable(shadePrim));
    };
    if (shade.kind == ShadeNodeKind::Inputs) {
        addInputAction();
        return;
    }

    if (shade.kind == ShadeNodeKind::Outputs && shadePrim.IsA<UsdShadeMaterial>()) {
        addInputAction();
        QMenu* terminals = menu->addMenu(tr("Add Terminal"));
        terminals->setEnabled(editable);
        for (const RendererProfile& profile : ShaderNodeCatalog::instance().profiles()) {
            for (const std::string& terminal : profile.terminals) {
                const std::string name = profile.terminalPrefix.empty() ? terminal : profile.terminalPrefix + ":" + terminal;
                const bool taken = std::any_of(shade.in.begin(), shade.in.end(), [&name](const ShadeGraphPort& port) { return port.authored && port.attribute.GetString() == "outputs:" + name; });
                if (taken) continue;
                terminals->addAction(QString::fromStdString(name), this, [this, path = shade.path, name]() {
                    shade_actions::addTerminal(*m_undo, currentStage(), path, name);
                });
            }
        }
        return;
    }
    if (shade.kind != ShadeNodeKind::Shader && shade.kind != ShadeNodeKind::NodeGraph) return;
    if (shade.kind == ShadeNodeKind::NodeGraph) {
        menu->addAction(tr("Enter"), this, [this, path = shade.path]() { showLevel(path); });
        menu->addAction(tr("Ungroup"), this, [this]() { ungroupSelected(); })->setEnabled(editable);
        addInputAction();
    }
    menu->addAction(tr("Group into NodeGraph"), this, [this]() { groupSelected(); })->setEnabled(editable);
    menu->addAction(tr("Layout Selected"), this, [this]() { layoutSelected(); })->setEnabled(editable);
    menu->addSeparator();
    menu->addAction(tr("Rename"), this, [this]() { startRename(); })->setEnabled(editable);
    menu->addAction(tr("Duplicate"), this, [this]() { duplicateSelected(); })->setEnabled(editable);
    menu->addAction(tr("Copy"), this, [this]() { copySelected(); });
    menu->addAction(tr("Cut"), this, [this]() { cutSelected(); })->setEnabled(editable);
    menu->addAction(tr("Delete"), this, [this]() { deleteSelected(); })->setEnabled(editable);
}

bool MaterialEditorViewPanel::pressOnPage(QPoint viewportPoint) {
    if (!m_scene || !m_model) return false;
    auto* node = nodeAt(m_view, viewportPoint);
    if (!node) return false;
    const auto& geometry = static_cast<const ShaderNodeGeometry&>(m_scene->nodeGeometry());
    const std::optional<std::string> page = geometry.pageAt(node->nodeId(), node->mapFromScene(m_view->mapToScene(viewportPoint)));
    if (!page) return false;
    // Not from inside the node's own event: opening a page replaces it.
    QTimer::singleShot(0, this, [this, id = node->nodeId(), name = *page]() {
        if (m_model) m_model->togglePage(id, name);
    });
    return true;
}

bool MaterialEditorViewPanel::handleKey(QKeyEvent* event, bool run) {
    const auto when = [run](const std::function<void()>& action) {
        if (run) action();
        return true;
    };
    const Qt::KeyboardModifiers modifiers = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    const int key = event->key();
    if (modifiers == Qt::NoModifier) {
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) return when([this]() { deleteSelected(); });
        if (key == Qt::Key_F2) return when([this]() { startRename(); });
        if (key == Qt::Key_Tab) return when([this]() { showNodeSearch(); });
        if (key == Qt::Key_L) return when([this]() { autoLayout(); });
        if (key == Qt::Key_F3 && isFindShown()) return when([this]() { findNext(false); });
        if (key == Qt::Key_Escape && isFindShown()) return when([this]() { closeFind(); });
    }
    if (modifiers == Qt::ShiftModifier && key == Qt::Key_F3 && isFindShown()) return when([this]() { findNext(true); });
    if (modifiers == Qt::ControlModifier) {
        if (key == Qt::Key_F) return when([this]() { showFind(); });
        if (key == Qt::Key_D) return when([this]() { duplicateSelected(); });
        if (key == Qt::Key_C) return when([this]() { copySelected(); });
        if (key == Qt::Key_X) return when([this]() { cutSelected(); });
        if (key == Qt::Key_V) return when([this]() { paste(); });
        if (key == Qt::Key_G) return when([this]() { groupSelected(); });
    }
    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_G) return when([this]() { ungroupSelected(); });
    if (modifiers == Qt::ShiftModifier && key == Qt::Key_L) return when([this]() { layoutSelected(); });
    return false;
}

// Shift adds what's clicked to the selection and Ctrl takes it out, where Qt's own Ctrl
// toggles. A Ctrl click on a node goes no further, so it neither drags nor selects it.
bool MaterialEditorViewPanel::clickWithSelectionKeys(QMouseEvent* mouse) {
    m_keepSelectionOnRelease = false;
    if (!m_scene || mouse->button() != Qt::LeftButton) return false;
    const QPoint at = mouse->position().toPoint();
    QGraphicsItem* item = nodeAt(m_view, at);
    if (!item) item = backdropAt(m_view, at);
    const Qt::KeyboardModifiers modifiers = mouse->modifiers();
    if (modifiers & Qt::ControlModifier) {
        if (!item) return false;
        item->setSelected(false);
        m_swallowingClick = true;
        return true;
    }
    if (!(modifiers & Qt::ShiftModifier)) return false;
    if (item) {
        item->setSelected(true);
        m_keepSelectionOnRelease = true;
    } else {
        // Qt's Ctrl: the click keeps the selection, and Shift's rubber band adds to it.
        mouse->setModifiers(modifiers | Qt::ControlModifier);
    }
    return false;
}

bool MaterialEditorViewPanel::handleViewportEvent(QEvent* event) {
    if (m_swallowingClick && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) m_swallowingClick = false;
        return true;
    }
    if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) && clickWithSelectionKeys(static_cast<QMouseEvent*>(event))) {
        return true;
    }
    switch (event->type()) {
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            if (pressOnPage(mouse->position().toPoint())) return true;
            if (m_model) m_model->beginInteraction();
            m_pressedNode = 0;
            if (auto* node = nodeAt(m_view, mouse->position().toPoint())) m_pressedNode = node->nodeId();
            m_shake.reset();
            m_shakeClock.restart();
            return false;
        }
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (!m_model || !m_model->isInteracting() || !m_pressedNode || !(mouse->buttons() & Qt::LeftButton)) return false;
            // Shaking a node off its connections, as one undo step with its move.
            if (m_shake.addMove(mouse->position().x(), static_cast<double>(m_shakeClock.elapsed()))) {
                for (const QtNodes::ConnectionId& id : m_model->allConnectionIds(m_pressedNode)) m_model->deleteConnection(id);
            }
            return false;
        }
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            // After QtNodes has seen it, which is where a line lands.
            QTimer::singleShot(0, this, [this]() {
                if (m_model) m_model->endInteraction();
            });
            return false;
        }
        case QEvent::MouseButtonDblClick: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (auto* backdrop = backdropAt(m_view, mouse->position().toPoint())) {
                m_scene->clearSelection();
                backdrop->setSelected(true);
                startRename();
                return true;
            }
            return false;
        }
        case QEvent::ContextMenu: {
            const auto* context = static_cast<QContextMenuEvent*>(event);
            QMenu menu(this);
            populateGraphMenu(&menu, m_view->mapToScene(context->pos()));
            if (!menu.isEmpty()) menu.exec(context->globalPos());
            return true;
        }
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* drag = static_cast<QDragMoveEvent*>(event);
            if (!drag->mimeData()->hasFormat(QString::fromLatin1(ShaderNodePalette::kMimeType))) return false;
            drag->acceptProposedAction();
            return true;
        }
        case QEvent::Drop: {
            auto* drop = static_cast<QDropEvent*>(event);
            if (!drop->mimeData()->hasFormat(QString::fromLatin1(ShaderNodePalette::kMimeType))) return false;
            addNode(QString::fromUtf8(drop->mimeData()->data(QString::fromLatin1(ShaderNodePalette::kMimeType))), m_view->mapToScene(drop->position().toPoint()));
            drop->acceptProposedAction();
            return true;
        }
        default:
            return false;
    }
}

bool MaterialEditorViewPanel::eventFilter(QObject* watched, QEvent* event) {
    // Each time it's shown QtNodes' view fits the graph itself, however small that makes it.
    // After it, and after the dock has given it its size.
    if (watched == m_view && event->type() == QEvent::Show) {
        QTimer::singleShot(0, this, [this]() { fitGraph(kSmallestToOpenAt); });
    }
    if (watched == m_side && event->type() == QEvent::Resize) fitPreview();
    if (watched == m_view->viewport() && handleViewportEvent(event)) return true;
    if (m_scene && watched == m_scene.get() && event->type() == QEvent::GraphicsSceneMouseRelease && m_keepSelectionOnRelease) {
        auto* mouse = static_cast<QGraphicsSceneMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_keepSelectionOnRelease = false;
            // A click that didn't move leaves Qt's item the only one selected, or with its
            // Ctrl, toggled: so that, with it unselected for a moment the stage doesn't see.
            QGraphicsItem* item = m_scene->mouseGrabberItem();
            if (item && item->isSelected() && mouse->scenePos() == mouse->buttonDownScenePos(Qt::LeftButton)) {
                m_syncingSelection = true;
                item->setSelected(false);
                m_syncingSelection = false;
                mouse->setModifiers(mouse->modifiers() | Qt::ControlModifier);
            }
        }
    }
    if (watched == m_view && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Before anything else that wants them, such as QtNodes' own Delete.
        if (handleKey(key, event->type() == QEvent::KeyPress)) {
            event->accept();
            return true;
        }
    }
    if (m_inlineEdit && watched == m_inlineEdit && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_inlineEdit->hide();
        m_inlineEdit->deleteLater();
        m_inlineEdit = nullptr;
        m_view->setFocus();
        return true;
    }
    return ViewPanel::eventFilter(watched, event);
}

}  // namespace usdcc::usd
