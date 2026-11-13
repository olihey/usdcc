#include "usdcc/usd/property_sheet.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/add_shade_input_dialog.h"
#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/applied_schemas.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/properties/connected_inputs_editor.h"
#include "usdcc/usd/properties/relationship_targets_editor.h"
#include "usdcc/usd/properties/schema_pills.h"
#include "usdcc/usd/relationship_target_dialog.h"
#include "usdcc/usd/relationship_targets.h"
#include "usdcc/usd/shade_actions.h"
#include "usdcc/usd/shade_connections.h"
#include "usdcc/usd/shade_graph.h"
#include "usdcc/usd/shader_connection_dialog.h"
#include "usdcc/usd/shader_node_catalog.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/prim_picker_dialog.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/variant_actions.h"
#include "usdcc/usd/variant_editor_dialog.h"
#include "usdcc/usd/properties/attribute_binding.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/number_field.h"
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
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
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
#include <QTextDocument>
#include <QPushButton>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <map>
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
// Inside a frame labels are narrower by its padding, so the fields line up with
// the other rows'.
constexpr int kFramePadding = 8;

struct GroupFrame {
    QFrame* widget = nullptr;
    QVBoxLayout* layout = nullptr;
};

// A small frame in the section for rows that belong together (QFrame#compositionArc
// in theme/dark.qss).
GroupFrame addRowFrame(properties::CategorySection* section) {
    auto* frame = new QFrame();
    frame->setObjectName(QStringLiteral("compositionArc"));
    frame->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(frame);
    usdcc::ui::scaled::margins(layout, kFramePadding, 6, 6, 6);
    usdcc::ui::scaled::spacing(layout, 6);
    section->addRow(frame);
    return {frame, layout};
}

// A frame for the primvars of one namespace, titled with it as it's written.
GroupFrame addPrimvarFrame(properties::CategorySection* section, const std::string& group) {
    const GroupFrame frame = addRowFrame(section);
    auto* title = new QLabel(QString::fromStdString(group), frame.widget);
    title->setObjectName(QStringLiteral("propertyFrameTitle"));
    title->setToolTip(QStringLiteral("primvars:%1").arg(QString::fromStdString(group)));
    frame.layout->addWidget(title);
    return frame;
}
constexpr auto kMaterialBindingApi = "MaterialBindingAPI";
const TfToken kMaterialBinding("material:binding");

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

PropertySheet::PropertySheet(StageManager* stageManager, std::shared_ptr<UndoManager> undoManager, Mode mode, QWidget* parent)
    : QWidget(parent), m_stageManager(stageManager), m_undoManager(std::move(undoManager)), m_mode(mode) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("attributeScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    layout->addWidget(m_scroll, 1);
    rebuild();
}

PropertySheet::~PropertySheet() = default;

void PropertySheet::setStage(const StageRefPtr& stage) {
    m_stageChangedConnection = {};
    m_editTargetConnection = {};
    m_stage = stage;
    if (stage) {
        m_stageChangedConnection =
            stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
        // Queued: the target can change from inside a menu or another panel's handler.
        m_editTargetConnection = stage->editTargetChanged.connect(
            [this]() { QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection); });
    }
    // Always rebuild: the new stage may have the same prim path, and the old rows are
    // bound to the old stage.
    rebuild();
}

void PropertySheet::setPrimPath(const SdfPath& path, bool force) {
    if (path == m_primPath && m_content && !force) return;
    m_primPath = path;
    rebuild();
}

void PropertySheet::setSearchText(const QString& text) {
    if (text == m_searchText) return;
    m_searchText = text;
    applyFilter();
}

void PropertySheet::setAuthoredOnly(bool authoredOnly) {
    if (authoredOnly == m_authoredOnly) return;
    m_authoredOnly = authoredOnly;
    rebuild();
}

void PropertySheet::setShowHidden(bool showHidden) {
    if (showHidden == m_showHidden) return;
    m_showHidden = showHidden;
    rebuild();
}

void PropertySheet::setEmptyText(const QString& text) {
    m_emptyText = text;
    if (!currentPrim()) rebuild();
}

QString PropertySheet::collapsedKeyPrefix() const {
    return m_mode == Mode::Full ? QStringLiteral("AttributeEditor/collapsed/") : QStringLiteral("MaterialEditor/collapsed/");
}

UsdPrim PropertySheet::currentPrim() const {
    return m_primPath.IsEmpty() || !m_stage ? UsdPrim()
                                                   : m_stage->usdStage()->GetPrimAtPath(m_primPath);
}

std::string PropertySheet::layoutSignature(const UsdPrim& prim) const {
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
    signature += "|schemas:";
    for (const AppliedSchema& schema : appliedSchemas(prim)) signature += schema.name + ",";
    signature += "|arcs:";
    for (const ArcInfo& arc : compositionArcs(prim)) {
        signature += std::to_string(static_cast<int>(arc.type)) + arc.assetPath + "@" + arc.primPath.GetString() + "@" +
                     std::to_string(arc.layerOffset.GetOffset()) + "x" + std::to_string(arc.layerOffset.GetScale()) +
                     (arc.loaded ? "L" : "U") + arc.variantSet + "=" + arc.variantSelection + arc.layerIdentifier +
                     (arc.editable ? "e" : "r") + ",";
        if (arc.type == ArcInfo::Type::Variant) {
            for (const std::string& name : prim.GetVariantSet(arc.variantSet).GetVariantNames()) signature += name + "/";
        }
    }
    const CategorizeOptions options{m_authoredOnly, m_showHidden};
    for (const PropertyCategory& category : categorizeProperties(prim, options)) {
        signature += "|" + category.key + ":";
        for (const UsdProperty& property : category.properties) {
            signature += property.GetName().GetString() + ",";
        }
    }
    // A Shader's pages: an input made, or one a shownIf hides or shows, changes the rows.
    signature += "|shader:";
    for (const ShaderInputInfo& input : shaderInputs(prim)) {
        signature += input.attribute.GetString() + (input.authored ? "a" : "") + (input.shown ? "s" : "") + ",";
    }
    return signature;
}

CategorySection* PropertySheet::makeSection(const QString& key, const QString& title, bool collapsedByDefault) {
    auto* section = new CategorySection(key, title);
    QSettings settings = usdcc::ui::appSettings();
    section->setCollapsed(settings.value(collapsedKeyPrefix() + key, collapsedByDefault).toBool());
    connect(section, &CategorySection::collapsedChanged, this, [key, prefix = collapsedKeyPrefix()](bool collapsed) {
        QSettings s = usdcc::ui::appSettings();
        s.setValue(prefix + key, collapsed);
    });
    return section;
}

namespace {

// A field Tab stops at: a number field or a text field that can be typed in now.
// A number field's own typing box, and the line edits inside combo and spin boxes,
// aren't fields of their own.
bool isTabStop(const QWidget* widget) {
    if (!widget->isEnabled()) return false;
    if (const auto* number = qobject_cast<const properties::NumberField*>(widget)) return !number->isReadOnly();
    if (const auto* edit = qobject_cast<const QLineEdit*>(widget)) {
        const QWidget* parent = edit->parentWidget();
        if (qobject_cast<const properties::NumberField*>(parent) || qobject_cast<const QComboBox*>(parent) ||
            qobject_cast<const QAbstractSpinBox*>(parent)) {
            return false;
        }
        return !edit->isReadOnly() && (edit->focusPolicy() & Qt::TabFocus);
    }
    return false;
}

// The field `widget` is, or is typing in (a number field's typing box); null if none.
QWidget* owningTabStop(QWidget* widget, const QWidget* content) {
    for (QWidget* at = widget; at && at != content; at = at->parentWidget()) {
        if (isTabStop(at)) return at;
    }
    return nullptr;
}

// Moves the focus into `stop` the way a user starting to type there would: a number
// field opens its typing box, a text field selects its text.
void activateTabStop(QWidget* stop) {
    if (auto* number = qobject_cast<properties::NumberField*>(stop)) {
        number->beginTextEdit();
    } else if (auto* edit = qobject_cast<QLineEdit*>(stop)) {
        edit->setFocus(Qt::TabFocusReason);
        edit->selectAll();
    }
}

}  // namespace

