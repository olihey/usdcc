#include "usdcc/usd/attributes_view_panel.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/prim_picker_dialog.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/properties/attribute_binding.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/properties/transform_stack_editor.h"
#include "usdcc/usd/property_categorizer.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QFileInfo>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace usdcc::usd {

using namespace PXR_NS;
using properties::AttributeBinding;
using properties::CategorySection;
using properties::EditorHints;
using properties::PropertyEditor;
using properties::PropertyRow;

namespace {

constexpr int kLabelWidth = 130;
constexpr auto kCollapsedKeyPrefix = "AttributeEditor/collapsed/";
constexpr auto kAuthoredOnlyKey = "AttributeEditor/authoredOnly";
constexpr auto kShowHiddenKey = "AttributeEditor/showHidden";

QString qs(const std::string& s) { return QString::fromStdString(s); }

// The layers other than the edit target that hold an opinion for `attr`,
// strongest first — what keeps an attribute alive that removing its op could
// not delete (see XformStack::remove()).
std::vector<SdfLayerHandle> otherDefiningLayers(const UsdAttribute& attr) {
    const SdfLayerHandle target = attr.GetStage()->GetEditTarget().GetLayer();
    std::vector<SdfLayerHandle> layers;
    for (const SdfPropertySpecHandle& spec : attr.GetPropertyStack()) {
        const SdfLayerHandle layer = spec ? spec->GetLayer() : SdfLayerHandle();
        if (layer && layer != target && std::find(layers.begin(), layers.end(), layer) == layers.end()) {
            layers.push_back(layer);
        }
    }
    return layers;
}

PropertyEditor* readOnlyText(const QString& text) {
    PropertyEditor* editor = properties::createPropertyEditor(SdfValueTypeNames->String);
    editor->setReadOnly(true);
    editor->setValue(VtValue(text.toStdString()));
    return editor;
}

QString targetsText(const UsdRelationship& rel) {
    SdfPathVector targets;
    rel.GetTargets(&targets);
    if (targets.empty()) return QObject::tr("(no targets)");
    QStringList parts;
    for (const SdfPath& t : targets) parts << qs(t.GetString());
    return parts.join(QStringLiteral(", "));
}

const char* specifierName(SdfSpecifier specifier) {
    switch (specifier) {
        case SdfSpecifierDef:
            return "def";
        case SdfSpecifierOver:
            return "over";
        case SdfSpecifierClass:
            return "class";
        default:
            return "";
    }
}

}  // namespace

AttributesViewPanel::AttributesViewPanel(StageManager* stageManager, StageRefPtr stage,
                                          std::shared_ptr<UndoManager> undoManager, const QString& title,
                                          QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);

    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Search attributes"));
    m_search->setClearButtonEnabled(true);

    m_authoredOnly = new QAction(tr("Authored Only"), this);
    m_authoredOnly->setCheckable(true);
    m_showHidden = new QAction(tr("Hidden Properties"), this);
    m_showHidden->setCheckable(true);
    {
        QSettings settings = usdcc::ui::appSettings();
        m_authoredOnly->setChecked(settings.value(kAuthoredOnlyKey, false).toBool());
        m_showHidden->setChecked(settings.value(kShowHiddenKey, false).toBool());
    }

    addPanelControls(toolbarLayout);
    toolbarLayout->addWidget(m_search, 1);

    m_scroll = new QScrollArea(container);
    m_scroll->setObjectName(QStringLiteral("attributeScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);

    layout->addWidget(toolbar);
    layout->addWidget(m_scroll, 1);
    setContentWidget(container);

    connect(m_search, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    for (QAction* action : {m_authoredOnly, m_showHidden}) {
        connect(action, &QAction::toggled, this, [this]() {
            QSettings settings = usdcc::ui::appSettings();
            settings.setValue(kAuthoredOnlyKey, m_authoredOnly->isChecked());
            settings.setValue(kShowHiddenKey, m_showHidden->isChecked());
            rebuild();
        });
    }

    m_selectionChangedConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr changedStage, std::vector<PXR_NS::SdfPath> paths) {
            onStageSelectionChanged(changedStage, std::move(paths));
        });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* AttributesViewPanel::duplicate(QWidget* parent) const {
    return new AttributesViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

void AttributesViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(m_authoredOnly);
    menu->addAction(m_showHidden);
}

void AttributesViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    m_editTargetConnection = {};
    if (stage) {
        m_stageChangedConnection =
            stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
        // Queued: the target can change from inside a menu or another panel's handler.
        m_editTargetConnection = stage->editTargetChanged.connect(
            [this]() { QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection); });
    }
    // Always rebuild: the new stage may have the same prim path selected, and
    // the old rows are bound to the old stage.
    const std::vector<SdfPath> paths = stageManager()->selectedPaths(stage);
    m_primPath = paths.empty() ? SdfPath() : paths.front();
    rebuild();
}

void AttributesViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage != currentStage()) {
        return;
    }
    const SdfPath path = paths.empty() ? SdfPath() : paths.front();
    if (path == m_primPath && m_content) {
        return;
    }
    m_primPath = path;
    rebuild();
}

