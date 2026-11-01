#include "usdcc/usd/properties/property_editor.h"

#include "usdcc/usd/properties/array_editor_dialog.h"
#include "usdcc/usd/properties/color_swatch.h"
#include "usdcc/usd/properties/number_field.h"
#include "usdcc/usd/properties/property_style.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/pathExpression.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/attributeLimits.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace usdcc::usd::properties {

using namespace PXR_NS;

// ---- Hints / base ---------------------------------------------------------

namespace {
std::optional<std::pair<double, double>> rangeOf(const UsdAttributeLimits& limits) {
    if (!limits || !limits.HasAuthoredMinimum() || !limits.HasAuthoredMaximum()) {
        return std::nullopt;
    }
    const std::vector<double> lo = numericComponents(limits.GetMinimum());
    const std::vector<double> hi = numericComponents(limits.GetMaximum());
    if (lo.size() != 1 || hi.size() != 1 || !(hi[0] > lo[0])) {
        return std::nullopt;
    }
    return std::make_pair(lo[0], hi[0]);
}
}  // namespace

EditorHints EditorHints::forAttribute(const UsdAttribute& attribute) {
    EditorHints hints;
    if (!attribute) {
        return hints;
    }
    VtTokenArray allowed;
    if (attribute.GetMetadata(SdfFieldKeys->AllowedTokens, &allowed)) {
        hints.allowedTokens.assign(allowed.begin(), allowed.end());
    }
    hints.softRange = rangeOf(attribute.GetSoftLimits());
    hints.hardRange = rangeOf(attribute.GetHardLimits());
    // The layer with the strongest opinion is what an authored asset path is
    // relative to.
    for (const auto& spec : attribute.GetPropertyStack()) {
        if (spec) {
            hints.anchorLayer = spec->GetLayer();
            break;
        }
    }
    if (!hints.anchorLayer) {
        hints.anchorLayer = attribute.GetStage()->GetEditTarget().GetLayer();
    }
    return hints;
}

PropertyEditor::PropertyEditor(ValueTypeInfo info, QWidget* parent) : QWidget(parent), m_info(std::move(info)) {}

void PropertyEditor::setReadOnly(bool readOnly) { m_readOnly = readOnly; }

void PropertyEditor::emitSingleEdit(const VtValue& value) {
    Q_EMIT editStarted();
    Q_EMIT valueEdited(value);
    Q_EMIT editFinished();
}

namespace {

QHBoxLayout* rowLayout(QWidget* parent, int spacing = 4) {
    auto* layout = new QHBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    return layout;
}

// A label showing a value must never widen its row: let it shrink (clipping
// the text; the full text is in its tooltip) instead of growing to fit.
QLabel* valueLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setMinimumWidth(0);
    return label;
}

QColor labelColor(const std::string& label, bool quaternion) {
    if (quaternion && label == "W") return style::axisW();
    if (label == "X" || label == "R" || label == "U") return style::axisX();
    if (label == "Y" || label == "G" || label == "V") return style::axisY();
    if (label == "Z" || label == "B" || (label == "W" && !quaternion)) return style::axisZ();
    return style::axisW();  // A
}

// Range and stepping for one number of the given kind.
void configureNumber(NumberField* field, const ValueTypeInfo& info, int decimals) {
    switch (info.scalarKind) {
        case ScalarKind::UChar:
            field->setRange(0, 255);
            break;
        case ScalarKind::UInt:
            field->setRange(0, std::numeric_limits<unsigned int>::max());
            break;
        case ScalarKind::UInt64:
            field->setRange(0, static_cast<double>(std::numeric_limits<uint64_t>::max()));
            break;
        case ScalarKind::Int:
            field->setRange(std::numeric_limits<int>::lowest(), std::numeric_limits<int>::max());
            break;
        case ScalarKind::Half:
            field->setRange(-65504.0, 65504.0);
            break;
        default:
            break;
    }
    field->setInteger(info.isInteger());
    field->setDecimals(decimals);
    field->setStep(info.isInteger() ? 0.1 : 0.01);
}

// ---- Bool -----------------------------------------------------------------