void PropertySheet::rebuild() {
    const int scrollPosition = m_scroll->verticalScrollBar()->value();
    // Typing in a field when the rows are rebuilt (an edit that changes what's shown,
    // such as a rename committed by Tab): the same field of the new rows takes over.
    int typingIn = -1;
    if (QWidget* focus = QApplication::focusWidget(); m_content && qobject_cast<QLineEdit*>(focus) &&
                                                      m_content->isAncestorOf(focus)) {
        if (QWidget* stop = owningTabStop(focus, m_content)) {
            const std::vector<QWidget*> stops = tabStops();
            const auto at = std::find(stops.begin(), stops.end(), stop);
            if (at != stops.end()) typingIn = static_cast<int>(at - stops.begin());
        }
    }
    // The scroll range is only known once the new content has been laid out.
    const auto restoreView = [this, scrollPosition, typingIn]() {
        QTimer::singleShot(0, this, [this, scrollPosition, typingIn]() {
            m_scroll->verticalScrollBar()->setValue(scrollPosition);
            const QWidget* focus = QApplication::focusWidget();
            if (typingIn < 0 || (focus && focus != this && !isAncestorOf(focus))) return;  // the user went elsewhere
            const std::vector<QWidget*> stops = tabStops();
            if (typingIn < static_cast<int>(stops.size())) activateTabStop(stops[static_cast<size_t>(typingIn)]);
        });
    };
    // Held here while the old rows go, or Qt hands the focus to whatever comes next,
    // such as a host's search field, which would look like the user went elsewhere.
    if (typingIn >= 0) setFocus(Qt::OtherFocusReason);
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
    m_connectionScope = SdfPath();  // set again by a shader output's row
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
        const QString select = m_emptyText.isEmpty() ? tr("Select a prim to see its attributes.") : m_emptyText;
        const QString note = !m_stage              ? tr("No stage open.")
                             : m_primPath.IsEmpty() ? select
                                                    : tr("%1 no longer exists.").arg(qs(m_primPath.GetString()));
        auto* empty = new QLabel(note, m_content);
        empty->setObjectName(QStringLiteral("propertyNote"));
        empty->setAlignment(Qt::AlignCenter);
        usdcc::ui::scaled::margins(empty, 0, 24, 0, 0);
        layout->addWidget(empty);
        layout->addStretch(1);
        m_scroll->setWidget(m_content);
        Q_EMIT rebuilt();
        return;
    }

    const bool shader = prim.IsA<UsdShadeShader>();
    if (m_mode == Mode::Shading) {
        buildShadingTitle(prim, layout);
        if (shader) {
            buildShaderPages(prim, layout);
        } else {
            // A Material's or NodeGraph's interface, in its display groups.
            const CategorizeOptions options{m_authoredOnly, m_showHidden};
            for (const PropertyCategory& category : categorizeProperties(prim, options)) {
                PropertyCategory inputs{category.key, category.label, {}};
                for (const UsdProperty& property : category.properties) {
                    if (UsdShadeInput::IsInput(property.As<UsdAttribute>())) inputs.properties.push_back(property);
                }
                if (inputs.properties.empty()) continue;
                CategorySection* section = makeSection(qs(inputs.key), qs(inputs.label));
                for (const UsdProperty& property : inputs.properties) {
                    Row row = buildRow(property, inputs);
                    row.section = section;
                    section->addRow(row.row);
                    m_rows.push_back(row);
                }
                layout->addWidget(section);
                m_sections.push_back(section);
            }
            if (m_sections.empty() && prim.IsA<UsdShadeNodeGraph>()) {
                // An empty Inputs section, for its + (one can be promoted too).
                CategorySection* section = makeSection(QStringLiteral("inputs"), tr("Inputs"));
                auto* none = new QLabel(prim_actions::isEditable(prim) ? tr("No interface inputs. Add one with +, or promote a node's input.")
                                                                       : tr("No interface inputs."),
                                        section);
                none->setObjectName(QStringLiteral("propertyNote"));
                none->setWordWrap(true);
                section->addRow(none);
                layout->addWidget(section);
                m_sections.push_back(section);
            }
            if (m_sections.empty()) {
                auto* none = new QLabel(tr("Not a shader, material or node graph."), m_content);
                none->setObjectName(QStringLiteral("propertyNote"));
                none->setWordWrap(true);
                usdcc::ui::scaled::margins(none, 12, 12, 12, 0);
                layout->addWidget(none);
            } else {
                addInputButton(m_sections.front(), prim);
            }
        }
        layout->addStretch(1);
        m_scroll->setWidget(m_content);
        applyFilter();
        restoreView();
        Q_EMIT rebuilt();
        return;
    }

    buildPrimSection(prim, layout);
    buildCompositionSection(prim, layout);

    const CategorizeOptions options{m_authoredOnly, m_showHidden};
    std::vector<PropertyCategory> categories = categorizeProperties(prim, options);
    // Every xformable prim gets the Transform section, even with nothing to
    // list (Authored Only, no ops yet): it is where ops are added.
    const bool xformable = static_cast<bool>(UsdGeomXformable(prim));
    if (xformable && (categories.empty() || categories.front().key != "transform")) {
        categories.insert(categories.begin(), PropertyCategory{"transform", "Transform", {}});
    }
    // The same for a Material's or NodeGraph's Inputs, where its + adds one: before its
    // Outputs, or last. (A Shader's + is on its first page.)
    const bool interfaceInputs = !shader && shade_actions::canAddInput(prim) && prim_actions::isEditable(prim);
    if (interfaceInputs) {
        const auto has = [&categories](const char* key) {
            return std::find_if(categories.begin(), categories.end(), [key](const PropertyCategory& c) { return c.key == key; });
        };
        if (has("inputs") == categories.end()) categories.insert(has("outputs"), PropertyCategory{"inputs", "Inputs", {}});
    }
    // The material near the top, after Transform, rather than with the namespaces
    // at the end. With MaterialBindingAPI it's there even with no binding yet: the
    // schema defines no properties, so nothing else would say one can be bound.
    const std::vector<AppliedSchema> schemas = appliedSchemas(prim);
    const bool hasBindingApi = std::any_of(schemas.begin(), schemas.end(),
                                           [](const AppliedSchema& s) { return s.name == kMaterialBindingApi; });
    {
        auto material = std::find_if(categories.begin(), categories.end(),
                                     [](const PropertyCategory& c) { return c.key == "ns:material"; });
        PropertyCategory section{"ns:material", tr("Material").toStdString(), {}};
        if (material != categories.end()) {
            section = std::move(*material);
            categories.erase(material);
        }
        if (hasBindingApi || !section.properties.empty()) {
            const bool afterTransform = !categories.empty() && categories.front().key == "transform";
            categories.insert(categories.begin() + (afterTransform ? 1 : 0), std::move(section));
        }
    }
    bool materialBindingShown = false;
    // A Shader's inputs are in its definition's pages, where its Inputs would be (or,
    // with none authored, before its Outputs).
    bool pagesBuilt = false;
    for (PropertyCategory category : categories) {
        if (shader) {
            if (!pagesBuilt && (category.key == "inputs" || category.key == "outputs")) {
                buildShaderPages(prim, layout);
                pagesBuilt = true;
            }
            std::erase_if(category.properties,
                          [](const UsdProperty& property) { return UsdShadeInput::IsInput(property.As<UsdAttribute>()); });
            if (category.properties.empty()) continue;
        }
        CategorySection* section = makeSection(qs(category.key), qs(category.label));
        if (interfaceInputs && category.key == "inputs") addInputButton(section, prim);
        std::set<TfToken> inStack;
        const std::set<std::string> groups = primvarGroups(category);
        std::map<std::string, GroupFrame> frames;
        if (category.key == "transform" && xformable) {
            buildTransformStack(prim, section, inStack);
        }
        for (const UsdProperty& property : category.properties) {
            if (inStack.count(property.GetName()) || property.GetName() == UsdGeomTokens->xformOpOrder) {
                continue;  // shown (and edited) by the transform stack
            }
            const std::string namespaceName = primvarNamespace(property);
            const bool grouped = groups.count(namespaceName) > 0;
            Row row = buildRow(property, category, grouped ? namespaceName : std::string());
            row.section = section;
            if (property.GetName() == kMaterialBinding) materialBindingShown = true;
            if (category.key == "transform" && UsdGeomXformOp::IsXformOp(property.GetName())) {
                setUpUnusedOp(row);
            }
            if (grouped) {
                // In the frame of its namespace, which goes where its first primvar would.
                GroupFrame& frame = frames[namespaceName];
                if (!frame.widget) frame = addPrimvarFrame(section, namespaceName);
                row.frame = frame.widget;
                row.row->setLabelWidth(kLabelWidth - kFramePadding);
                frame.layout->addWidget(row.row);
            } else {
                section->addRow(row.row);
            }
            m_rows.push_back(row);
        }
        // Authored or not, whatever the Authored Only option says: an unset binding
        // never is, and this row is how one gets set.
        if (category.key == "ns:material" && hasBindingApi && !materialBindingShown) {
            Row row;
            row.searchText = QStringLiteral("binding material:binding");
            buildRelationshipEditor(row, kMaterialBinding, tr("Binding"));
            row.row->setLabelWidth(kLabelWidth);
            row.section = section;
            updateRowState(row);
            section->addRow(row.row);
            m_rows.push_back(row);
        }
        layout->addWidget(section);
        m_sections.push_back(section);
    }
    if (shader && !pagesBuilt) buildShaderPages(prim, layout);
    layout->addStretch(1);
    m_scroll->setWidget(m_content);

    applyFilter();
    restoreView();
    Q_EMIT rebuilt();
}

