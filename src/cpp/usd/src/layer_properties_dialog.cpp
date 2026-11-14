#include "usdcc/usd/layer_properties_dialog.h"

#include "usdcc/ui/scaled_metrics.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usdGeom/metrics.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace usdcc::usd {

namespace {

QString number(double value) { return QLocale::c().toString(value, 'g', 12); }

// Empty: unset. `ok` is false for text that isn't a number.
std::optional<double> parse(const QString& text, bool* ok) {
    *ok = true;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return std::nullopt;
    const double value = QLocale::c().toDouble(trimmed, ok);
    return *ok ? std::optional<double>(value) : std::nullopt;
}

// The number at the start of a Meters per Unit entry: "0.01 (centimeters)".
QString unitsNumber(const QString& text) { return text.trimmed().section(QLatin1Char(' '), 0, 0); }

QString text(const std::optional<double>& value) { return value ? number(*value) : QString(); }

}  // namespace

LayerPropertiesDialog::LayerPropertiesDialog(const PXR_NS::SdfLayerHandle& layer,
                                             const layer_actions::LayerProperties& current, bool sublayer,
                                             QWidget* parent)
    : QDialog(parent) {
    const QString name = layer ? QString::fromStdString(layer->GetDisplayName()) : QString();
    setWindowTitle(tr("Edit %1").arg(name));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(form);
    auto unset = [](const QString& fallback) { return tr("Not set: %1").arg(fallback); };

    m_defaultPrim = new QLineEdit(QString::fromStdString(current.defaultPrim.value_or(std::string())), this);
    m_defaultPrim->setPlaceholderText(tr("Not set"));
    m_defaultPrim->setToolTip(tr("A root prim's name (World), or a prim's path (/World/Asset)"));
    form->addRow(tr("Default Prim"), m_defaultPrim);

    // Up axis: unset (USD's fallback), Y or Z.
    m_upAxis = new QComboBox(this);
    m_upAxis->addItem(unset(QString::fromStdString(PXR_NS::UsdGeomGetFallbackUpAxis().GetString())), QString());
    m_upAxis->addItem(QStringLiteral("Y"), QStringLiteral("Y"));
    m_upAxis->addItem(QStringLiteral("Z"), QStringLiteral("Z"));
    m_upAxis->setCurrentIndex(std::max(0, m_upAxis->findData(QString::fromStdString(current.upAxis.value_or("")))));
    form->addRow(tr("Up Axis"), m_upAxis);

    // Meters per unit: the common units, or any number.
    m_metersPerUnit = new QComboBox(this);
    m_metersPerUnit->setEditable(true);
    m_metersPerUnit->setInsertPolicy(QComboBox::NoInsert);
    m_metersPerUnit->addItem(QString());
    for (const auto& [units, unitName] : {std::pair{PXR_NS::UsdGeomLinearUnits::millimeters, tr("millimeters")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::centimeters, tr("centimeters")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::decimeters, tr("decimeters")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::meters, tr("meters")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::kilometers, tr("kilometers")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::inches, tr("inches")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::feet, tr("feet")},
                                          std::pair{PXR_NS::UsdGeomLinearUnits::yards, tr("yards")}}) {
        m_metersPerUnit->addItem(QStringLiteral("%1 (%2)").arg(number(units), unitName));
    }
    m_metersPerUnit->setCurrentIndex(0);
    m_metersPerUnit->setEditText(QString());
    if (current.metersPerUnit) {
        // A preset shown with its name, any other number as it is.
        QString shown = number(*current.metersPerUnit);
        for (int i = 1; i < m_metersPerUnit->count(); ++i) {
            if (PXR_NS::UsdGeomLinearUnitsAre(unitsNumber(m_metersPerUnit->itemText(i)).toDouble(),
                                              *current.metersPerUnit)) {
                shown = m_metersPerUnit->itemText(i);
            }
        }
        m_metersPerUnit->setEditText(shown);
    }
    m_metersPerUnit->lineEdit()->setPlaceholderText(unset(number(PXR_NS::UsdGeomLinearUnits::centimeters)));
    form->addRow(tr("Meters per Unit"), m_metersPerUnit);

    // The time fields, USD's fallback as the placeholder.
    auto timeField = [&](const QString& label, const std::optional<double>& value, const PXR_NS::TfToken& key) {
        auto* field = new QLineEdit(text(value), this);
        const PXR_NS::VtValue fallback = PXR_NS::SdfSchema::GetInstance().GetFallback(key);
        field->setPlaceholderText(unset(fallback.IsHolding<double>() ? number(fallback.UncheckedGet<double>()) : QString()));
        form->addRow(label, field);
        return field;
    };
    m_start = timeField(tr("Start Time Code"), current.startTimeCode, PXR_NS::SdfFieldKeys->StartTimeCode);
    m_end = timeField(tr("End Time Code"), current.endTimeCode, PXR_NS::SdfFieldKeys->EndTimeCode);
    m_timeCodesPerSecond =
        timeField(tr("Time Codes per Second"), current.timeCodesPerSecond, PXR_NS::SdfFieldKeys->TimeCodesPerSecond);
    m_framesPerSecond = timeField(tr("Frames per Second"), current.framesPerSecond, PXR_NS::SdfFieldKeys->FramesPerSecond);
    if (sublayer) {
        // In the parent's subLayerOffsets: empty is no offset, no scale.
        m_offset = new QLineEdit(current.offset.GetOffset() != 0.0 ? number(current.offset.GetOffset()) : QString(), this);
        m_offset->setPlaceholderText(unset(QStringLiteral("0")));
        form->addRow(tr("Time Offset"), m_offset);
        m_scale = new QLineEdit(current.offset.GetScale() != 1.0 ? number(current.offset.GetScale()) : QString(), this);
        m_scale->setPlaceholderText(unset(QStringLiteral("1")));
        form->addRow(tr("Time Scale"), m_scale);
    }

    m_comment = new QLineEdit(QString::fromStdString(current.comment), this);
    form->addRow(tr("Comment"), m_comment);
    m_documentation = new QPlainTextEdit(QString::fromStdString(current.documentation), this);
    m_documentation->setTabChangesFocus(true);
    m_documentation->setFixedHeight(usdcc::ui::scaled::pixels(72));
    form->addRow(tr("Documentation"), m_documentation);

    m_problem = new QLabel(this);
    m_problem->setObjectName(QStringLiteral("propertySubLine"));  // red or amber, as a property's sub line
    m_problem->setWordWrap(true);
    m_problem->hide();
    layout->addWidget(m_problem);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    for (QLineEdit* field : {m_defaultPrim, m_start, m_end, m_timeCodesPerSecond, m_framesPerSecond, m_offset, m_scale,
                             m_metersPerUnit->lineEdit()}) {
        if (field) connect(field, &QLineEdit::textChanged, this, &LayerPropertiesDialog::check);
    }
    check();
    resize(usdcc::ui::scaled::pixels(440), sizeHint().height());
}