class BoolEditor : public PropertyEditor {
public:
    BoolEditor(const ValueTypeInfo& info, QWidget* parent) : PropertyEditor(info, parent) {
        auto* layout = rowLayout(this);
        m_check = new QCheckBox(this);
        layout->addWidget(m_check);
        layout->addStretch(1);
        connect(m_check, &QCheckBox::clicked, this, [this](bool on) { emitSingleEdit(VtValue(on)); });
    }
    void setValue(const VtValue& value) override {
        m_check->setChecked(value.IsHolding<bool>() && value.UncheckedGet<bool>());
    }
    VtValue value() const override { return VtValue(m_check->isChecked()); }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        m_check->setEnabled(!readOnly);
    }

private:
    QCheckBox* m_check;
};

// ---- Numbers: scalars, vectors, quaternions, colors --------------------------

class NumericEditor : public PropertyEditor {
public:
    NumericEditor(const ValueTypeInfo& info, const EditorHints& hints, QWidget* parent)
        : PropertyEditor(info, parent), m_numbers(static_cast<size_t>(info.numberCount()), 0.0) {
        auto* layout = rowLayout(this);
        const bool isColor = info.role == SdfValueRoleNames->Color;
        if (isColor) {
            m_swatch = new ColorSwatch(this);
            m_swatch->setShowAlpha(info.components == 4);
            layout->addWidget(m_swatch);
            connect(m_swatch, &ColorSwatch::editStarted, this, &PropertyEditor::editStarted);
            connect(m_swatch, &ColorSwatch::editFinished, this, &PropertyEditor::editFinished);
            connect(m_swatch, &ColorSwatch::editCanceled, this, &PropertyEditor::editCanceled);
            connect(m_swatch, &ColorSwatch::colorEdited, this, [this](const QColor& c) {
                m_numbers[0] = ColorSwatch::displayToLinear(c.redF());
                m_numbers[1] = ColorSwatch::displayToLinear(c.greenF());
                m_numbers[2] = ColorSwatch::displayToLinear(c.blueF());
                if (m_numbers.size() == 4) {
                    m_numbers[3] = c.alphaF();
                }
                syncFields();
                Q_EMIT valueEdited(makeNumericValue(m_info.scalarTypeName, m_numbers));
            });
        }

        const std::vector<std::string> labels = info.componentLabels();
        const bool scalar = info.shape == ValueShape::Scalar;
        const int decimals = scalar ? 3 : 2;
        for (int i = 0; i < info.numberCount(); ++i) {
            auto* field = new NumberField(this);
            configureNumber(field, info, decimals);
            if (i < static_cast<int>(labels.size())) {
                field->setPrefix(QString::fromStdString(labels[i]),
                                 labelColor(labels[i], info.shape == ValueShape::Quaternion));
            }
            if (scalar) {
                field->setSuffix(hints.suffix);
                if (hints.hardRange) {
                    field->setRange(hints.hardRange->first, hints.hardRange->second);
                }
                if (hints.softRange) {
                    field->setSliderRange(hints.softRange->first, hints.softRange->second);
                }
            } else if (!hints.suffix.isEmpty()) {
                field->setSuffix(hints.suffix);
            }
            layout->addWidget(field, 1);
            m_fields.push_back(field);
            connect(field, &NumberField::editStarted, this, &PropertyEditor::editStarted);
            connect(field, &NumberField::editFinished, this, &PropertyEditor::editFinished);
            connect(field, &NumberField::editCanceled, this, &PropertyEditor::editCanceled);
            connect(field, &NumberField::valueEdited, this, [this, i](double v) {
                m_numbers[static_cast<size_t>(i)] = v;
                syncSwatch();
                Q_EMIT valueEdited(makeNumericValue(m_info.scalarTypeName, m_numbers));
            });
        }
    }

    void setValue(const VtValue& value) override {
        const std::vector<double> numbers = numericComponents(value);
        if (numbers.size() == m_numbers.size()) {
            m_numbers = numbers;
        }
        syncFields();
        syncSwatch();
    }
    VtValue value() const override { return makeNumericValue(m_info.scalarTypeName, m_numbers); }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        for (auto* f : m_fields) f->setReadOnly(readOnly);
        if (m_swatch) m_swatch->setReadOnly(readOnly);
    }