void PropertySheet::buildShadingTitle(const UsdPrim& prim, QVBoxLayout* layout) {
    QString what = qs(prim.GetTypeName().GetString());
    if (const UsdShadeShader shader{prim}) {
        if (const SdrShaderNodeConstPtr definition = ShaderNodeCatalog::definitionOf(shader)) {
            const std::string label = definition->GetLabel().IsEmpty() ? definition->GetName() : definition->GetLabel().GetString();
            what = qs(label);
        } else {
            TfToken id;
            shader.GetShaderId(&id);
            what = id.IsEmpty() ? tr("Shader with no definition") : qs(id.GetString());
        }
    }
    auto* title = new QLabel(m_content);
    title->setObjectName(QStringLiteral("propertySheetTitle"));
    title->setTextFormat(Qt::RichText);
    title->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;<span style='color:#8e8e93'>%2</span>")
                       .arg(qs(prim.GetName().GetString()).toHtmlEscaped(), what.toHtmlEscaped()));
    title->setToolTip(qs(prim.GetPath().GetString()));
    usdcc::ui::scaled::margins(title, 10, 8, 10, 6);
    layout->addWidget(title);
}

void PropertySheet::buildShaderPages(const UsdPrim& prim, QVBoxLayout* layout) {
    const std::vector<ShaderInputInfo> inputs = shaderInputs(prim);
    // Pages in the order their first input comes; inputs without one first.
    std::vector<std::string> pages;
    std::map<std::string, std::vector<const ShaderInputInfo*>> byPage;
    for (const ShaderInputInfo& input : inputs) {
        const bool listed =
            input.authored || input.connected || (!m_authoredOnly && input.shown && (!input.hidden || m_showHidden));
        if (!listed) continue;
        if (!byPage.count(input.page)) pages.push_back(input.page);
        byPage[input.page].push_back(&input);
    }
    std::stable_partition(pages.begin(), pages.end(), [](const std::string& page) { return page.empty(); });
    for (const std::string& page : pages) {
        const std::vector<const ShaderInputInfo*>& rows = byPage[page];
        // A page starts open if something in it is authored or connected, as in the graph.
        const bool anyAuthored = std::any_of(rows.begin(), rows.end(),
                                             [](const ShaderInputInfo* input) { return input->authored || input->connected; });
        QString title = page.empty() ? tr("Inputs") : qs(page);
        title.replace(QLatin1Char(':'), QStringLiteral(" / "));
        CategorySection* section =
            makeSection(QStringLiteral("shaderPage:%1").arg(qs(page)), title, !page.empty() && !anyAuthored);
        for (const ShaderInputInfo* input : rows) {
            Row row = buildShaderInputRow(prim, *input);
            row.section = section;
            section->addRow(row.row);
            m_rows.push_back(row);
        }
        if (page == pages.front()) addInputButton(section, prim);
        layout->addWidget(section);
        m_sections.push_back(section);
    }
    if (!pages.empty()) return;
    const QString note = m_authoredOnly ? tr("No inputs authored.") : tr("This shader has no inputs.");
    if (shade_actions::canAddInput(prim) && prim_actions::isEditable(prim)) {
        // An empty Inputs section, for its +.
        CategorySection* section = makeSection(QStringLiteral("shaderPage:"), tr("Inputs"));
        addInputButton(section, prim);
        if (m_mode == Mode::Shading) {
            auto* none = new QLabel(note, section);
            none->setObjectName(QStringLiteral("propertyNote"));
            section->addRow(none);
        }
        layout->addWidget(section);
        m_sections.push_back(section);
    } else if (m_mode == Mode::Shading) {
        auto* none = new QLabel(note, m_content);
        none->setObjectName(QStringLiteral("propertyNote"));
        usdcc::ui::scaled::margins(none, 12, 12, 12, 0);
        layout->addWidget(none);
    }
}

void PropertySheet::addInputButton(CategorySection* section, const UsdPrim& prim) {
    if (!section || !shade_actions::canAddInput(prim) || !prim_actions::isEditable(prim)) return;
    QToolButton* add = properties::iconButton(QStringLiteral(":/usdcc/theme/plus.png"), tr("Add an input..."));
    add->setProperty("addInputButton", true);  // keeps the #propertyIconButton look
    connect(add, &QToolButton::clicked, this, [this, path = prim.GetPath()]() {
        // Queued: adding rebuilds this panel, the button included.
        QMetaObject::invokeMethod(
            this, [this, path]() { addShadeInputInteractively(*m_undoManager, m_stage, path, this); }, Qt::QueuedConnection);
    });
    section->addHeaderWidget(add);
}