UsdPrim AttributesViewPanel::currentPrim() const {
    return m_primPath.IsEmpty() || !currentStage() ? UsdPrim()
                                                   : currentStage()->usdStage()->GetPrimAtPath(m_primPath);
}

std::string AttributesViewPanel::layoutSignature(const UsdPrim& prim) const {
    if (!prim) return {};
    std::string signature = prim.GetTypeName().GetString();
    signature += prim.IsActive() ? "|a" : "|i";
    // The op order too: moving an inverse op changes no property, only the order.
    if (const UsdGeomXformable xformable{prim}) {
        bool resets = false;
        signature += "|ops:";
        for (const UsdGeomXformOp& op : xformable.GetOrderedXformOps(&resets)) {
            signature += op.GetOpName().GetString() + ",";
        }
    }
    // The arcs too: a changed reference, payload or variant changes the
    // Composition section's rows.
    signature += "|arcs:";
    for (const ArcInfo& arc : compositionArcs(prim)) {
        signature += std::to_string(static_cast<int>(arc.type)) + arc.assetPath + "@" + arc.primPath.GetString() + "@" +
                     std::to_string(arc.layerOffset.GetOffset()) + "x" + std::to_string(arc.layerOffset.GetScale()) +
                     (arc.loaded ? "L" : "U") + arc.variantSet + "=" + arc.variantSelection + arc.layerIdentifier +
                     (arc.editable ? "e" : "r") + ",";
    }
    const CategorizeOptions options{m_authoredOnly->isChecked(), m_showHidden->isChecked()};
    for (const PropertyCategory& category : categorizeProperties(prim, options)) {
        signature += "|" + category.key + ":";
        for (const UsdProperty& property : category.properties) {
            signature += property.GetName().GetString() + ",";
        }
    }
    return signature;
}

CategorySection* AttributesViewPanel::makeSection(const QString& key, const QString& title, bool collapsedByDefault) {
    auto* section = new CategorySection(key, title);
    QSettings settings = usdcc::ui::appSettings();
    section->setCollapsed(settings.value(kCollapsedKeyPrefix + key, collapsedByDefault).toBool());
    connect(section, &CategorySection::collapsedChanged, this, [key](bool collapsed) {
        QSettings s = usdcc::ui::appSettings();
        s.setValue(kCollapsedKeyPrefix + key, collapsed);
    });
    return section;
}

void AttributesViewPanel::rebuild() {
    const int scrollPosition = m_scroll->verticalScrollBar()->value();
    if (QWidget* old = m_scroll->takeWidget()) {
        // Deferred: a rebuild can be triggered from inside one of the old
        // widgets' own signal handlers.
        old->deleteLater();
    }
    m_rows.clear();
    m_sections.clear();
    m_primSection = nullptr;
    m_compositionSection = nullptr;
    m_xformEditor = nullptr;
    m_header = {};
    m_rebuildPending = false;
    m_refreshPending = false;

    m_content = new QWidget;
    m_content->setObjectName(QStringLiteral("attributeContent"));
    auto* layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    const UsdPrim prim = currentPrim();
    m_signature = layoutSignature(prim);
    if (!prim) {
        const QString note = !currentStage()       ? tr("No stage open.")
                             : m_primPath.IsEmpty() ? tr("Select a prim to see its attributes.")
                                                    : tr("%1 no longer exists.").arg(qs(m_primPath.GetString()));
        auto* empty = new QLabel(note, m_content);
        empty->setObjectName(QStringLiteral("propertyNote"));
        empty->setAlignment(Qt::AlignCenter);
        usdcc::ui::scaled::margins(empty, 0, 24, 0, 0);
        layout->addWidget(empty);
        layout->addStretch(1);
        m_scroll->setWidget(m_content);
        return;
    }

    buildPrimSection(prim, layout);
    buildCompositionSection(prim, layout);

    const CategorizeOptions options{m_authoredOnly->isChecked(), m_showHidden->isChecked()};
    std::vector<PropertyCategory> categories = categorizeProperties(prim, options);
    // Every xformable prim gets the Transform section, even with nothing to
    // list (Authored Only, no ops yet): it is where ops are added.
    const bool xformable = static_cast<bool>(UsdGeomXformable(prim));
    if (xformable && (categories.empty() || categories.front().key != "transform")) {
        categories.insert(categories.begin(), PropertyCategory{"transform", "Transform", {}});
    }
    for (const PropertyCategory& category : categories) {
        CategorySection* section = makeSection(qs(category.key), qs(category.label));
        std::set<TfToken> inStack;
        if (category.key == "transform" && xformable) {
            buildTransformStack(prim, section, inStack);
        }
        for (const UsdProperty& property : category.properties) {
            if (inStack.count(property.GetName()) || property.GetName() == UsdGeomTokens->xformOpOrder) {
                continue;  // shown (and edited) by the transform stack
            }
            Row row = buildRow(property, category);
            row.section = section;
            if (category.key == "transform" && UsdGeomXformOp::IsXformOp(property.GetName())) {
                setUpUnusedOp(row);
            }
            section->addRow(row.row);
            m_rows.push_back(row);
        }
        layout->addWidget(section);
        m_sections.push_back(section);
    }
    layout->addStretch(1);
    m_scroll->setWidget(m_content);

    applyFilter();
    // The scroll range is only known once the new content has been laid out.
    QTimer::singleShot(0, this, [this, scrollPosition]() { m_scroll->verticalScrollBar()->setValue(scrollPosition); });
}

