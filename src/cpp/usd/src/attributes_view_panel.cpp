#include "usdcc/usd/attributes_view_panel.h"

#include "usdcc/ui/settings.h"
#include "usdcc/usd/properties/attribute_binding.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/properties/transform_stack_editor.h"
#include "usdcc/usd/property_categorizer.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QAction>
#include <QComboBox>
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
    toolbarLayout->setContentsMargins(10, 4, 10, 4);

    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Search attributes"));
    m_search->setClearButtonEnabled(true);

    m_showButton = new QToolButton(toolbar);
    m_showButton->setText(tr("Show"));
    m_showButton->setPopupMode(QToolButton::InstantPopup);
    auto* showMenu = new QMenu(m_showButton);
    m_authoredOnly = showMenu->addAction(tr("Authored Only"));
    m_authoredOnly->setCheckable(true);
    m_showHidden = showMenu->addAction(tr("Hidden Properties"));
    m_showHidden->setCheckable(true);
    m_showButton->setMenu(showMenu);
    {
        QSettings settings = usdcc::ui::appSettings();
        m_authoredOnly->setChecked(settings.value(kAuthoredOnlyKey, false).toBool());
        m_showHidden->setChecked(settings.value(kShowHiddenKey, false).toBool());
    }

    toolbarLayout->addWidget(new QLabel(tr("Stage:"), toolbar));
    toolbarLayout->addWidget(stageCombo(), 1);
    toolbarLayout->addWidget(m_search, 1);
    toolbarLayout->addWidget(m_showButton);

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

void AttributesViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_stageChangedConnection =
        stage->changed.connect([this](const StageChanges& changes) { onStageObjectsChanged(changes); });
    onStageSelectionChanged(stage, stageManager()->selectedPaths(stage));
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
    return m_primPath.IsEmpty() ? UsdPrim() : currentStage()->usdStage()->GetPrimAtPath(m_primPath);
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
    const CategorizeOptions options{m_authoredOnly->isChecked(), m_showHidden->isChecked()};
    for (const PropertyCategory& category : categorizeProperties(prim, options)) {
        signature += "|" + category.key + ":";
        for (const UsdProperty& property : category.properties) {
            signature += property.GetName().GetString() + ",";
        }
    }
    return signature;
}

CategorySection* AttributesViewPanel::makeSection(const QString& key, const QString& title) {
    auto* section = new CategorySection(key, title);
    QSettings settings = usdcc::ui::appSettings();
    section->setCollapsed(settings.value(kCollapsedKeyPrefix + key, false).toBool());
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
        auto* empty = new QLabel(m_primPath.IsEmpty() ? tr("Select a prim to see its attributes.")
                                                      : tr("%1 no longer exists.").arg(qs(m_primPath.GetString())),
                                 m_content);
        empty->setObjectName(QStringLiteral("propertyNote"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setContentsMargins(0, 24, 0, 0);
        layout->addWidget(empty);
        layout->addStretch(1);
        m_scroll->setWidget(m_content);
        return;
    }

    buildPrimSection(prim, layout);

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
            if (category.key == "transform") {
                row.row->setNote(tr("not in stack"));  // an xformOp:* attribute xformOpOrder doesn't use
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

void AttributesViewPanel::buildPrimSection(const UsdPrim& prim, QVBoxLayout* layout) {
    m_primSection = makeSection(QStringLiteral("prim"), tr("Prim"));
    auto addRow = [this](const QString& label, PropertyEditor* editor) {
        auto* row = new PropertyRow(label, editor);
        row->setLabelWidth(kLabelWidth);
        m_primSection->addRow(row);
        return editor;
    };

    m_header.path = addRow(tr("Path"), readOnlyText(QString()));
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

void AttributesViewPanel::refreshPrimHeader(const UsdPrim& prim) {
    if (!m_primSection || !prim) return;
    m_header.path->setValue(VtValue(prim.GetPath().GetString()));
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
        m_sections[i]->setCount(visibleCounts[i]);
        m_sections[i]->setForceExpanded(!text.isEmpty());
    }
    if (m_primSection) {
        m_primSection->setVisible(text.isEmpty());
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