PropertySheet::Row PropertySheet::buildShaderInputRow(const UsdPrim& prim, const ShaderInputInfo& input) {
    const UsdAttribute attr = prim.GetAttribute(input.attribute);
    const bool exists = attr.IsValid() && attr.IsDefined();
    Row row;
    if (exists) {
        row.property = attr;
    } else {
        row.pendingInput = input.attribute;
    }
    const QString label = qs(input.label);
    const QString name = qs(input.attribute.GetString());
    row.searchText = (label + QLatin1Char(' ') + name).toLower();

    EditorHints hints = exists ? EditorHints::forAttribute(attr) : EditorHints();
    if (!exists && m_stage) {
        hints.anchorLayer = m_stage->usdStage()->GetEditTarget().GetLayer();
        hints.editLayer = hints.anchorLayer;
    }
    if (!input.options.empty() && hints.allowedTokens.empty()) hints.allowedTokens = input.options;
    if (!hints.softRange) hints.softRange = input.softRange;
    if (!hints.hardRange) hints.hardRange = input.hardRange;
    const SdfValueTypeName type = input.type ? input.type : SdfValueTypeNames->Token;
    row.editor = properties::createPropertyEditor(type, hints);
    row.row = new PropertyRow(label, row.editor);
    if (exists) {
        row.binding = new AttributeBinding(attr, row.editor, m_stage, m_undoManager.get(), row.row);
    } else {
        row.binding = new AttributeBinding(prim, input.attribute, type, input.defaultValue, row.editor, m_stage,
                                           m_undoManager.get(), row.row);
    }
    connect(row.row, &PropertyRow::resetRequested, row.binding, &AttributeBinding::resetToDefault);
    connect(row.binding, &AttributeBinding::editCommitted, this, [this]() { queueProcessPendingChanges(); });
    row.row->setReadOnly(!prim_actions::isEditable(prim));
    if (exists && isReceiver(attr)) {
        addConnectionButton(row, attr.GetPath());
    } else if (!exists && UsdShadeConnectableAPI(prim)) {
        // Not authored yet: connecting creates it, as editing its value does.
        addConnectionButton(row, prim.GetPath().AppendProperty(input.attribute), type);
    }
    if (exists) {
        connect(row.row, &PropertyRow::connectedSourceClicked, this, [this, path = attr.GetPath()]() {
            if (const UsdAttribute connected = m_stage->usdStage()->GetAttributeAtPath(path)) {
                const SdfPath source = connectedSource(connected);
                if (!source.IsEmpty()) selectPrimLater(source);
            }
        });
    }

    QString tooltip = QStringLiteral("<b>%1</b><br>%2").arg(name.toHtmlEscaped(), qs(type.GetAsToken().GetString()));
    if (!input.help.empty()) tooltip += QStringLiteral("<br><br>%1").arg(qs(input.help).left(600).toHtmlEscaped());
    if (!input.defaultValue.IsEmpty()) {
        tooltip += QStringLiteral("<br><br>Default: %1").arg(qs(TfStringify(input.defaultValue)).left(80).toHtmlEscaped());
    }
    if (!exists) {
        tooltip += QStringLiteral("<br><br>") + tr("Not authored: editing it creates it.");
    } else {
        const auto stack = attr.GetPropertyStack();
        if (!stack.empty() && stack.front()) {
            tooltip += QStringLiteral("<br><br>Strongest opinion: %1").arg(qs(stack.front()->GetLayer()->GetDisplayName()));
        }
    }
    row.row->setLabelWidth(kLabelWidth);
    row.row->setLabelToolTip(tooltip);
    row.row->setPropertyPath(qs(prim.GetPath().AppendProperty(input.attribute).GetString()));
    row.row->setObjectName(QStringLiteral("shaderInput"));
    row.row->setProperty("attribute", name);
    updateRowState(row);
    return row;
}

void PropertySheet::buildTransformStack(const UsdPrim& prim, CategorySection* section, std::set<TfToken>& inStack) {
    m_xformEditor = new properties::TransformStackEditor(section, prim, m_stage, m_undoManager.get(), section);
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

void PropertySheet::setUpUnusedOp(const Row& row) {
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
            if (!prim_actions::isEditable(prim)) return;
            // At the end of xformOpOrder, like "+ Add" with no target op: applied first.
            m_undoManager->apply(m_stage, tr("Add %1 to Stack").arg(qs(name.GetString())), [&]() {
                const XformStack stack(prim);
                return stack.addExisting(name, stack.ops().size()).has_value();
            });
        });
    });
}

void PropertySheet::buildPrimSection(const UsdPrim& prim, QVBoxLayout* layout) {
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
    m_header.schemas = new properties::SchemaPillsEditor();
    addRow(tr("Applied Schemas"), m_header.schemas);
    connect(m_header.schemas, &properties::SchemaPillsEditor::removeRequested, this, [this](const QString& schema) {
        // Queued: removing rebuilds this panel, the pill included.
        QMetaObject::invokeMethod(this, [this, schema]() { removeSchemaInteractively(schema); }, Qt::QueuedConnection);
    });
    auto* schemaMenu = new QMenu(m_header.schemas->addButton());
    schemaMenu->setToolTipsVisible(true);
    connect(schemaMenu, &QMenu::aboutToShow, this, [this, schemaMenu]() {
        schemaMenu->clear();
        populateSchemaMenu(schemaMenu);
    });
    m_header.schemas->addButton()->setMenu(schemaMenu);

    // Prim metadata edits: each one undo step.
    auto edit = [this](const QString& description, std::function<bool(UsdPrim&)> change) {
        UsdPrim p = currentPrim();
        if (p) m_undoManager->apply(m_stage, description, [&]() { return change(p); });
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

void PropertySheet::buildCompositionSection(const UsdPrim& prim, QVBoxLayout* layout) {
    const std::vector<ArcInfo> arcs = compositionArcs(prim);
    const bool editablePrim = prim_actions::isEditable(prim);
    if (arcs.empty() && !editablePrim) return;  // nothing to show, nothing to add
    // Collapsed by default while there are no arcs (the + is in the header).
    m_compositionSection = makeSection(QStringLiteral("composition"), tr("Composition"), arcs.empty());
    CategorySection* section = m_compositionSection;
    const SdfPath path = prim.GetPath();
    const SdfLayerHandle editTarget = m_stage->usdStage()->GetEditTarget().GetLayer();

    if (editablePrim) {
        QToolButton* add = properties::iconButton(QStringLiteral(":/usdcc/theme/plus.png"), tr("Add a composition arc"));
        add->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(add);
        for (ArcInfo::Type type : {ArcInfo::Type::Reference, ArcInfo::Type::Payload, ArcInfo::Type::Inherit,
                                   ArcInfo::Type::Specialize}) {
            menu->addAction(tr("%1...").arg(tr(arcTypeName(type))), this,
                            [this, type]() { addArcInteractively(static_cast<int>(type)); });
        }
        menu->addSeparator();
        menu->addAction(tr("Variant Set..."), this, [this, path]() {
            // Queued: adding rebuilds this panel, the menu's button included.
            QMetaObject::invokeMethod(
                this, [this, path]() { addVariantSetInteractively(m_stage, *m_undoManager, path, this); }, Qt::QueuedConnection);
        });
        add->setMenu(menu);
        section->addHeaderWidget(add);
    }

    // An editable reference's or payload's rows go in a small frame of their
    // own. Every other row goes straight in the section.
    QVBoxLayout* frameLayout = nullptr;
    auto beginFrame = [section, &frameLayout]() { frameLayout = addRowFrame(section).layout; };
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
                    this, [this, path, arc]() { prim_actions::removeArc(*m_undoManager, m_stage, path, arc); },
                    Qt::QueuedConnection);
            });
        }
        if (file) {
            const QString resolved = qs(arc.resolvedPath);
            QAction* open = menu->addAction(tr("Open as Stage"), this, [resolved]() { openStageFile(resolved); });
            open->setEnabled(!arc.missing);
            QAction* show = menu->addAction(tr("Show USDA"), this, [this, resolved]() { showUsdaPreview(resolved, this); });
            show->setEnabled(!arc.missing);
            QAction* prims = menu->addAction(tr("Show Prims"), this, [this, resolved]() { showPrimTree(resolved, this); });
            prims->setEnabled(!arc.missing);
            QAction* reveal = menu->addAction(tr("Reveal in Explorer"), this, [resolved]() { revealFile(resolved); });
            reveal->setEnabled(!arc.missing && QFileInfo::exists(resolved));
            if (arc.missing) {
                open->setToolTip(tr("The file can't be found"));
                show->setToolTip(tr("The file can't be found"));
                prims->setToolTip(tr("The file can't be found"));
                reveal->setToolTip(tr("The file can't be found"));
            }
        }
        button->setMenu(menu);
        row->addLastWidget(button);  // always the rightmost
    };
    auto change = [this, path](const ArcInfo& arc, auto modify) {
        ArcInfo changed = arc;
        modify(changed);
        prim_actions::changeArc(*m_undoManager, m_stage, path, arc, changed);
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
                    prim_actions::setVariant(*m_undoManager, m_stage, path, setName, v.UncheckedGet<TfToken>().GetString());
                });
                PropertyRow* row = addRow(tr("Variant: %1").arg(qs(arc.variantSet)), editor);
                const std::vector<std::string> names = set.GetVariantNames();
                if (!arc.variantSelection.empty() &&
                    std::find(names.begin(), names.end(), arc.variantSelection) == names.end()) {
                    row->setSubLine(tr("\"%1\" isn't a variant of this set").arg(qs(arc.variantSelection)), QString(), true);
                }
                if (editablePrim) addVariantSetRowActions(row, arc.variantSet);
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
                change(arc, [&](ArcInfo& a) {
                    const std::string asset = v.UncheckedGet<SdfAssetPath>().GetAssetPath();
                    // A prim in the old file means nothing in the new one, so back to its
                    // defaultPrim. Not with no file: that refers to this stage and needs a prim.
                    if (asset != a.assetPath && !asset.empty()) a.primPath = SdfPath();
                    a.assetPath = asset;
                });
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
                    on ? prim_actions::load(*m_undoManager, m_stage, {path})
                       : prim_actions::unload(*m_undoManager, m_stage, {path});
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