void AttributesViewPanel::buildTransformStack(const UsdPrim& prim, CategorySection* section, std::set<TfToken>& inStack) {
    m_xformEditor = new properties::TransformStackEditor(section, prim, currentStage(), m_undoManager.get(), section);
    for (const auto& opRow : m_xformEditor->rows()) {
        Row row;
        row.row = opRow.row;
        row.binding = opRow.binding;
        row.editor = opRow.row->editor();
        row.section = section;
        row.property = prim.GetAttribute(opRow.attrName);
        row.searchText = (opRow.row->labelText() + QLatin1Char(' ') + qs(opRow.opName.GetString())).toLower();
        row.managedByStack = true;
        if (row.binding) {
            connect(row.binding, &AttributeBinding::editCommitted, this, [this]() { queueProcessPendingChanges(); });
        }
        m_rows.push_back(row);
        inStack.insert(opRow.attrName);
    }
}

void AttributesViewPanel::setUpUnusedOp(const Row& row) {
    const UsdAttribute attr = row.property.As<UsdAttribute>();
    if (!attr) return;
    QString note = tr("not in stack");
    QString toolTip = tr("Not listed in xformOpOrder, so USD ignores it: it has no effect on the transform.");
    const std::vector<SdfLayerHandle> layers = otherDefiningLayers(attr);
    if (!layers.empty()) {
        // U+00B7 middle dot, as UTF-8 bytes (no /utf-8 source charset here).
        note += QString::fromUtf8(" \xC2\xB7 ") + tr("from %1").arg(qs(layers.front()->GetDisplayName()));
        if (layers.size() > 1) note += QStringLiteral(" +%1").arg(layers.size() - 1);
        toolTip += QStringLiteral("\n\n") +
                   tr("Defined in other layers than the edit target, so it can't be deleted from here:");
        for (const SdfLayerHandle& layer : layers) {
            toolTip += QStringLiteral("\n    ") + qs(layer->GetIdentifier());
        }
    }
    toolTip += QStringLiteral("\n\n") + tr("Right-click > Add to Stack to use it again.");
    row.row->setNote(note, 0, toolTip);
    row.row->setUnused(true);

    const TfToken name = attr.GetName();
    row.row->setExtraMenu([this, name](QMenu* menu) {
        menu->addSeparator();
        menu->addAction(tr("Add to Stack"), this, [this, name]() {
            const UsdPrim prim = currentPrim();
            if (!prim) return;
            // At the end of xformOpOrder, like "+ Add" with no target op: applied first.
            m_undoManager->apply(currentStage(), tr("Add %1 to Stack").arg(qs(name.GetString())), [&]() {
                const XformStack stack(prim);
                return stack.addExisting(name, stack.ops().size()).has_value();
            });
        });
    });
}

void AttributesViewPanel::buildPrimSection(const UsdPrim& prim, QVBoxLayout* layout) {
    m_primSection = makeSection(QStringLiteral("prim"), tr("Prim"));
    auto addRow = [this](const QString& label, PropertyEditor* editor) {
        auto* row = new PropertyRow(label, editor);
        row->setLabelWidth(kLabelWidth);
        m_primSection->addRow(row);
        return editor;
    };

    // Editable: a rename, like the Outliner's (prim_actions::rename()).
    m_header.name = addRow(tr("Name"), properties::createPropertyEditor(SdfValueTypeNames->String));
    m_header.type = addRow(tr("Type"), readOnlyText(QString()));

    EditorHints kindHints;
    kindHints.allowedTokens = {TfToken(), KindTokens->model, KindTokens->component, KindTokens->group,
                               KindTokens->assembly, KindTokens->subcomponent};
    m_header.kind = addRow(tr("Kind"), properties::createPropertyEditor(SdfValueTypeNames->Token, kindHints));
    m_header.specifier = addRow(tr("Specifier"), readOnlyText(QString()));
    m_header.active = addRow(tr("Active"), properties::createPropertyEditor(SdfValueTypeNames->Bool));
    m_header.instanceable = addRow(tr("Instanceable"), properties::createPropertyEditor(SdfValueTypeNames->Bool));
    m_header.schemas = addRow(tr("Applied Schemas"), readOnlyText(QString()));

    // Prim metadata edits: each one undo step.
    auto edit = [this](const QString& description, std::function<bool(UsdPrim&)> change) {
        UsdPrim p = currentPrim();
        if (p) m_undoManager->apply(currentStage(), description, [&]() { return change(p); });
    };
    connect(m_header.name, &PropertyEditor::valueEdited, this, [this](const VtValue& v) {
        const std::string name = v.IsHolding<std::string>() ? v.UncheckedGet<std::string>() : std::string();
        // Queued: renaming rebuilds this panel, editor included, which must
        // not happen inside the editor's own signal.
        QMetaObject::invokeMethod(this, [this, name]() { renameCurrentPrim(name); }, Qt::QueuedConnection);
    });
    connect(m_header.kind, &PropertyEditor::valueEdited, this, [edit](const VtValue& v) {
        const TfToken kind = v.IsHolding<TfToken>() ? v.UncheckedGet<TfToken>() : TfToken();
        edit(tr("Set Kind"), [kind](UsdPrim& p) { UsdModelAPI(p).SetKind(kind); return true; });
    });
    connect(m_header.active, &PropertyEditor::valueEdited, this, [edit](const VtValue& v) {
        const bool on = v.IsHolding<bool>() && v.UncheckedGet<bool>();
        edit(on ? tr("Activate") : tr("Deactivate"), [on](UsdPrim& p) { return p.SetActive(on); });
    });
    connect(m_header.instanceable, &PropertyEditor::valueEdited, this, [edit](const VtValue& v) {
        const bool on = v.IsHolding<bool>() && v.UncheckedGet<bool>();
        edit(tr("Set Instanceable"), [on](UsdPrim& p) { return p.SetInstanceable(on); });
    });

    refreshPrimHeader(prim);
    layout->addWidget(m_primSection);
}