private:
    void syncFields() {
        for (size_t i = 0; i < m_fields.size(); ++i) m_fields[i]->setValue(m_numbers[i]);
    }
    void syncSwatch() {
        if (!m_swatch) return;
        QColor c;
        c.setRgbF(static_cast<float>(ColorSwatch::linearToDisplay(m_numbers[0])),
                  static_cast<float>(ColorSwatch::linearToDisplay(m_numbers[1])),
                  static_cast<float>(ColorSwatch::linearToDisplay(m_numbers[2])),
                  m_numbers.size() == 4 ? static_cast<float>(std::clamp(m_numbers[3], 0.0, 1.0)) : 1.0f);
        m_swatch->setColor(c);
    }

    std::vector<double> m_numbers;
    std::vector<NumberField*> m_fields;
    ColorSwatch* m_swatch = nullptr;
};

// ---- Matrices ---------------------------------------------------------------

class MatrixEditor : public PropertyEditor {
public:
    MatrixEditor(const ValueTypeInfo& info, QWidget* parent)
        : PropertyEditor(info, parent), m_numbers(static_cast<size_t>(info.numberCount()), 0.0) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);

        auto* header = new QWidget(this);
        auto* headerLayout = rowLayout(header);
        m_toggle = new QToolButton(header);
        m_toggle->setObjectName(QStringLiteral("propertyDisclosure"));
        m_toggle->setCheckable(true);
        m_toggle->setAutoRaise(true);
        m_toggle->setArrowType(Qt::RightArrow);
        m_toggle->setToolTip(tr("Show all %1 values").arg(info.numberCount()));
        m_summary = valueLabel(header);
        m_summary->setObjectName(QStringLiteral("propertyValueText"));
        headerLayout->addWidget(m_toggle);
        headerLayout->addWidget(m_summary, 1);
        layout->addWidget(header);

        m_grid = new QWidget(this);
        auto* grid = new QGridLayout(m_grid);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(3);
        const int n = info.components;
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < n; ++c) {
                auto* field = new NumberField(m_grid);
                configureNumber(field, info, 3);
                grid->addWidget(field, r, c);
                const size_t index = static_cast<size_t>(r * n + c);
                m_fields.push_back(field);
                connect(field, &NumberField::editStarted, this, &PropertyEditor::editStarted);
                connect(field, &NumberField::editFinished, this, &PropertyEditor::editFinished);
                connect(field, &NumberField::editCanceled, this, &PropertyEditor::editCanceled);
                connect(field, &NumberField::valueEdited, this, [this, index](double v) {
                    m_numbers[index] = v;
                    updateSummary();
                    Q_EMIT valueEdited(makeNumericValue(m_info.scalarTypeName, m_numbers));
                });
            }
        }
        m_grid->setVisible(false);
        layout->addWidget(m_grid);
        connect(m_toggle, &QToolButton::toggled, this, [this](bool open) {
            m_grid->setVisible(open);
            m_toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        });
    }

    void setValue(const VtValue& value) override {
        const std::vector<double> numbers = numericComponents(value);
        if (numbers.size() == m_numbers.size()) m_numbers = numbers;
        for (size_t i = 0; i < m_fields.size(); ++i) m_fields[i]->setValue(m_numbers[i]);
        updateSummary();
    }
    VtValue value() const override { return makeNumericValue(m_info.scalarTypeName, m_numbers); }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        for (auto* f : m_fields) f->setReadOnly(readOnly);
    }

private:
    void updateSummary() {
        const int n = m_info.components;
        bool identity = true;
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
                identity = identity && m_numbers[static_cast<size_t>(r * n + c)] == (r == c ? 1.0 : 0.0);
        QString text;
        if (identity) {
            text = tr("Identity");
        } else if (n == 4) {
            // A transform's translation is its last row; show that, it's what
            // tells matrices apart at a glance.
            text = tr("Translate %1").arg(QString::fromStdString(formatValue(
                VtValue(GfVec3d(m_numbers[12], m_numbers[13], m_numbers[14])), 2)));
        } else {
            text = QString::fromStdString(formatValue(value(), 2));
        }
        m_summary->setText(text);
    }

    std::vector<double> m_numbers;
    std::vector<NumberField*> m_fields;
    QToolButton* m_toggle;
    QLabel* m_summary;
    QWidget* m_grid;
};

// ---- Text: string, token, path expression ----------------------------------------

