#include "usdcc/usd/properties/schema_pills.h"

#include "usdcc/ui/flow_layout.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/properties/property_style.h"

#include <pxr/usd/sdf/schema.h>

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>

namespace usdcc::usd::properties {

namespace {
constexpr int kMaxToolTipProperties = 12;

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString pillToolTip(const AppliedSchema& schema) {
    QString tip = QStringLiteral("<b>%1</b>").arg(qs(schema.name).toHtmlEscaped());
    if (schema.state == AppliedSchema::State::Unregistered) {
        return tip + SchemaPillsEditor::tr("<br>Not registered: its plugin may not be loaded");
    }
    if (schema.state == AppliedSchema::State::BuiltIn) {
        tip += schema.cameFrom.empty() ? SchemaPillsEditor::tr("<br>USD applies it automatically")
                                       : SchemaPillsEditor::tr("<br>Comes with %1").arg(qs(schema.cameFrom).toHtmlEscaped());
    }
    const QString documentation = qs(schemaDocumentation(schema.name)).trimmed();
    if (!documentation.isEmpty()) {
        // The first paragraph is enough for a tooltip.
        tip += QStringLiteral("<p>%1</p>").arg(documentation.section(QStringLiteral("\n\n"), 0, 0).toHtmlEscaped());
    }
    const std::vector<std::string> properties = schemaPropertyNames(schema.name);
    if (!properties.empty()) {
        QStringList names;
        for (std::size_t i = 0; i < properties.size() && i < kMaxToolTipProperties; ++i) {
            names << qs(properties[i]).toHtmlEscaped();
        }
        if (properties.size() > kMaxToolTipProperties) {
            names << SchemaPillsEditor::tr("and %1 more").arg(properties.size() - kMaxToolTipProperties);
        }
        tip += QStringLiteral("<p>%1</p>").arg(names.join(QStringLiteral("<br>")));
    }
    return tip;
}
}  // namespace

SchemaPillsEditor::SchemaPillsEditor(QWidget* parent)
    : PropertyEditor(ValueTypeInfo::classify(PXR_NS::SdfValueTypeNames->Token), parent) {
    auto* flow = new usdcc::ui::FlowLayout(this);
    usdcc::ui::scaled::spacing(flow, 4);
    m_add = iconButton(QStringLiteral(":/usdcc/theme/plus.png"), tr("Apply a schema"), this);
    m_add->setPopupMode(QToolButton::InstantPopup);
}

QWidget* SchemaPillsEditor::rowButton() const { return m_add; }

void SchemaPillsEditor::setSchemas(const std::vector<AppliedSchema>& schemas) {
    const bool same = schemas.size() == m_schemas.size() &&
                      std::equal(schemas.begin(), schemas.end(), m_schemas.begin(), [](const AppliedSchema& a, const AppliedSchema& b) {
                          return a.name == b.name && a.state == b.state && a.cameFrom == b.cameFrom;
                      });
    if (same && !m_schemas.empty()) return;
    m_schemas = schemas;
    rebuild();
}

void SchemaPillsEditor::setReadOnly(bool readOnly) {
    if (readOnly == isReadOnly()) return;
    PropertyEditor::setReadOnly(readOnly);
    m_add->setVisible(!readOnly);
    rebuild();
}

void SchemaPillsEditor::rebuild() {
    QLayout* flow = layout();
    while (QLayoutItem* item = flow->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_schemas.empty()) {
        auto* none = new QLabel(tr("None"), this);
        none->setObjectName(QStringLiteral("propertyNote"));
        usdcc::ui::scaled::fixedHeight(none, style::kFieldHeight);
        flow->addWidget(none);
    }
    for (const AppliedSchema& schema : m_schemas) {
        auto* pill = new QFrame(this);
        pill->setObjectName(QStringLiteral("schemaPill"));  // styled by theme/dark.qss
        pill->setProperty("state", schema.state == AppliedSchema::State::BuiltIn        ? "builtIn"
                                   : schema.state == AppliedSchema::State::Unregistered ? "unregistered"
                                                                                        : "authored");
        pill->setToolTip(pillToolTip(schema));
        usdcc::ui::scaled::fixedHeight(pill, style::kFieldHeight);
        auto* row = new QHBoxLayout(pill);
        usdcc::ui::scaled::margins(row, 8, 0, 4, 0);
        usdcc::ui::scaled::spacing(row, 4);
        auto* label = new QLabel(qs(schema.name), pill);
        label->setMinimumWidth(1);  // cut short in a narrow panel, the × still showing
        row->addWidget(label, 1);
        if (schema.state != AppliedSchema::State::BuiltIn && !isReadOnly()) {
            auto* remove = new QToolButton(pill);
            remove->setObjectName(QStringLiteral("schemaPillRemove"));
            remove->setIcon(QIcon(QStringLiteral(":/usdcc/theme/close.png")));
            usdcc::ui::scaled::iconSize(remove, 10, 10);
            remove->setToolTip(tr("Remove %1").arg(qs(schema.name)));
            const QString name = qs(schema.name);
            connect(remove, &QToolButton::clicked, this, [this, name]() { Q_EMIT removeRequested(name); });
            row->addWidget(remove);
        }
        flow->addWidget(pill);
    }
    updateHeight();
}

void SchemaPillsEditor::updateHeight() {
    // Set rather than left to height-for-width, which the rows, sections and
    // scroll area around this wouldn't all pass on.
    const int wanted = layout()->heightForWidth(width());
    if (height() != wanted || minimumHeight() != wanted) setFixedHeight(wanted);
}

void SchemaPillsEditor::resizeEvent(QResizeEvent* event) {
    PropertyEditor::resizeEvent(event);
    updateHeight();
}

bool SchemaPillsEditor::event(QEvent* event) {
    // The pills' sizes change with the theme's font and the UI scale.
    if (event->type() == QEvent::LayoutRequest) updateHeight();
    return PropertyEditor::event(event);
}

}  // namespace usdcc::usd::properties