void AttributesViewPanel::buildCompositionSection(const UsdPrim& prim, QVBoxLayout* layout) {
    const std::vector<ArcInfo> arcs = compositionArcs(prim);
    const bool editablePrim = prim_actions::isEditable(prim);
    if (arcs.empty() && !editablePrim) return;  // nothing to show, nothing to add
    // Collapsed by default while there are no arcs (the + is in the header).
    m_compositionSection = makeSection(QStringLiteral("composition"), tr("Composition"), arcs.empty());
    CategorySection* section = m_compositionSection;
    const SdfPath path = prim.GetPath();
    const SdfLayerHandle editTarget = currentStage()->usdStage()->GetEditTarget().GetLayer();

    if (editablePrim) {
        QToolButton* add = properties::iconButton(QStringLiteral(":/usdcc/theme/plus.png"), tr("Add a composition arc"));
        add->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(add);
        for (ArcInfo::Type type : {ArcInfo::Type::Reference, ArcInfo::Type::Payload, ArcInfo::Type::Inherit,
                                   ArcInfo::Type::Specialize}) {
            menu->addAction(tr("%1...").arg(tr(arcTypeName(type))), this,
                            [this, type]() { addArcInteractively(static_cast<int>(type)); });
        }
        add->setMenu(menu);
        section->addHeaderWidget(add);
    }

    // An editable reference's or payload's rows go in a small frame of their
    // own (QFrame#compositionArc in theme/dark.qss), its labels narrower by
    // the frame's padding so the fields line up with the other sections'.
    // Every other row goes straight in the section.
    constexpr int kFramePadding = 8;
    QVBoxLayout* frameLayout = nullptr;
    auto beginFrame = [section, &frameLayout]() {
        auto* frame = new QFrame();
        frame->setObjectName(QStringLiteral("compositionArc"));
        frame->setAttribute(Qt::WA_StyledBackground);
        frameLayout = new QVBoxLayout(frame);
        usdcc::ui::scaled::margins(frameLayout, kFramePadding, 6, 6, 6);
        usdcc::ui::scaled::spacing(frameLayout, 6);
        section->addRow(frame);
    };
    auto addRow = [section, &frameLayout](const QString& label, PropertyEditor* editor) {
        auto* row = new PropertyRow(label, editor);
        if (frameLayout) {
            row->setLabelWidth(kLabelWidth - kFramePadding);
            frameLayout->addWidget(row);
        } else {
            row->setLabelWidth(kLabelWidth);
            section->addRow(row);
        }
        return row;
    };
    // One button at the row's right edge, its menu the arc's actions: Remove
    // (an arc authored here), Open as Stage and Reveal in Explorer (a file).
    auto actionsButton = [this, path](const ArcInfo& arc, PropertyRow* row, bool removable) {
        const bool file = !arc.assetPath.empty() &&
                          (arc.type == ArcInfo::Type::Reference || arc.type == ArcInfo::Type::Payload);
        if (!removable && !file) return;  // e.g. a read-only reference to this file
        QToolButton* button = properties::iconButton(QStringLiteral(":/usdcc/theme/more.png"),
                                                     tr("%1 actions").arg(tr(arcTypeName(arc.type))), row);
        button->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(button);
        menu->setToolTipsVisible(true);
        if (removable) {
            menu->addAction(tr("Remove"), this, [this, path, arc]() {
                // Queued: removing rebuilds this panel, the menu's button included.
                QMetaObject::invokeMethod(
                    this, [this, path, arc]() { prim_actions::removeArc(*m_undoManager, currentStage(), path, arc); },
                    Qt::QueuedConnection);
            });
        }
        if (file) {
            const QString resolved = qs(arc.resolvedPath);
            QAction* open = menu->addAction(tr("Open as Stage"), this, [resolved]() { openStageFile(resolved); });
            open->setEnabled(!arc.missing);
            QAction* show = menu->addAction(tr("Show USDA"), this, [this, resolved]() { showUsdaPreview(resolved, this); });
            show->setEnabled(!arc.missing);
            QAction* reveal = menu->addAction(tr("Reveal in Explorer"), this, [resolved]() { revealFile(resolved); });
            reveal->setEnabled(!arc.missing && QFileInfo::exists(resolved));
            if (arc.missing) {
                open->setToolTip(tr("The file can't be found"));
                show->setToolTip(tr("The file can't be found"));
                reveal->setToolTip(tr("The file can't be found"));
            }
        }
        button->setMenu(menu);
        row->addLastWidget(button);  // always the rightmost
    };
    auto change = [this, path](const ArcInfo& arc, auto modify) {
        ArcInfo changed = arc;
        modify(changed);
        prim_actions::changeArc(*m_undoManager, currentStage(), path, arc, changed);
    };

    bool otherLayers = false;
    for (int pass = 0; pass < 2; ++pass) {  // the editable arcs first, then the others
        for (const ArcInfo& arc : arcs) {
            const bool editable = arc.editable && editablePrim;
            if (arc.type == ArcInfo::Type::Variant ? pass != 0 : editable != (pass == 0)) continue;
            if (pass == 1 && !otherLayers) {
                otherLayers = true;
                auto* label = new QLabel(tr("From other layers"));
                label->setObjectName(QStringLiteral("propertyNote"));
                section->addRow(label);
            }
            const QString type = tr(arcTypeName(arc.type));
            const bool asset = arc.type == ArcInfo::Type::Reference || arc.type == ArcInfo::Type::Payload;
            frameLayout = nullptr;

            if (arc.type == ArcInfo::Type::Variant) {
                const UsdVariantSet set = prim.GetVariantSet(arc.variantSet);
                EditorHints hints;
                for (const std::string& name : set.GetVariantNames()) hints.allowedTokens.push_back(TfToken(name));
                PropertyEditor* editor = properties::createPropertyEditor(SdfValueTypeNames->Token, hints);
                editor->setValue(VtValue(TfToken(arc.variantSelection)));
                editor->setReadOnly(!editablePrim);
                const std::string setName = arc.variantSet;
                connect(editor, &PropertyEditor::valueEdited, this, [this, path, setName](const VtValue& v) {
                    if (!v.IsHolding<TfToken>()) return;
                    prim_actions::setVariant(*m_undoManager, currentStage(), path, setName, v.UncheckedGet<TfToken>().GetString());
                });
                addRow(tr("Variant: %1").arg(qs(arc.variantSet)), editor);
                continue;
            }

            if (!editable) {
                // Read-only: what it points at, and the layer it's in.
                QString what = asset ? (arc.assetPath.empty() ? tr("(this file)") : qs(arc.assetPath)) : QString();
                if (!arc.primPath.IsEmpty()) what += (what.isEmpty() ? QString() : QStringLiteral("  ")) +
                                                     QStringLiteral("<%1>").arg(qs(arc.primPath.GetString()));
                PropertyRow* row = addRow(type, readOnlyText(what));
                row->setNote(tr("in %1").arg(qs(arc.layerName)), 0, qs(arc.layerIdentifier));
                if (asset && !arc.assetPath.empty()) {
                    // As the asset field shows it, U+00B7 as UTF-8 bytes.
                    const QString resolved = qs(arc.resolvedPath);
                    row->setSubLine(arc.missing ? tr("Unresolved") : tr("Resolved \xC2\xB7 %1").arg(resolved), resolved,
                                    arc.missing);
                }
                actionsButton(arc, row, false);
                continue;
            }

            if (!asset) {  // an inherit or a specialize: its prim path
                PropertyEditor* editor = properties::createPropertyEditor(SdfValueTypeNames->String);
                editor->setValue(VtValue(arc.primPath.GetString()));
                connect(editor, &PropertyEditor::valueEdited, this, [change, arc](const VtValue& v) {
                    if (!v.IsHolding<std::string>() || !SdfPath::IsValidPathString(v.UncheckedGet<std::string>())) return;
                    change(arc, [&](ArcInfo& a) { a.primPath = SdfPath(v.UncheckedGet<std::string>()); });
                });
                actionsButton(arc, addRow(type, editor), true);
                continue;
            }

            // A reference or payload: the file, then its prim, offset and scale.
            beginFrame();
            EditorHints hints;
            hints.anchorLayer = editTarget;
            PropertyEditor* file = properties::createPropertyEditor(SdfValueTypeNames->Asset, hints);
            file->setValue(VtValue(SdfAssetPath(arc.assetPath)));
            connect(file, &PropertyEditor::valueEdited, this, [change, arc](const VtValue& v) {
                if (!v.IsHolding<SdfAssetPath>()) return;
                change(arc, [&](ArcInfo& a) { a.assetPath = v.UncheckedGet<SdfAssetPath>().GetAssetPath(); });
            });
            PropertyRow* fileRow = addRow(type, file);
            actionsButton(arc, fileRow, true);

            EditorHints targetHints;
            targetHints.placeholder = QStringLiteral("defaultPrim");  // empty brings in the file's default prim
            PropertyEditor* target = properties::createPropertyEditor(SdfValueTypeNames->String, targetHints);
            target->setValue(VtValue(arc.primPath.GetString()));
            target->setToolTip(tr("The prim to bring in; empty for the file's default prim"));
            connect(target, &PropertyEditor::valueEdited, this, [change, arc](const VtValue& v) {
                const std::string text = v.IsHolding<std::string>() ? v.UncheckedGet<std::string>() : std::string();
                if (!text.empty() && !SdfPath::IsValidPathString(text)) return;
                change(arc, [&](ArcInfo& a) { a.primPath = text.empty() ? SdfPath() : SdfPath(text); });
            });
            PropertyRow* targetRow = addRow(tr("Prim"), target);
            // Its menu: back to the default prim, or pick one from the file.
            QToolButton* pick = properties::iconButton(QStringLiteral(":/usdcc/theme/more.png"), tr("Prim actions"),
                                                       targetRow);
            pick->setPopupMode(QToolButton::InstantPopup);
            auto* pickMenu = new QMenu(pick);
            pickMenu->setToolTipsVisible(true);
            // Queued, as Remove: an edit rebuilds this panel, the menu's button included.
            if (!arc.primPath.IsEmpty()) {
                pickMenu->addAction(tr("Use defaultPrim"), this, [this, change, arc]() {
                    QMetaObject::invokeMethod(
                        this, [change, arc]() { change(arc, [](ArcInfo& a) { a.primPath = SdfPath(); }); },
                        Qt::QueuedConnection);
                });
            }
            QAction* select = pickMenu->addAction(tr("Select Prim from Stage..."), this, [this, change, arc]() {
                QMetaObject::invokeMethod(
                    this,
                    [this, change, arc]() {
                        const std::optional<SdfPath> chosen = pickPrimFromFile(qs(arc.resolvedPath), arc.primPath, this);
                        if (chosen && *chosen != arc.primPath) change(arc, [&](ArcInfo& a) { a.primPath = *chosen; });
                    },
                    Qt::QueuedConnection);
            });
            select->setEnabled(!arc.missing);
            if (arc.missing) select->setToolTip(tr("The file can't be found"));
            pick->setMenu(pickMenu);
            targetRow->addLastWidget(pick);

            PropertyEditor* offset = properties::createPropertyEditor(SdfValueTypeNames->Double);
            offset->setValue(VtValue(arc.layerOffset.GetOffset()));
            connect(offset, &PropertyEditor::editFinished, this, [change, arc, offset]() {
                const VtValue v = offset->value();
                if (!v.IsHolding<double>()) return;
                change(arc, [&](ArcInfo& a) { a.layerOffset.SetOffset(v.UncheckedGet<double>()); });
            });
            addRow(tr("Time Offset"), offset);
            PropertyEditor* scale = properties::createPropertyEditor(SdfValueTypeNames->Double);
            scale->setValue(VtValue(arc.layerOffset.GetScale()));
            connect(scale, &PropertyEditor::editFinished, this, [change, arc, scale]() {
                const VtValue v = scale->value();
                if (!v.IsHolding<double>()) return;
                change(arc, [&](ArcInfo& a) { a.layerOffset.SetScale(v.UncheckedGet<double>()); });
            });
            addRow(tr("Time Scale"), scale);

            if (arc.type == ArcInfo::Type::Payload) {
                PropertyEditor* loaded = properties::createPropertyEditor(SdfValueTypeNames->Bool);
                loaded->setValue(VtValue(arc.loaded));
                connect(loaded, &PropertyEditor::valueEdited, this, [this, path](const VtValue& v) {
                    const bool on = v.IsHolding<bool>() && v.UncheckedGet<bool>();
                    on ? prim_actions::load(*m_undoManager, currentStage(), {path})
                       : prim_actions::unload(*m_undoManager, currentStage(), {path});
                });
                addRow(tr("Loaded"), loaded);
            }
        }
    }
    if (arcs.empty()) {
        auto* label = new QLabel(tr("No composition arcs. Add one with +."));
        label->setObjectName(QStringLiteral("propertyNote"));
        section->addRow(label);
    }
    layout->addWidget(section);
}