class TextEditor : public PropertyEditor {
public:
    TextEditor(const ValueTypeInfo& info, QWidget* parent) : PropertyEditor(info, parent) {
        auto* layout = rowLayout(this);
        m_edit = new QLineEdit(this);
        layout->addWidget(m_edit, 1);
        if (info.scalarKind == ScalarKind::PathExpression) {
            m_edit->setPlaceholderText(tr("e.g. /World/** or //Mesh"));
        }
        connect(m_edit, &QLineEdit::editingFinished, this, [this]() { commit(); });
        connect(m_edit, &QLineEdit::textEdited, this, [this]() { setInvalid(false); });
    }

    void setValue(const VtValue& value) override {
        m_text = QString::fromStdString(formatValue(value));
        m_edit->setText(m_text);
        setInvalid(false);
    }
    VtValue value() const override { return toValue(m_text); }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        m_edit->setReadOnly(readOnly);
    }

private:
    VtValue toValue(const QString& text) const {
        const std::string s = text.toStdString();
        switch (m_info.scalarKind) {
            case ScalarKind::Token:
                return VtValue(TfToken(s));
            case ScalarKind::PathExpression: {
                TfErrorMark mark;
                SdfPathExpression expression(s);
                const bool valid = mark.IsClean() && (!expression.IsEmpty() || s.empty());
                mark.Clear();
                return valid ? VtValue(expression) : VtValue();
            }
            default:
                return VtValue(s);
        }
    }
    void commit() {
        const QString text = m_edit->text();
        if (m_readOnly || text == m_text) return;
        const VtValue v = toValue(text);
        if (v.IsEmpty()) {
            setInvalid(true);
            return;
        }
        m_text = text;
        emitSingleEdit(v);
    }
    void setInvalid(bool invalid) {
        m_edit->setStyleSheet(invalid ? QStringLiteral("QLineEdit { border-color: #ff5f56; }") : QString());
        m_edit->setToolTip(invalid ? tr("Not a valid path expression") : QString());
    }

    QLineEdit* m_edit;
    QString m_text;
};

class TokenComboEditor : public PropertyEditor {
public:
    TokenComboEditor(const ValueTypeInfo& info, const std::vector<TfToken>& allowed, QWidget* parent)
        : PropertyEditor(info, parent), m_allowed(allowed) {
        auto* layout = rowLayout(this);
        m_combo = new QComboBox(this);
        layout->addWidget(m_combo, 1);
        for (const auto& token : allowed) m_combo->addItem(QString::fromStdString(token.GetString()));
        connect(m_combo, &QComboBox::activated, this, [this](int index) {
            if (index >= 0) emitSingleEdit(VtValue(TfToken(m_combo->itemText(index).toStdString())));
        });
    }
    void setValue(const VtValue& value) override {
        const QString text = value.IsHolding<TfToken>() ? QString::fromStdString(value.UncheckedGet<TfToken>().GetString())
                                                       : QString();
        int index = m_combo->findText(text);
        if (index < 0 && !text.isEmpty()) {
            // A value outside allowedTokens is still shown, not silently replaced.
            m_combo->addItem(text);
            index = m_combo->count() - 1;
        }
        m_combo->setCurrentIndex(index);
    }
    VtValue value() const override { return VtValue(TfToken(m_combo->currentText().toStdString())); }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        m_combo->setEnabled(!readOnly);
    }

private:
    std::vector<TfToken> m_allowed;
    QComboBox* m_combo;
};

// ---- Asset paths -----------------------------------------------------------------

class AssetEditor : public PropertyEditor {
public:
    AssetEditor(const ValueTypeInfo& info, const EditorHints& hints, QWidget* parent)
        : PropertyEditor(info, parent), m_anchor(hints.anchorLayer) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        auto* row = new QWidget(this);
        auto* rowLay = rowLayout(row);
        m_edit = new QLineEdit(row);
        m_relative = new QToolButton(row);
        m_relative->setObjectName(QStringLiteral("propertyButton"));
        m_relative->setToolTip(tr("Switch between a path relative to the layer and an absolute path"));
        m_browse = new QToolButton(row);
        m_browse->setObjectName(QStringLiteral("propertyButton"));
        m_browse->setText(QStringLiteral("\xE2\x80\xA6"));
        m_browse->setToolTip(tr("Browse for a file"));
        rowLay->addWidget(m_edit, 1);
        rowLay->addWidget(m_relative);
        rowLay->addWidget(m_browse);
        layout->addWidget(row);