void PropertySheet::addVariantSetRowActions(PropertyRow* row, const std::string& set) {
    const SdfPath path = m_primPath;
    QToolButton* edit = properties::iconButton(QStringLiteral(":/usdcc/theme/pencil.png"),
                                               tr("Edit the variants of %1").arg(qs(set)), row);
    connect(edit, &QToolButton::clicked, this, [this, path, set]() {
        showVariantEditor(m_stageManager, m_stage, m_undoManager.get(), path, set, this);
    });
    row->addTrailingWidget(edit);

    QToolButton* more = properties::iconButton(QStringLiteral(":/usdcc/theme/more.png"), tr("Variant set actions"), row);
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->setToolTipsVisible(true);
    // Queued, as the arcs' actions: each rebuilds this panel, the menu's button included.
    auto queued = [this](std::function<void()> action) {
        return [this, action = std::move(action)]() { QMetaObject::invokeMethod(this, action, Qt::QueuedConnection); };
    };
    QAction* clear = menu->addAction(tr("Clear Selection"), this, queued([this, path, set]() {
        prim_actions::clearVariant(*m_undoManager, m_stage, path, set);
    }));
    QAction* rename = menu->addAction(tr("Rename Set..."), this, queued([this, path, set]() {
        renameVariantSetInteractively(m_stage, *m_undoManager, path, set, this);
    }));
    QAction* remove = menu->addAction(tr("Remove Set"), this, queued([this, path, set]() {
        removeVariantSetInteractively(m_stage, *m_undoManager, path, set, this);
    }));
    // As the menu opens: what can be done follows the edit target.
    connect(menu, &QMenu::aboutToShow, this, [this, path, set, clear, rename, remove]() {
        const UsdPrim prim = currentPrim();
        QString why;
        const bool editing = m_stage->isEditingVariantSet(path, set);
        clear->setEnabled(!editing);
        clear->setToolTip(editing ? tr("A variant of it is being edited: exit first") : QString());
        rename->setEnabled(variant_actions::canRenameSet(m_stage, prim, set, &why));
        rename->setToolTip(why);
        why.clear();
        remove->setEnabled(variant_actions::canRemoveSet(m_stage, prim, set, &why));
        remove->setToolTip(why);
    });
    more->setMenu(menu);
    row->addLastWidget(more);
}