void AttributesViewPanel::addArcInteractively(int typeValue) {
    const UsdPrim prim = currentPrim();
    if (!prim || !m_undoManager) return;
    const auto type = static_cast<ArcInfo::Type>(typeValue);
    const QString typeName = tr(arcTypeName(type));
    if (type == ArcInfo::Type::Reference || type == ArcInfo::Type::Payload) {
        // Relative to the layer being edited, when it's a file, as an asset
        // field makes it.
        const SdfLayerHandle layer = currentStage()->usdStage()->GetEditTarget().GetLayer();
        const QString layerDir =
            layer->GetRealPath().empty() ? QString() : QFileInfo(qs(layer->GetRealPath())).absolutePath();
        const QString file = QFileDialog::getOpenFileName(this, tr("Add %1").arg(typeName), layerDir,
                                                          tr("USD Files (*.usd *.usda *.usdc *.usdz);;All Files (*)"));
        if (file.isEmpty()) return;
        QString assetPath = file;
        if (!layerDir.isEmpty()) {
            assetPath = QDir(layerDir).relativeFilePath(file);
            if (!assetPath.startsWith(QStringLiteral("../"))) assetPath.prepend(QStringLiteral("./"));
        }
        prim_actions::addArc(*m_undoManager, currentStage(), prim.GetPath(), type, assetPath.toStdString(), SdfPath());
        return;
    }
    // An inherit or specialize: another selected prim, if there is one, to
    // start from.
    QString start = QStringLiteral("/");
    for (const SdfPath& selected : stageManager()->selectedPaths(currentStage())) {
        if (selected != prim.GetPath()) {
            start = qs(selected.GetString());
            break;
        }
    }
    bool ok = false;
    const QString target = QInputDialog::getText(this, tr("Add %1").arg(typeName), tr("Prim path:"), QLineEdit::Normal,
                                                 start, &ok)
                               .trimmed();
    if (!ok || !SdfPath::IsValidPathString(target.toStdString())) return;
    prim_actions::addArc(*m_undoManager, currentStage(), prim.GetPath(), type, std::string(), SdfPath(target.toStdString()));
}