        m_resolved = valueLabel(this);
        m_resolved->setObjectName(QStringLiteral("propertySubLine"));
        m_resolved->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(m_resolved);

        connect(m_edit, &QLineEdit::editingFinished, this, [this]() { commit(m_edit->text()); });
        connect(m_browse, &QToolButton::clicked, this, [this]() { browse(); });
        connect(m_relative, &QToolButton::clicked, this, [this]() { toggleRelative(); });
        updateDisplay();
    }

    void setValue(const VtValue& value) override {
        m_path = value.IsHolding<SdfAssetPath>() ? QString::fromStdString(value.UncheckedGet<SdfAssetPath>().GetAssetPath())
                                                 : QString();
        updateDisplay();
    }
    VtValue value() const override { return VtValue(SdfAssetPath(m_path.toStdString())); }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        m_edit->setReadOnly(readOnly);
        m_browse->setEnabled(!readOnly);
        updateDisplay();
    }

private:
    QString layerDir() const {
        if (!m_anchor || m_anchor->IsAnonymous() || m_anchor->GetRealPath().empty()) return {};
        return QFileInfo(QString::fromStdString(m_anchor->GetRealPath())).absolutePath();
    }
    QString resolvedPath() const {
        if (m_path.isEmpty()) return {};
        const std::string anchored = m_anchor ? SdfComputeAssetPathRelativeToLayer(m_anchor, m_path.toStdString())
                                              : m_path.toStdString();
        return QString::fromStdString(ArGetResolver().Resolve(anchored).GetPathString());
    }
    static bool isRelative(const QString& path) { return path.startsWith(QStringLiteral("./")) || path.startsWith(QStringLiteral("../")); }

    void updateDisplay() {
        m_edit->setText(m_path);
        m_relative->setText(isRelative(m_path) ? tr("Rel") : tr("Abs"));
        m_relative->setEnabled(!m_readOnly && !layerDir().isEmpty() && !m_path.isEmpty());
        const QString resolved = resolvedPath();
        if (m_path.isEmpty()) {
            m_resolved->setText(QString());
            m_resolved->setVisible(false);
            return;
        }
        m_resolved->setVisible(true);
        m_resolved->setProperty("unresolved", resolved.isEmpty());
        m_resolved->setText(resolved.isEmpty() ? tr("Unresolved") : tr("Resolved \xC2\xB7 %1").arg(resolved));
        m_resolved->setToolTip(resolved);
        m_resolved->style()->unpolish(m_resolved);
        m_resolved->style()->polish(m_resolved);
    }
    void commit(const QString& path) {
        if (m_readOnly || path == m_path) return;
        m_path = path;
        updateDisplay();
        emitSingleEdit(value());
    }
    void browse() {
        const QString start = !resolvedPath().isEmpty() ? QFileInfo(resolvedPath()).absolutePath() : layerDir();
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose Asset"), start);
        if (file.isEmpty()) return;
        const QString dir = layerDir();
        commit(dir.isEmpty() ? file : QStringLiteral("./") + QDir(dir).relativeFilePath(file));
    }
    void toggleRelative() {
        const QString dir = layerDir();
        const QString resolved = resolvedPath();
        if (dir.isEmpty() || resolved.isEmpty()) return;
        if (isRelative(m_path)) {
            commit(QDir::cleanPath(resolved));
        } else {
            QString rel = QDir(dir).relativeFilePath(resolved);
            if (!rel.startsWith(QStringLiteral("../"))) rel.prepend(QStringLiteral("./"));
            commit(rel);
        }
    }

    SdfLayerHandle m_anchor;
    QString m_path;
    QLineEdit* m_edit;
    QToolButton* m_relative;
    QToolButton* m_browse;
    QLabel* m_resolved;
};

// ---- Arrays ------------------------------------------------------------------------

