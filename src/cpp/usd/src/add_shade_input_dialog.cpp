#include "usdcc/usd/add_shade_input_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/value_type_info.h"

#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/tokens.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QMenu>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }

// What a type is, for its tooltip: the C++ type and the role, as the data-type gallery says it.
QString describe(const ValueTypeInfo& info) {
    QString text = qs(info.typeName.GetType().GetTypeName());
    if (!info.role.IsEmpty()) text += QStringLiteral(" \xC2\xB7 ") + qs(info.role.GetString());
    return text;
}

QAction* addType(QMenu* menu, const SdfValueTypeName& type) {
    const QString token = qs(type.GetAsToken().GetString());
    QAction* action = menu->addAction(token);
    action->setData(token);
    action->setCheckable(true);
    action->setToolTip(describe(ValueTypeInfo::classify(type)));
    return action;
}

// Like a context menu, not a long list: the types a shading network uses most, then a submenu
// per group, in the order a shader's inputs need them.
void populateTypes(QMenu* menu) {
    menu->setToolTipsVisible(true);
    const SdfSchemaBase& schema = SdfSchema::GetInstance();
    for (const char* name : {"float", "color3f", "float2", "float3", "normal3f", "int", "bool", "string", "token", "asset"}) {
        if (const SdfValueTypeName type = schema.FindType(name)) addType(menu, type);
    }
    menu->addSeparator();
    const std::vector<SdfValueTypeName> types = canonicalValueTypes(false);
    for (TypeGroup group : {TypeGroup::Scalars, TypeGroup::Colors, TypeGroup::Vectors, TypeGroup::PointsNormalsVectors,
                            TypeGroup::TextureCoordinates, TypeGroup::StringsAndPaths, TypeGroup::Matrices,
                            TypeGroup::Quaternions, TypeGroup::Other}) {
        QMenu* submenu = nullptr;
        for (const SdfValueTypeName& type : types) {
            if (ValueTypeInfo::classify(type).group != group) continue;
            if (!submenu) {
                submenu = menu->addMenu(AddShadeInputDialog::tr(typeGroupLabel(group)));
                submenu->setToolTipsVisible(true);
            }
            addType(submenu, type);
        }
    }
}

// Every type action of `menu` and its submenus.
void forEachTypeAction(QMenu* menu, const std::function<void(QAction*)>& visit) {
    for (QAction* action : menu->actions()) {
        if (QMenu* submenu = action->menu()) {
            forEachTypeAction(submenu, visit);
        } else if (!action->isSeparator()) {
            visit(action);
        }
    }
}

// "inputs:x" typed whole is taken as "x".
std::string baseName(const QString& text) {
    std::string name = text.trimmed().toStdString();
    const std::string prefix = UsdShadeTokens->inputs.GetString();
    if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
    return name;
}
}  // namespace