void AttributesViewPanel::renameCurrentPrim(const std::string& name) {
    const UsdPrim prim = currentPrim();
    if (!prim || !m_undoManager || name == prim.GetName().GetString()) return;
    const SdfPath oldPath = prim.GetPath();
    if (!prim_actions::rename(*m_undoManager, currentStage(), oldPath, name)) {
        USDCC_LOG_WARNING("usdcc.stage", "Can't rename {} to \"{}\": not a valid prim name, or a sibling has it",
                          oldPath.GetString(), name);
        if (m_primSection && currentPrim()) m_header.name->setValue(VtValue(oldPath.GetName()));  // back to the old one
        return;
    }
    // Keep showing it: the selection follows the new path, and so does
    // anything selected under it.
    const SdfPath newPath = oldPath.ReplaceName(TfToken(name));
    std::vector<SdfPath> paths = stageManager()->selectedPaths(currentStage());
    for (SdfPath& path : paths) {
        if (path.HasPrefix(oldPath)) path = path.ReplacePrefix(oldPath, newPath);
    }
    stageManager()->setSelectedPaths(currentStage(), paths);
}

void AttributesViewPanel::refreshPrimHeader(const UsdPrim& prim) {
    if (!m_primSection || !prim) return;
    m_header.name->setValue(VtValue(prim.GetName().GetString()));
    m_header.name->setToolTip(QString::fromStdString(prim.GetPath().GetString()));  // the path, for a look
    m_header.name->setReadOnly(!prim_actions::isEditable(prim));  // an instance proxy
    m_header.type->setValue(VtValue(prim.GetTypeName().IsEmpty() ? std::string("(typeless)")
                                                                 : prim.GetTypeName().GetString()));
    TfToken kind;
    UsdModelAPI(prim).GetKind(&kind);
    m_header.kind->setValue(VtValue(kind));
    m_header.specifier->setValue(VtValue(std::string(specifierName(prim.GetSpecifier()))));
    m_header.active->setValue(VtValue(prim.IsActive()));
    m_header.instanceable->setValue(VtValue(prim.IsInstanceable()));
    std::vector<std::string> schemas;
    for (const TfToken& s : prim.GetAppliedSchemas()) schemas.push_back(s.GetString());
    m_header.schemas->setValue(VtValue(schemas.empty() ? std::string("(none)") : TfStringJoin(schemas, ", ")));
}