class ArrayEditor : public PropertyEditor {
public:
    ArrayEditor(const ValueTypeInfo& info, const EditorHints& hints, QWidget* parent)
        : PropertyEditor(info, parent), m_hints(hints) {
        auto* layout = rowLayout(this);
        m_summary = valueLabel(this);
        m_summary->setObjectName(QStringLiteral("propertyArraySummary"));
        m_edit = new QPushButton(tr("Edit"), this);
        m_edit->setObjectName(QStringLiteral("propertyButton"));
        layout->addWidget(m_summary, 1);
        layout->addWidget(m_edit);
        connect(m_edit, &QPushButton::clicked, this, [this]() { openDialog(); });
        m_value = emptyArray(info.typeName);
    }
    void setValue(const VtValue& value) override {
        m_value = value.IsEmpty() ? emptyArray(m_info.typeName) : value;
        const QString text = QString::fromStdString(formatValue(m_value, 2));
        m_summary->setText(text);
        m_summary->setToolTip(text);
    }
    VtValue value() const override { return m_value; }
    void setReadOnly(bool readOnly) override {
        PropertyEditor::setReadOnly(readOnly);
        m_edit->setText(readOnly ? tr("View") : tr("Edit"));
    }

private:
    void openDialog() {
        ArrayEditorDialog dialog(m_value, m_info.typeName, m_hints,
                                 QString::fromStdString(m_info.typeName.GetAsToken().GetString()), window());
        if (m_readOnly) dialog.setEnabled(true);
        if (dialog.exec() == QDialog::Accepted && !m_readOnly) {
            const VtValue edited = dialog.array();
            if (edited != m_value) {
                setValue(edited);
                emitSingleEdit(edited);
            }
        }
    }

    EditorHints m_hints;
    VtValue m_value;
    QLabel* m_summary;
    QPushButton* m_edit;
};

// ---- Values that can't be edited ---------------------------------------------------

class ReadOnlyTextEditor : public PropertyEditor {
public:
    ReadOnlyTextEditor(const ValueTypeInfo& info, bool fallback, QWidget* parent)
        : PropertyEditor(info, parent), m_fallback(fallback) {
        auto* layout = rowLayout(this);
        m_label = valueLabel(this);
        m_label->setObjectName(QStringLiteral("propertyValueText"));
        m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(m_label, 1);
        if (fallback) {
            setToolTip(tr("No editor for %1 values yet").arg(QString::fromStdString(info.typeName.GetAsToken().GetString())));
        } else {
            setToolTip(tr("%1 attributes have no value").arg(QString::fromStdString(info.typeName.GetAsToken().GetString())));
        }
        m_readOnly = true;
    }
    void setValue(const VtValue& value) override {
        m_value = value;
        m_label->setText(m_fallback ? QString::fromStdString(formatValue(value)) : tr("(no value)"));
    }
    VtValue value() const override { return m_value; }
    void setReadOnly(bool) override {}
    bool isFallback() const override { return m_fallback; }

private:
    bool m_fallback;
    VtValue m_value;
    QLabel* m_label;
};

}  // namespace

PropertyEditor* createPropertyEditor(const SdfValueTypeName& typeName, const EditorHints& hints, QWidget* parent) {
    const ValueTypeInfo info = ValueTypeInfo::classify(typeName);
    PropertyEditor* editor = nullptr;
    if (info.isArray) {
        editor = info.scalarKind == ScalarKind::Unknown || info.scalarKind == ScalarKind::Opaque
                     ? static_cast<PropertyEditor*>(new ReadOnlyTextEditor(info, true, parent))
                     : new ArrayEditor(info, hints, parent);
    } else if (info.shape == ValueShape::Matrix) {
        editor = new MatrixEditor(info, parent);
    } else if (info.isNumeric()) {
        editor = new NumericEditor(info, hints, parent);
    } else {
        switch (info.scalarKind) {
            case ScalarKind::Bool:
                editor = new BoolEditor(info, parent);
                break;
            case ScalarKind::Token:
                editor = hints.allowedTokens.empty() ? static_cast<PropertyEditor*>(new TextEditor(info, parent))
                                                     : new TokenComboEditor(info, hints.allowedTokens, parent);
                break;
            case ScalarKind::String:
            case ScalarKind::PathExpression:
                editor = new TextEditor(info, parent);
                break;
            case ScalarKind::Asset:
                editor = new AssetEditor(info, hints, parent);
                break;
            case ScalarKind::Opaque:
                editor = new ReadOnlyTextEditor(info, false, parent);
                break;
            default:
                editor = new ReadOnlyTextEditor(info, true, parent);
                break;
        }
    }
    editor->setValue(VtValue());
    return editor;
}

}  // namespace usdcc::usd::properties