layer_actions::LayerProperties LayerPropertiesDialog::values() const {
    layer_actions::LayerProperties v;
    bool ok = true;
    if (const QString prim = m_defaultPrim->text().trimmed(); !prim.isEmpty()) v.defaultPrim = prim.toStdString();
    if (const QString axis = m_upAxis->currentData().toString(); !axis.isEmpty()) v.upAxis = axis.toStdString();
    v.metersPerUnit = parse(unitsNumber(m_metersPerUnit->currentText()), &ok);
    v.startTimeCode = parse(m_start->text(), &ok);
    v.endTimeCode = parse(m_end->text(), &ok);
    v.timeCodesPerSecond = parse(m_timeCodesPerSecond->text(), &ok);
    v.framesPerSecond = parse(m_framesPerSecond->text(), &ok);
    if (m_offset && m_scale) {
        v.offset = PXR_NS::SdfLayerOffset(parse(m_offset->text(), &ok).value_or(0.0), parse(m_scale->text(), &ok).value_or(1.0));
    }
    v.comment = m_comment->text().toStdString();
    v.documentation = m_documentation->toPlainText().toStdString();
    return v;
}

void LayerPropertiesDialog::check() {
    QStringList errors;
    QString warning;
    auto numberOf = [&](const QString& label, const QString& text) {
        bool ok = true;
        const std::optional<double> value = parse(text, &ok);
        if (!ok) errors << tr("%1 isn't a number.").arg(label);
        return value;
    };
    const QString prim = m_defaultPrim->text().trimmed();
    if (!prim.isEmpty()) {
        const std::string p = prim.toStdString();
        const bool valid = prim.startsWith(QLatin1Char('/'))
                               ? PXR_NS::SdfPath::IsValidPathString(p) && PXR_NS::SdfPath(p).IsAbsolutePath() &&
                                     PXR_NS::SdfPath(p).IsPrimPath()
                               : PXR_NS::SdfPath::IsValidIdentifier(p);
        if (!valid) errors << tr("Default Prim is neither a prim name nor a prim path.");
    }
    const std::optional<double> units = numberOf(tr("Meters per Unit"), unitsNumber(m_metersPerUnit->currentText()));
    if (units && *units <= 0.0) errors << tr("Meters per Unit must be above 0.");
    const std::optional<double> start = numberOf(tr("Start Time Code"), m_start->text());
    const std::optional<double> end = numberOf(tr("End Time Code"), m_end->text());
    for (const auto& [label, field] : {std::pair{tr("Time Codes per Second"), m_timeCodesPerSecond},
                                       std::pair{tr("Frames per Second"), m_framesPerSecond}}) {
        const std::optional<double> rate = numberOf(label, field->text());
        if (rate && *rate <= 0.0) errors << tr("%1 must be above 0.").arg(label);
    }
    if (m_offset && m_scale) {
        numberOf(tr("Time Offset"), m_offset->text());
        const std::optional<double> scale = numberOf(tr("Time Scale"), m_scale->text());
        if (scale && *scale == 0.0) errors << tr("Time Scale can't be 0.");
    }
    if (start && end && *end < *start) warning = tr("The end time code is before the start.");

    m_ok->setEnabled(errors.isEmpty());
    const QString shown = errors.isEmpty() ? warning : errors.join(QLatin1Char(' '));
    m_problem->setText(shown);
    m_problem->setProperty("unresolved", !errors.isEmpty());  // red (theme/dark.qss)
    m_problem->setProperty("caution", errors.isEmpty() && !warning.isEmpty());  // amber
    m_problem->style()->unpolish(m_problem);
    m_problem->style()->polish(m_problem);
    m_problem->setVisible(!shown.isEmpty());
}

}  // namespace usdcc::usd