AttributesViewPanel::Row AttributesViewPanel::buildRow(const UsdProperty& property, const PropertyCategory& category) {
    Row row;
    row.property = property;
    const QString label = qs(propertyLabelInCategory(property, category));
    const QString name = qs(property.GetName().GetString());
    row.searchText = (label + QLatin1Char(' ') + name).toLower();

    QString tooltip = QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped());
    if (const UsdAttribute attr = property.As<UsdAttribute>()) {
        tooltip += QStringLiteral("<br>%1").arg(qs(attr.GetTypeName().GetAsToken().GetString()));
        EditorHints hints = EditorHints::forAttribute(attr);
        if (TfStringStartsWith(property.GetName().GetString(), "xformOp:rotate")) {
            hints.suffix = QStringLiteral("\xC2\xB0");
        }
        row.editor = properties::createPropertyEditor(attr.GetTypeName(), hints);
        row.row = new PropertyRow(label, row.editor);
        row.binding = new AttributeBinding(attr, row.editor, currentStage(), m_undoManager.get(), row.row);
        connect(row.row, &PropertyRow::resetRequested, row.binding, &AttributeBinding::resetToDefault);
        // Rebuilds wait for a gesture to finish; pick up any that were held back.
        connect(row.binding, &AttributeBinding::editCommitted, this, [this]() { queueProcessPendingChanges(); });
        if (const UsdGeomPrimvar primvar{attr}) {
            row.row->setNote(qs(primvar.GetInterpolation().GetString()));
        }
    } else if (const UsdRelationship rel = property.As<UsdRelationship>()) {
        tooltip += QStringLiteral("<br>relationship");
        row.editor = readOnlyText(targetsText(rel));
        row.row = new PropertyRow(label, row.editor);
        auto* select = new QToolButton(row.row);
        select->setObjectName(QStringLiteral("propertyButton"));
        select->setText(tr("Select"));
        select->setToolTip(tr("Select the target prims"));
        connect(select, &QToolButton::clicked, this, [this, rel]() {
            SdfPathVector targets;
            rel.GetTargets(&targets);
            std::vector<SdfPath> prims;
            for (const SdfPath& t : targets) prims.push_back(t.GetPrimPath());
            if (!prims.empty()) stageManager()->setSelectedPaths(currentStage(), prims);
        });
        row.row->addTrailingWidget(select);
    }
    const std::string doc = property.GetDocumentation();
    if (!doc.empty()) {
        tooltip += QStringLiteral("<br><br>%1").arg(qs(doc).left(600).toHtmlEscaped());
    }
    const auto stack = property.GetPropertyStack();
    if (!stack.empty() && stack.front()) {
        tooltip += QStringLiteral("<br><br>Strongest opinion: %1").arg(qs(stack.front()->GetLayer()->GetDisplayName()));
    }
    row.row->setLabelWidth(kLabelWidth);
    row.row->setLabelToolTip(tooltip);
    row.row->setPropertyPath(qs(property.GetPath().GetString()));
    updateRowState(row);
    return row;
}