AddShadeInputDialog::AddShadeInputDialog(const UsdPrim& prim, QWidget* parent) : QDialog(parent), m_prim(prim) {
    setWindowTitle(tr("Add Input to %1").arg(qs(prim.GetName().GetString())));
    setObjectName(QStringLiteral("addShadeInputDialog"));

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("addInputName"));
    m_name->setPlaceholderText(tr("e.g. baseColor"));

    // The types as the Attribute Editor knows them, in a menu; arrays by the checkbox.
    m_type = new QPushButton(this);
    m_type->setObjectName(QStringLiteral("addInputType"));
    m_type->setStyleSheet(QStringLiteral("text-align: left;"));
    m_typeMenu = new QMenu(m_type);
    m_typeMenu->setObjectName(QStringLiteral("addInputTypeMenu"));
    populateTypes(m_typeMenu);
    m_type->setMenu(m_typeMenu);
    forEachTypeAction(m_typeMenu, [this](QAction* action) {
        connect(action, &QAction::triggered, this, [this, action]() { setScalarType(action->data().toString()); });
    });
    m_scalarType = QStringLiteral("float");
    m_type->setText(m_scalarType);
    forEachTypeAction(m_typeMenu, [this](QAction* action) { action->setChecked(action->data().toString() == m_scalarType); });
    m_array = new QCheckBox(tr("Array"), this);
    m_array->setObjectName(QStringLiteral("addInputArray"));
    auto* typeRow = new QHBoxLayout;
    typeRow->addWidget(m_type, 1);
    typeRow->addWidget(m_array);

    m_allowedTokens = new QLineEdit(this);
    m_allowedTokens->setObjectName(QStringLiteral("addInputAllowedTokens"));
    m_allowedTokens->setPlaceholderText(tr("Optional, comma separated: a, b, c"));

    m_setDefault = new QCheckBox(this);
    m_setDefault->setObjectName(QStringLiteral("addInputSetDefault"));
    m_setDefault->setChecked(true);
    m_setDefault->setToolTip(tr("Author a default value; without one the input has none until it's set"));
    m_valueHolder = new QWidget(this);
    auto* valueLayout = new QHBoxLayout(m_valueHolder);
    valueLayout->setContentsMargins(0, 0, 0, 0);
    valueLayout->addWidget(m_setDefault);

    m_connectability = new QComboBox(this);
    m_connectability->setObjectName(QStringLiteral("addInputConnectability"));
    m_connectability->addItem(tr("Full"), qs(UsdShadeTokens->full.GetString()));
    m_connectability->addItem(tr("Interface only"), qs(UsdShadeTokens->interfaceOnly.GetString()));
    m_connectability->setItemData(0, tr("Can be connected to any output or input"), Qt::ToolTipRole);
    m_connectability->setItemData(1, tr("Can only be connected to an input of a Material's or NodeGraph's interface"), Qt::ToolTipRole);

    m_displayName = new QLineEdit(this);
    m_displayName->setObjectName(QStringLiteral("addInputDisplayName"));
    m_displayName->setPlaceholderText(tr("Optional: the label the editors show"));
    m_displayGroup = new QLineEdit(this);
    m_displayGroup->setObjectName(QStringLiteral("addInputDisplayGroup"));
    m_displayGroup->setPlaceholderText(tr("Optional: the page it's shown on"));
    QStringList groups;
    for (const UsdShadeInput& input : UsdShadeConnectableAPI(prim).GetInputs()) {
        const std::string existing = input.GetDisplayGroup();
        if (!existing.empty() && !groups.contains(qs(existing))) groups << qs(existing);
    }
    if (!groups.isEmpty()) {
        groups.sort(Qt::CaseInsensitive);
        auto* completer = new QCompleter(groups, m_displayGroup);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        m_displayGroup->setCompleter(completer);
    }
    m_documentation = new QPlainTextEdit(this);
    m_documentation->setObjectName(QStringLiteral("addInputDocumentation"));
    m_documentation->setPlaceholderText(tr("Optional: shown as its tooltip"));
    m_documentation->setTabChangesFocus(true);
    m_documentation->setFixedHeight(QFontMetrics(m_documentation->font()).lineSpacing() * 3 + 12);

    m_problem = new QLabel(this);
    m_problem->setObjectName(QStringLiteral("propertyNote"));
    m_problem->setWordWrap(true);

    m_form = new QFormLayout;
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_form->addRow(tr("Name"), m_name);
    m_form->addRow(tr("Type"), typeRow);
    m_form->addRow(tr("Allowed tokens"), m_allowedTokens);
    m_form->addRow(tr("Default value"), m_valueHolder);
    m_form->addRow(tr("Connectability"), m_connectability);
    m_form->addRow(tr("Display name"), m_displayName);
    m_form->addRow(tr("Display group"), m_displayGroup);
    m_form->addRow(tr("Documentation"), m_documentation);

    auto* buttons = new QDialogButtonBox(this);
    m_add = buttons->addButton(tr("Add Input"), QDialogButtonBox::AcceptRole);
    m_add->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_add->isEnabled()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    usdcc::ui::scaled::margins(layout, 12, 12, 12, 10);
    layout->addLayout(m_form);
    layout->addWidget(m_problem);
    layout->addStretch(1);
    layout->addWidget(buttons);
    usdcc::ui::scaled::minimumWidth(this, 460);

    connect(m_name, &QLineEdit::textChanged, this, [this]() { updateState(); });
    connect(m_array, &QCheckBox::toggled, this, [this]() { rebuildValueEditor(); });
    connect(m_allowedTokens, &QLineEdit::editingFinished, this, [this]() { rebuildValueEditor(); });
    connect(m_setDefault, &QCheckBox::toggled, this, [this]() { updateState(); });

    rebuildValueEditor();
    m_name->setFocus();
}