void PropertySheet::addArcInteractively(int typeValue) {
    const UsdPrim prim = currentPrim();
    if (!prim || !m_undoManager) return;
    const auto type = static_cast<ArcInfo::Type>(typeValue);
    const QString typeName = tr(arcTypeName(type));
    if (type == ArcInfo::Type::Reference || type == ArcInfo::Type::Payload) {
        // Relative to the layer being edited, when it's a file, as an asset
        // field makes it.
        const SdfLayerHandle layer = m_stage->usdStage()->GetEditTarget().GetLayer();
        const QString file =
            QFileDialog::getOpenFileName(this, tr("Add %1").arg(typeName), properties::layerFileFolder(layer),
                                         tr("USD Files (*.usd *.usda *.usdc *.usdz);;All Files (*)"));
        if (file.isEmpty()) return;
        // Absolute in a .usdz, where a relative path is anchored inside it.
        const QString assetPath = properties::assetPathForFile(file, properties::layerFolder(layer));
        prim_actions::addArc(*m_undoManager, m_stage, prim.GetPath(), type, assetPath.toStdString(), SdfPath());
        return;
    }
    // An inherit or specialize: another selected prim, if there is one, to
    // start from.
    QString start = QStringLiteral("/");
    for (const SdfPath& selected : m_stageManager->selectedPaths(m_stage)) {
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
    prim_actions::addArc(*m_undoManager, m_stage, prim.GetPath(), type, std::string(), SdfPath(target.toStdString()));
}

void PropertySheet::renameCurrentPrim(const std::string& name) {
    const UsdPrim prim = currentPrim();
    if (!prim || !m_undoManager || name == prim.GetName().GetString()) return;
    const SdfPath oldPath = prim.GetPath();
    // The selection follows it (Stage::primsMoved), and so this panel.
    if (!prim_actions::rename(*m_undoManager, m_stage, oldPath, name, m_stageManager->stages())) {
        USDCC_LOG_WARNING("usdcc.stage", "Can't rename {} to \"{}\": not a valid prim name, or a sibling has it",
                          oldPath.GetString(), name);
        if (m_primSection && currentPrim()) m_header.name->setValue(VtValue(oldPath.GetName()));  // back to the old one
        return;
    }
}

void PropertySheet::refreshPrimHeader(const UsdPrim& prim) {
    if (!m_primSection || !prim) return;
    m_header.name->setValue(VtValue(prim.GetName().GetString()));
    m_header.name->setToolTip(QString::fromStdString(prim.GetPath().GetString()));  // the path, for a look
    const bool editable = prim_actions::isEditable(prim);  // not an instance proxy, nor outside a variant being edited
    m_header.name->setReadOnly(!prim_actions::canRename(prim));
    if (editable && !prim_actions::canRename(prim)) {
        m_header.name->setToolTip(tr("%1\nCan't be renamed while editing a variant: exit first").arg(qs(prim.GetPath().GetString())));
    }
    m_header.kind->setReadOnly(!editable);
    m_header.active->setReadOnly(!editable);
    m_header.instanceable->setReadOnly(!editable);
    m_header.type->setValue(VtValue(prim.GetTypeName().IsEmpty() ? std::string("(typeless)")
                                                                 : prim.GetTypeName().GetString()));
    TfToken kind;
    UsdModelAPI(prim).GetKind(&kind);
    m_header.kind->setValue(VtValue(kind));
    m_header.specifier->setValue(VtValue(std::string(specifierName(prim.GetSpecifier()))));
    m_header.active->setValue(VtValue(prim.IsActive()));
    m_header.instanceable->setValue(VtValue(prim.IsInstanceable()));
    m_header.schemas->setReadOnly(!editable);
    m_header.schemas->setSchemas(appliedSchemas(prim));
}

void PropertySheet::populateSchemaMenu(QMenu* menu) {
    for (const SchemaCategory& category : schemaCatalog(currentPrim())) {
        QMenu* submenu = menu->addMenu(qs(category.name));
        submenu->setToolTipsVisible(true);
        for (const SchemaCatalogEntry& entry : category.schemas) {
            const QString name = qs(entry.name);
            const bool multiple = entry.multipleApply;
            QAction* action = submenu->addAction(multiple ? name + QStringLiteral("...") : name, this, [this, name, multiple]() {
                // Queued: applying rebuilds this panel, the menu's button included.
                QMetaObject::invokeMethod(
                    this, [this, name, multiple]() { applySchemaInteractively(name, multiple); }, Qt::QueuedConnection);
            });
            action->setEnabled(entry.available);
            // Wrapped: a schema's documentation is long, USD's reasons too.
            const std::string tip = entry.available ? entry.documentation : entry.whyNot;
            action->setToolTip(Qt::convertFromPlainText(qs(tip).trimmed().section(QStringLiteral("\n\n"), 0, 0)));
        }
    }
}

void PropertySheet::applySchemaInteractively(const QString& schema, bool multipleApply) {
    const UsdPrim prim = currentPrim();
    if (!prim) return;
    QString applied = schema;
    if (multipleApply) {
        QString instance;
        for (;;) {
            bool ok = false;
            instance = QInputDialog::getText(this, tr("Apply %1").arg(schema), tr("Instance name:"), QLineEdit::Normal,
                                             instance, &ok)
                           .trimmed();
            if (!ok) return;
            std::string why;
            if (canApplyInstance(prim, schema.toStdString(), instance.toStdString(), &why)) break;
            QMessageBox::warning(this, tr("Apply %1").arg(schema), qs(why));
        }
        applied = schema + QLatin1Char(':') + instance;
    }
    // Its material, rather than a binding to find afterwards.
    if (applied == QLatin1String(kMaterialBindingApi)) {
        RelationshipTargetDialog dialog(m_stage->usdStage(), kMaterialBinding.GetString(), SdfPath(), this);
        dialog.setWindowTitle(tr("Bind a Material to %1").arg(qs(prim.GetName().GetString())));
        dialog.allowNoTarget(tr("Apply Without Material"));
        if (dialog.exec() != QDialog::Accepted) return;
        if (dialog.selectedPath().IsEmpty()) {
            prim_actions::applySchema(*m_undoManager, m_stage, prim.GetPath(), applied.toStdString());
        } else {
            prim_actions::applySchemaWithTarget(*m_undoManager, m_stage, prim.GetPath(), applied.toStdString(),
                                                kMaterialBinding, dialog.selectedPath());
        }
        return;
    }
    // Nothing to add, nothing to choose between.
    bool addAttributes = false;
    const std::vector<TfToken> attributes = schemaAttributesToAdd(prim, applied.toStdString());
    if (!attributes.empty()) {
        constexpr std::size_t kMaxListed = 12;
        QStringList lines;
        for (std::size_t i = 0; i < attributes.size() && i < kMaxListed; ++i) {
            lines << QStringLiteral("    ") + qs(attributes[i].GetString());
        }
        if (attributes.size() > kMaxListed) {
            lines << QStringLiteral("    ") + tr("and %1 more").arg(attributes.size() - kMaxListed);
        }
        QMessageBox box(QMessageBox::Question, tr("Apply %1").arg(applied),
                        tr("Apply %1 to %2?").arg(applied, qs(prim.GetPath().GetString())), QMessageBox::Cancel, this);
        box.setInformativeText((attributes.size() == 1
                                    ? tr("Add its attribute to the prim, with its default value?")
                                    : tr("Add its %1 attributes to the prim, with their default values?").arg(attributes.size())) +
                               QLatin1Char('\n') + lines.join(QLatin1Char('\n')));
        QPushButton* only = box.addButton(tr("Apply Schema Only"), QMessageBox::AcceptRole);
        QPushButton* add = box.addButton(tr("Apply and Add Attributes"), QMessageBox::AcceptRole);
        box.setDefaultButton(only);
        box.exec();
        if (box.clickedButton() != only && box.clickedButton() != add) return;
        addAttributes = box.clickedButton() == add;
    }
    prim_actions::applySchema(*m_undoManager, m_stage, prim.GetPath(), applied.toStdString(), addAttributes);
}

void PropertySheet::removeSchemaInteractively(const QString& schema) {
    const UsdPrim prim = currentPrim();
    if (!prim) return;
    const OrphanedProperties orphans = orphanedProperties(prim, schema.toStdString());
    bool cleanUp = false;
    // Nothing here to clean up: nothing to choose between.
    if (!orphans.inEditTarget.empty()) {
        constexpr std::size_t kMaxListed = 12;
        const std::size_t count = orphans.inEditTarget.size();
        QStringList lines;
        for (std::size_t i = 0; i < count && i < kMaxListed; ++i) {
            lines << QStringLiteral("    ") + qs(orphans.inEditTarget[i].GetString());
        }
        if (count > kMaxListed) lines << QStringLiteral("    ") + tr("and %1 more").arg(count - kMaxListed);
        if (!orphans.inOtherLayers.empty()) {
            lines << QString()
                  << (orphans.inOtherLayers.size() == 1
                          ? tr("1 more is in another layer, so it can't be removed from here.")
                          : tr("%1 more are in other layers, so they can't be removed from here.").arg(orphans.inOtherLayers.size()));
        }
        QMessageBox box(QMessageBox::Question, tr("Remove %1").arg(schema),
                        tr("Remove %1 from %2?").arg(schema, qs(prim.GetPath().GetString())), QMessageBox::Cancel, this);
        box.setInformativeText((count == 1 ? tr("It leaves 1 authored property behind:")
                                           : tr("It leaves %1 authored properties behind:").arg(count)) +
                               QLatin1Char('\n') + lines.join(QLatin1Char('\n')));
        QPushButton* only = box.addButton(tr("Remove Schema Only"), QMessageBox::AcceptRole);
        QPushButton* clean = box.addButton(tr("Remove and Clean Up"), QMessageBox::DestructiveRole);
        box.setDefaultButton(only);
        box.exec();
        if (box.clickedButton() != only && box.clickedButton() != clean) return;
        cleanUp = box.clickedButton() == clean;
    }
    prim_actions::removeSchema(*m_undoManager, m_stage, prim.GetPath(), schema.toStdString(), cleanUp);
}

PropertySheet::Row PropertySheet::buildRow(const UsdProperty& property, const PropertyCategory& category,
                                                       const std::string& primvarGroup) {
    Row row;
    row.property = property;
    const QString label = qs(primvarGroup.empty() ? propertyLabelInCategory(property, category)
                                                  : propertyLabelInPrimvarGroup(property, primvarGroup));
    const QString name = qs(property.GetName().GetString());
    row.searchText = (label + QLatin1Char(' ') + name).toLower();

    QString tooltip = QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped());
    if (const UsdAttribute attr = property.As<UsdAttribute>(); attr && isShaderOutput(attr)) {
        tooltip += QStringLiteral("<br>%1").arg(qs(attr.GetTypeName().GetAsToken().GetString()));
        buildShaderOutputEditor(row, attr.GetPath(), label);
    } else if (attr) {
        tooltip += QStringLiteral("<br>%1").arg(qs(attr.GetTypeName().GetAsToken().GetString()));
        EditorHints hints = EditorHints::forAttribute(attr);
        if (TfStringStartsWith(property.GetName().GetString(), "xformOp:rotate")) {
            hints.suffix = QStringLiteral("\xC2\xB0");
        }
        row.editor = properties::createPropertyEditor(attr.GetTypeName(), hints);
        row.row = new PropertyRow(label, row.editor);
        row.binding = new AttributeBinding(attr, row.editor, m_stage, m_undoManager.get(), row.row);
        connect(row.row, &PropertyRow::resetRequested, row.binding, &AttributeBinding::resetToDefault);
        // Rebuilds wait for a gesture to finish; pick up any that were held back.
        connect(row.binding, &AttributeBinding::editCommitted, this, [this]() { queueProcessPendingChanges(); });
        row.row->setReadOnly(!prim_actions::isEditable(attr.GetPrim()));
        if (const UsdGeomPrimvar primvar{attr}) {
            row.row->setNote(qs(primvar.GetInterpolation().GetString()));
        }
        if (isReceiver(attr)) addConnectionButton(row, attr.GetPath());
        connect(row.row, &PropertyRow::connectedSourceClicked, this, [this, path = attr.GetPath()]() {
            if (const UsdAttribute connected = m_stage->usdStage()->GetAttributeAtPath(path)) {
                const SdfPath source = connectedSource(connected);
                if (!source.IsEmpty()) selectPrimLater(source);
            }
        });
    } else if (const UsdRelationship rel = property.As<UsdRelationship>()) {
        tooltip += QStringLiteral("<br>relationship");
        buildRelationshipEditor(row, property.GetName(), label);
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

void PropertySheet::updateRowState(Row& row) {
    if (const UsdAttribute attr = row.property.As<UsdAttribute>(); attr && row.shaderOutput) {
        const UsdPrim scope = connectionScope(attr.GetPrim());
        std::vector<properties::ConnectedInputsEditor::Receiver> receivers;
        for (const UsdAttribute& receiver : receiversOf(attr)) {
            receivers.push_back({receiver.GetPath(), qs(relativeLabel(receiver.GetPath(), scope))});
        }
        auto* editor = static_cast<properties::ConnectedInputsEditor*>(row.editor);
        editor->setReadOnly(!prim_actions::isEditable(attr.GetPrim()));
        editor->setReceivers(receivers);
    } else if (attr) {
        const bool authored = attr.HasAuthoredValue();
        const bool editable = prim_actions::isEditable(attr.GetPrim());
        row.row->setUnauthored(!authored);
        row.row->setResettable(authored && editable);
        row.row->setAnimated(attr.GetNumTimeSamples() > 0);
        const SdfPath source = connectedSource(attr);
        const QString label = source.IsEmpty() ? QString() : qs(relativeLabel(source, connectionScope(attr.GetPrim())));
        row.row->setConnectedSource(label, qs(source.GetString()));
        if (row.connectButton) {
            row.connectButton->setIcon(QIcon(source.IsEmpty() ? QStringLiteral(":/usdcc/theme/link.png")
                                                              : QStringLiteral(":/usdcc/theme/unlink.png")));
            row.connectButton->setToolTip(source.IsEmpty() ? tr("Connect\xE2\x80\xA6") : tr("Disconnect from %1").arg(label));
            row.connectButton->setProperty("connected", !source.IsEmpty());
            row.connectButton->setVisible(editable);
        }
    } else if (!row.pendingInput.IsEmpty()) {
        // A definition's input with no attribute: its default, dimmed, until edited.
        row.row->setUnauthored(true);
        row.row->setResettable(false);
        row.row->setAnimated(false);
        row.row->setConnectedSource(QString(), QString());
        if (row.connectButton) {
            const UsdPrim prim = currentPrim();
            row.connectButton->setVisible(prim && prim_actions::isEditable(prim));
        }
    } else if (!row.relationship.IsEmpty()) {
        const UsdPrim prim = currentPrim();
        const UsdRelationship rel = prim ? prim.GetRelationship(row.relationship) : UsdRelationship();
        row.row->setUnauthored(!rel || !rel.IsAuthored());
        SdfPathVector paths;
        if (rel) rel.GetTargets(&paths);
        const RelationshipTargetRules& rules = RelationshipTargetRules::instance();
        const std::string name = row.relationship.GetString();
        std::vector<properties::RelationshipTargetsEditor::Target> targets;
        for (const SdfPath& path : paths) {
            properties::RelationshipTargetsEditor::Target target{path, QString()};
            const UsdPrim targetPrim = m_stage->usdStage()->GetPrimAtPath(path.GetPrimPath());
            if (!targetPrim) {
                target.problem = tr("This prim doesn't exist");
            } else if (const RelationshipTargetVerdict verdict = rules.check(name, targetPrim);
                       verdict.kind != RelationshipTargetVerdict::Kind::Fine) {
                target.problem = qs(verdict.reason);
            }
            targets.push_back(std::move(target));
        }
        const std::optional<RelationshipTargetRule> rule = rules.find(name);
        auto* editor = static_cast<properties::RelationshipTargetsEditor*>(row.editor);
        editor->setReadOnly(prim && !prim_actions::isEditable(prim));
        editor->setTargets(targets, rule && rule->single);
    }
}

void PropertySheet::selectPrimLater(const SdfPath& path) {
    // Queued: selecting rebuilds this panel, the link or pill that asked included.
    QMetaObject::invokeMethod(
        this,
        [this, path]() {
            const SdfPath prim = path.GetPrimPath();
            if (m_stage && m_stage->usdStage()->GetPrimAtPath(prim)) {
                m_stageManager->setSelectedPaths(m_stage, {prim});
            }
        },
        Qt::QueuedConnection);
}

void PropertySheet::addConnectionButton(Row& row, const SdfPath& attribute, const SdfValueTypeName& pendingType) {
    row.connectButton = properties::iconButton(QStringLiteral(":/usdcc/theme/link.png"), tr("Connect\xE2\x80\xA6"));
    row.connectButton->setProperty("connectionButton", true);  // keeps the #propertyIconButton look
    row.row->addLastWidget(row.connectButton);  // the rightmost control
    QToolButton* button = row.connectButton;
    connect(button, &QToolButton::clicked, this, [this, button, attribute, pendingType]() {
        const bool connected = button->property("connected").toBool();
        // Queued: an edit can rebuild this panel, the button included.
        QMetaObject::invokeMethod(
            this,
            [this, attribute, connected, pendingType]() {
                if (connected) {
                    disconnectReceiver(attribute);
                } else {
                    connectInteractively(attribute, /*fromOutput=*/false, pendingType);
                }
            },
            Qt::QueuedConnection);
    });
}

void PropertySheet::buildShaderOutputEditor(Row& row, const SdfPath& output, const QString& label) {
    auto* editor = new properties::ConnectedInputsEditor();
    row.editor = editor;
    row.shaderOutput = true;
    row.row = new PropertyRow(label, editor);
    if (const UsdPrim prim = currentPrim()) m_connectionScope = connectionScope(prim).GetPath();
    connect(editor, &properties::ConnectedInputsEditor::selectRequested, this, &PropertySheet::selectPrimLater);
    connect(editor, &properties::ConnectedInputsEditor::disconnectRequested, this, [this](const SdfPath& input) {
        QMetaObject::invokeMethod(this, [this, input]() { disconnectReceiver(input); }, Qt::QueuedConnection);
    });
    connect(editor, &properties::ConnectedInputsEditor::connectRequested, this, [this, output]() {
        QMetaObject::invokeMethod(this, [this, output]() { connectInteractively(output, /*fromOutput=*/true); },
                                  Qt::QueuedConnection);
    });
}

void PropertySheet::connectInteractively(const SdfPath& attribute, bool fromOutput, const SdfValueTypeName& pendingType) {
    if (!m_stage) return;
    const UsdStageRefPtr usd = m_stage->usdStage();
    const UsdAttribute attr = usd->GetAttributeAtPath(attribute);
    const UsdPrim prim = usd->GetPrimAtPath(attribute.GetPrimPath());
    // A definition's input not authored yet, which only a receiver can be.
    const bool pending = !attr && !fromOutput && prim && pendingType;
    if (!attr && !pending) return;
    const ConnectionEnd end = pending ? ConnectionEnd(prim, attribute.GetNameToken(), pendingType) : ConnectionEnd(attr);
    ShaderConnectionDialog dialog(end,
                                  fromOutput ? ShaderConnectionDialog::Mode::PickReceiver : ShaderConnectionDialog::Mode::PickSource,
                                  this);
    if (dialog.exec() != QDialog::Accepted || dialog.selected().IsEmpty()) return;
    if (pending) {
        // Created and connected in one undo step.
        const UsdAttribute source = usd->GetAttributeAtPath(dialog.selected());
        if (!source) return;
        shade_actions::connect(*m_undoManager, m_stage,
                               {attribute.GetPrimPath(), attribute.GetNameToken(), pendingType.GetAsToken().GetString()},
                               {source.GetPrim().GetPath(), source.GetName(), source.GetTypeName().GetAsToken().GetString()});
        return;
    }
    if (fromOutput) {
        prim_actions::connectSource(*m_undoManager, m_stage, dialog.selected(), attribute);
    } else {
        prim_actions::connectSource(*m_undoManager, m_stage, attribute, dialog.selected());
    }
}

void PropertySheet::disconnectReceiver(const SdfPath& receiver) {
    if (m_stage) prim_actions::disconnectSource(*m_undoManager, m_stage, receiver);
}

void PropertySheet::buildRelationshipEditor(Row& row, const TfToken& name, const QString& label) {
    auto* editor = new properties::RelationshipTargetsEditor();
    row.editor = editor;
    row.relationship = name;
    row.row = new PropertyRow(label, editor);
    // Queued: each of these rebuilds this panel, the pill included.
    connect(editor, &properties::RelationshipTargetsEditor::selectRequested, this, &PropertySheet::selectPrimLater);
    connect(editor, &properties::RelationshipTargetsEditor::removeRequested, this, [this, name](const SdfPath& path) {
        QMetaObject::invokeMethod(
            this,
            [this, name, path]() {
                if (const UsdPrim prim = currentPrim()) {
                    prim_actions::removeRelationshipTarget(*m_undoManager, m_stage, prim.GetPath(), name, path);
                }
            },
            Qt::QueuedConnection);
    });
    connect(editor, &properties::RelationshipTargetsEditor::addRequested, this, [this, name]() {
        QMetaObject::invokeMethod(this, [this, name]() { addRelationshipTargetInteractively(name); }, Qt::QueuedConnection);
    });
}

void PropertySheet::addRelationshipTargetInteractively(const TfToken& name) {
    const UsdPrim prim = currentPrim();
    if (!prim) return;
    const std::optional<RelationshipTargetRule> rule = RelationshipTargetRules::instance().find(name.GetString());
    const bool single = rule && rule->single;
    SdfPathVector current;
    if (const UsdRelationship rel = prim.GetRelationship(name); rel && single) rel.GetTargets(&current);
    RelationshipTargetDialog dialog(m_stage->usdStage(), name.GetString(),
                                    current.empty() ? SdfPath() : current.front(), this);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedPath().IsEmpty()) return;
    prim_actions::addRelationshipTarget(*m_undoManager, m_stage, prim.GetPath(), name, dialog.selectedPath(), single);
}

void PropertySheet::refreshValues() {
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

void PropertySheet::applyFilter() {
    const QString text = m_searchText.trimmed().toLower();
    std::vector<int> visibleCounts(m_sections.size(), 0);
    std::set<QWidget*> framesInUse;
    for (const Row& row : m_rows) {
        if (row.frame && (text.isEmpty() || row.searchText.contains(text))) framesInUse.insert(row.frame);
    }
    for (Row& row : m_rows) {
        const bool visible = text.isEmpty() || row.searchText.contains(text);
        row.row->setVisible(visible);
        if (row.frame) row.frame->setVisible(framesInUse.count(row.frame) > 0);
        if (visible) {
            for (size_t i = 0; i < m_sections.size(); ++i) {
                if (m_sections[i] == row.section) ++visibleCounts[i];
            }
        }
    }
    for (size_t i = 0; i < m_sections.size(); ++i) {
        // Only a search hides a section: with none, one with no rows is still there
        // for what's in its header, such as Transform's + with no ops yet.
        m_sections[i]->setVisible(text.isEmpty() || visibleCounts[i] > 0);
        m_sections[i]->setForceExpanded(!text.isEmpty());
    }
    if (m_primSection) {
        m_primSection->setVisible(text.isEmpty());
    }
    if (m_compositionSection) {
        m_compositionSection->setVisible(text.isEmpty());
    }
}

void PropertySheet::onStageObjectsChanged(const StageChanges& changes) {
    if (m_primPath.IsEmpty()) {
        return;
    }
    // A shader output's pills list inputs on other prims of its material.
    const auto underScope = [this](const SdfPathVector& paths) {
        return std::any_of(paths.begin(), paths.end(), [this](const SdfPath& path) { return path.HasPrefix(m_connectionScope); });
    };
    if (changes.resyncsPrim(m_primPath)) {
        m_rebuildPending = true;
    } else if (changes.affectsPrim(m_primPath)) {
        m_refreshPending = true;
    } else if (!m_connectionScope.IsEmpty() &&
               (underScope(changes.changedInfoOnlyPaths) || underScope(changes.resyncedPaths))) {
        m_refreshPending = true;
    } else {
        return;
    }
    queueProcessPendingChanges();
}

void PropertySheet::queueProcessPendingChanges() {
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

bool PropertySheet::anyEditing() const {
    for (const Row& row : m_rows) {
        if (row.binding && row.binding->isEditing()) return true;
    }
    return false;
}

void PropertySheet::processPendingChanges() {
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

std::vector<QWidget*> PropertySheet::tabStops(QWidget* including) const {
    if (!m_content) return {};
    struct Stop {
        QWidget* widget;
        int centerY;
        int left;
    };
    std::vector<Stop> stops;
    for (QWidget* widget : m_content->findChildren<QWidget*>()) {
        if (widget != including && !isTabStop(widget)) continue;
        // Not in a collapsed section, a closed matrix grid or a row a search hides.
        if (!widget->isVisibleTo(m_content)) continue;
        const QPoint topLeft = widget->mapTo(m_content, QPoint(0, 0));
        stops.push_back({widget, topLeft.y() + widget->height() / 2, topLeft.x()});
    }
    // Reading order: top to bottom by line, then left to right. Fields on one line
    // can differ a little in height, so a line is everything whose middle is within
    // a few pixels of its first field's.
    std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.centerY < b.centerY; });
    const int tolerance = usdcc::ui::scaled::pixels(6);
    for (auto line = stops.begin(); line != stops.end();) {
        auto end = std::find_if(line, stops.end(), [&](const Stop& s) { return s.centerY - line->centerY > tolerance; });
        std::sort(line, end, [](const Stop& a, const Stop& b) { return a.left < b.left; });
        line = end;
    }
    std::vector<QWidget*> ordered;
    ordered.reserve(stops.size());
    for (const Stop& stop : stops) ordered.push_back(stop.widget);
    return ordered;
}

bool PropertySheet::focusNextPrevChild(bool next) {
    QWidget* focus = QApplication::focusWidget();
    if (!focus) focus = window()->focusWidget();
    // Qt also calls this when the focused widget is hidden (QWidget::hide()), to move the
    // focus off it: only a Tab key, from a widget still on screen, moves to a field.
    if (!m_content || !focus || !m_content->isAncestorOf(focus) || !focus->isVisible()) {
        return QWidget::focusNextPrevChild(next);
    }

    // Where Tab moves on from: the field being typed in, or whatever else has the
    // focus (a check box, a combo box), placed among the fields.
    QWidget* from = owningTabStop(focus, m_content);
    if (!from) from = focus;
    const std::vector<QWidget*> stops = tabStops(from);
    const auto at = std::find(stops.begin(), stops.end(), from);
    if (at == stops.end()) return QWidget::focusNextPrevChild(next);
    const int count = static_cast<int>(stops.size());
    if (count > 1) {
        // The neighbour, wrapping round; every entry but `from` is a field.
        const int index = static_cast<int>(at - stops.begin());
        QWidget* target = stops[static_cast<size_t>((index + (next ? 1 : count - 1)) % count)];
        // Taking the focus commits the field being left (its focus-out); the edit
        // updates the rows later (queued), so `target` stays.
        activateTabStop(target);
        m_scroll->ensureWidgetVisible(target);
        return true;
    }
    // The only field: commit it by leaving it and starting again.
    if (isTabStop(from)) {
        focus->clearFocus();
        activateTabStop(from);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

}  // namespace usdcc::usd