void AttributesViewPanel::updateRowState(Row& row) {
    if (const UsdAttribute attr = row.property.As<UsdAttribute>()) {
        const bool authored = attr.HasAuthoredValue();
        row.row->setUnauthored(!authored);
        row.row->setResettable(authored);
        row.row->setAnimated(attr.GetNumTimeSamples() > 0);
        SdfPathVector sources;
        attr.GetConnections(&sources);
        row.row->setConnectedSource(sources.empty() ? QString() : qs(sources.front().GetString()));
    } else if (const UsdRelationship rel = row.property.As<UsdRelationship>()) {
        row.row->setUnauthored(!rel.IsAuthored());
        row.editor->setValue(VtValue(targetsText(rel).toStdString()));
    }
}

void AttributesViewPanel::refreshValues() {
    m_refreshPending = false;
    const UsdPrim prim = currentPrim();
    if (!prim) {
        rebuild();
        return;
    }
    refreshPrimHeader(prim);
    if (m_xformEditor) m_xformEditor->refresh();
    for (Row& row : m_rows) {
        if (row.binding) row.binding->refresh();
        updateRowState(row);
    }
}

void AttributesViewPanel::applyFilter() {
    const QString text = m_search->text().trimmed().toLower();
    std::vector<int> visibleCounts(m_sections.size(), 0);
    for (Row& row : m_rows) {
        const bool visible = text.isEmpty() || row.searchText.contains(text);
        row.row->setVisible(visible);
        if (visible) {
            for (size_t i = 0; i < m_sections.size(); ++i) {
                if (m_sections[i] == row.section) ++visibleCounts[i];
            }
        }
    }
    for (size_t i = 0; i < m_sections.size(); ++i) {
        m_sections[i]->setVisible(visibleCounts[i] > 0);
        m_sections[i]->setForceExpanded(!text.isEmpty());
    }
    if (m_primSection) {
        m_primSection->setVisible(text.isEmpty());
    }
    if (m_compositionSection) {
        m_compositionSection->setVisible(text.isEmpty());
    }
}

void AttributesViewPanel::onStageObjectsChanged(const StageChanges& changes) {
    if (m_primPath.IsEmpty()) {
        return;
    }
    if (changes.resyncsPrim(m_primPath)) {
        m_rebuildPending = true;
    } else if (changes.affectsPrim(m_primPath)) {
        m_refreshPending = true;
    } else {
        return;
    }
    queueProcessPendingChanges();
}

void AttributesViewPanel::queueProcessPendingChanges() {
    // A drag sends a notice per mouse move: coalesce them into one update, run
    // after the edit that sent the notice has finished.
    if (m_processQueued) return;
    m_processQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_processQueued = false;
            processPendingChanges();
        },
        Qt::QueuedConnection);
}

bool AttributesViewPanel::anyEditing() const {
    for (const Row& row : m_rows) {
        if (row.binding && row.binding->isEditing()) return true;
    }
    return false;
}

void AttributesViewPanel::processPendingChanges() {
    if (!m_rebuildPending && !m_refreshPending) return;
    // Never rebuild under a gesture (it would delete the widget being dragged);
    // the gesture's end calls back here.
    if (anyEditing()) return;
    m_rebuildPending = false;
    // Rebuild only if what's shown changed — and check that for value changes
    // too: reordering transform ops only changes xformOpOrder's value, which
    // USD reports as a plain value change, not a resync. Conversely a resync
    // doesn't always change anything shown (e.g. the first edit of a schema
    // attribute creates its spec): then just refresh the values.
    if (layoutSignature(currentPrim()) != m_signature) {
        rebuild();
        return;
    }
    refreshValues();
}

}  // namespace usdcc::usd