bool AddShadeInputDialog::setScalarType(const QString& token) {
    const SdfValueTypeName type = SdfSchema::GetInstance().FindType(token.toStdString());
    if (!type || type.IsArray()) return false;
    const QString name = qs(type.GetAsToken().GetString());
    // A type in both the common ones and its group has two actions: both show it picked.
    forEachTypeAction(m_typeMenu, [&name](QAction* action) { action->setChecked(action->data().toString() == name); });
    if (name == m_scalarType) return true;
    m_scalarType = name;
    m_type->setText(name);
    rebuildValueEditor();
    return true;
}

SdfValueTypeName AddShadeInputDialog::typeName() const {
    const SdfValueTypeName scalar = SdfSchema::GetInstance().FindType(m_scalarType.toStdString());
    return m_array->isChecked() ? scalar.GetArrayType() : scalar;
}

std::vector<TfToken> AddShadeInputDialog::allowedTokens() const {
    std::vector<TfToken> tokens;
    if (typeName().GetScalarType() != SdfValueTypeNames->Token) return tokens;
    for (const QString& part : m_allowedTokens->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const TfToken token(part.trimmed().toStdString());
        if (!token.IsEmpty() && std::find(tokens.begin(), tokens.end(), token) == tokens.end()) tokens.push_back(token);
    }
    return tokens;
}

void AddShadeInputDialog::rebuildValueEditor() {
    const SdfValueTypeName type = typeName();
    if (!type) return;
    const VtValue old = m_value ? m_value->value() : VtValue();
    delete m_value;
    m_value = nullptr;

    properties::EditorHints hints;
    hints.allowedTokens = allowedTokens();
    if (const UsdStageRefPtr stage = m_prim.GetStage()) {
        hints.editLayer = stage->GetEditTarget().GetLayer();
        hints.anchorLayer = hints.editLayer;
    }
    m_value = properties::createPropertyEditor(type, hints, m_valueHolder);
    m_value->setObjectName(QStringLiteral("addInputValue"));
    VtValue start = type.GetDefaultValue();
    if (!old.IsEmpty() && old.GetType() == start.GetType()) start = old;
    if (!hints.allowedTokens.empty() && start.IsHolding<TfToken>() &&
        std::find(hints.allowedTokens.begin(), hints.allowedTokens.end(), start.UncheckedGet<TfToken>()) == hints.allowedTokens.end()) {
        start = VtValue(hints.allowedTokens.front());
    }
    m_value->setValue(start);
    m_valueHolder->layout()->addWidget(m_value);
    static_cast<QHBoxLayout*>(m_valueHolder->layout())->setStretchFactor(m_value, 1);

    const bool tokens = type.GetScalarType() == SdfValueTypeNames->Token;
    m_form->setRowVisible(m_allowedTokens, tokens);
    // `opaque` and `group` hold no value.
    const bool valued = ValueTypeInfo::classify(type).scalarKind != ScalarKind::Opaque;
    m_setDefault->setEnabled(valued);
    if (!valued) m_setDefault->setChecked(false);
    updateState();
}

void AddShadeInputDialog::updateState() {
    if (m_value) m_value->setEnabled(m_setDefault->isChecked());
    const std::string name = baseName(m_name->text());
    const std::string problem = shade_actions::inputNameProblem(m_prim, name);
    m_add->setEnabled(problem.empty() && typeName());
    // Not scolded for a name not typed yet.
    m_problem->setText(name.empty() ? QString() : qs(problem));
    m_problem->setVisible(!m_problem->text().isEmpty());
}

shade_actions::InputSpec AddShadeInputDialog::spec() const {
    shade_actions::InputSpec spec;
    spec.name = baseName(m_name->text());
    spec.typeName = typeName().GetAsToken().GetString();
    if (m_setDefault->isChecked() && m_value) spec.value = m_value->value();
    spec.connectability = TfToken(m_connectability->currentData().toString().toStdString());
    spec.displayName = m_displayName->text().trimmed().toStdString();
    spec.displayGroup = m_displayGroup->text().trimmed().toStdString();
    spec.documentation = m_documentation->toPlainText().trimmed().toStdString();
    spec.allowedTokens = allowedTokens();
    return spec;
}

bool addShadeInputInteractively(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, QWidget* parent) {
    if (!stage) return false;
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!shade_actions::canAddInput(prim)) return false;
    AddShadeInputDialog dialog(prim, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    return shade_actions::addInput(undo, stage, path, dialog.spec());
}

}  // namespace usdcc::usd
