#include "usdcc/usd/properties/data_type_gallery_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/attribute_binding.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/value_type_info.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/attributeLimits.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <map>

namespace usdcc::usd::properties {

using namespace PXR_NS;

namespace {

// Attribute names on the scratch prim: the type name, with "[]" spelled out.
TfToken attributeNameFor(const SdfValueTypeName& type) {
    std::string name = type.GetAsToken().GetString();
    if (type.IsArray()) {
        name = type.GetScalarType().GetAsToken().GetString() + "_array";
    }
    return TfToken(name);
}

QString noteFor(const ValueTypeInfo& info) {
    QString note = QString::fromStdString(info.typeName.GetType().GetTypeName());
    if (!info.role.IsEmpty()) {
        note += QStringLiteral(" \xC2\xB7 ") + QString::fromStdString(info.role.GetString());
    }
    return note;
}

}  // namespace

DataTypeGalleryDialog::DataTypeGalleryDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("USD Data Types"));
    resize(820, 680);

    m_readOnly = new QCheckBox(tr("Read-only"), this);
    m_unauthored = new QCheckBox(tr("Unauthored"), this);
    m_unauthored->setToolTip(tr("Clear every value, so each row shows the type's default"));
    m_animated = new QCheckBox(tr("Animated"), this);
    m_connected = new QCheckBox(tr("Connected"), this);
    m_limits = new QCheckBox(tr("Soft limits"), this);
    m_limits->setToolTip(tr("Give the numeric scalars a soft range, which turns them into sliders"));

    auto* toggles = new QHBoxLayout;
    toggles->addWidget(new QLabel(tr("Preview:"), this));
    for (QCheckBox* box : {m_readOnly, m_unauthored, m_animated, m_connected, m_limits}) {
        toggles->addWidget(box);
        connect(box, &QCheckBox::toggled, this, [this]() { rebuild(); });
    }
    toggles->addStretch(1);

    // Groups in a sidebar rather than tabs: ten of them don't fit a tab bar.
    m_groups = new QListWidget(this);
    m_groups->setObjectName(QStringLiteral("galleryGroups"));
    usdcc::ui::scaled::fixedWidth(m_groups, 190);
    m_pages = new QStackedWidget(this);
    connect(m_groups, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    auto* body = new QHBoxLayout;
    usdcc::ui::scaled::spacing(body, 10);
    body->addWidget(m_groups);
    body->addWidget(m_pages, 1);
    m_log = new QLabel(tr("Edit any value; the edit signals show up here."), this);
    m_log->setObjectName(QStringLiteral("propertyNote"));
    m_footer = new QLabel(this);
    m_footer->setObjectName(QStringLiteral("propertyNote"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(m_footer, 1);
    bottom->addWidget(buttons);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(toggles);
    layout->addLayout(body, 1);
    layout->addWidget(m_log);
    layout->addLayout(bottom);

    buildStage();
    rebuild();
}

void DataTypeGalleryDialog::buildStage() {
    m_stage = UsdStage::CreateInMemory("usdDataTypes.usda");
    m_prim = m_stage->DefinePrim(SdfPath("/Gallery"));
    for (const SdfValueTypeName& type : canonicalValueTypes()) {
        UsdAttribute attr = m_prim.CreateAttribute(attributeNameFor(type), type);
        const VtValue sample = sampleValue(type);
        if (!sample.IsEmpty()) {
            attr.Set(sample);
        }
    }
}

void DataTypeGalleryDialog::applyLimits(bool on) {
    for (const SdfValueTypeName& type : canonicalValueTypes(false)) {
        const ValueTypeInfo info = ValueTypeInfo::classify(type);
        if (!info.isNumeric() || info.shape != ValueShape::Scalar) continue;
        UsdAttributeLimits limits = m_prim.GetAttribute(attributeNameFor(type)).GetSoftLimits();
        if (on) {
            const double hi = info.isInteger() ? 100.0 : 1.0;
            limits.SetMinimum(makeNumericValue(type, {0.0}));
            limits.SetMaximum(makeNumericValue(type, {hi}));
        } else {
            limits.Clear();
        }
    }
}

void DataTypeGalleryDialog::applyAuthored(bool authored) {
    for (const SdfValueTypeName& type : canonicalValueTypes()) {
        UsdAttribute attr = m_prim.GetAttribute(attributeNameFor(type));
        const VtValue sample = sampleValue(type);
        if (authored && !sample.IsEmpty()) {
            if (!attr.HasAuthoredValue()) attr.Set(sample);
        } else {
            attr.ClearDefault();
        }
    }
}

void DataTypeGalleryDialog::rebuild() {
    m_lastTab = std::max(0, m_groups->currentRow());
    applyLimits(m_limits->isChecked());
    applyAuthored(!m_unauthored->isChecked());

    m_groups->clear();
    while (m_pages->count() > 0) {
        QWidget* page = m_pages->widget(0);
        m_pages->removeWidget(page);
        page->deleteLater();
    }

    // Group pages: one per TypeGroup for scalar types, one for all arrays.
    std::map<int, QVBoxLayout*> pages;
    std::map<int, QWidget*> pageWidgets;
    std::map<int, int> counts;
    const int kArrays = static_cast<int>(TypeGroup::Other) + 1;
    auto pageFor = [&](int key) {
        if (auto it = pages.find(key); it != pages.end()) return it->second;
        auto* content = new QWidget;
        auto* layout = new QVBoxLayout(content);
        usdcc::ui::scaled::margins(layout, 12, 10, 12, 10);
        usdcc::ui::scaled::spacing(layout, 6);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        const QString title = key == kArrays ? tr("Arrays") : QString::fromUtf8(typeGroupLabel(static_cast<TypeGroup>(key)));
        m_pages->addWidget(scroll);
        m_groups->addItem(title);
        pages[key] = layout;
        pageWidgets[key] = scroll;
        return layout;
    };

    int total = 0;
    int arrays = 0;
    int fallbacks = 0;
    QStringList fallbackNames;
    for (const SdfValueTypeName& type : canonicalValueTypes()) {
        const ValueTypeInfo info = ValueTypeInfo::classify(type);
        const UsdAttribute attr = m_prim.GetAttribute(attributeNameFor(type));
        const int key = info.isArray ? kArrays : static_cast<int>(info.group);
        QVBoxLayout* layout = pageFor(key);

        PropertyEditor* editor = createPropertyEditor(type, EditorHints::forAttribute(attr));
        auto* row = new PropertyRow(QString::fromStdString(type.GetAsToken().GetString()), editor);
        row->setLabelWidth(120);
        row->setNote(noteFor(info), 170);
        row->setPropertyPath(QString::fromStdString(attr.GetPath().GetString()));
        row->setUnauthored(!attr.HasAuthoredValue());
        row->setResettable(attr.HasAuthoredValue());
        row->setAnimated(m_animated->isChecked());
        row->setConnectedSource(m_connected->isChecked() ? QStringLiteral("Source.outputs:out") : QString(),
                                QStringLiteral("/Gallery/Source.outputs:out"));
        row->setReadOnly(m_readOnly->isChecked());
        layout->addWidget(row);

        auto* binding = new AttributeBinding(attr, editor, nullptr, nullptr, row);
        const QString name = QString::fromStdString(type.GetAsToken().GetString());
        connect(binding, &AttributeBinding::valueWritten, this, [this, name](const VtValue& v) {
            log(QStringLiteral("%1 \xE2\x86\x92 %2").arg(name, QString::fromStdString(formatValue(v, 3, 4))));
        });
        connect(binding, &AttributeBinding::editCommitted, this,
                [this, name]() { log(tr("%1 \xE2\x86\x92 edit finished (one undo step in a real panel)").arg(name)); });
        connect(row, &PropertyRow::resetRequested, binding, &AttributeBinding::resetToDefault);

        ++total;
        ++counts[key];
        arrays += info.isArray ? 1 : 0;
        if (editor->isFallback()) {
            ++fallbacks;
            fallbackNames << name;
        }
    }
    for (auto& [key, layout] : pages) {
        layout->addStretch(1);
        const int index = m_pages->indexOf(pageWidgets[key]);
        if (QListWidgetItem* item = m_groups->item(index)) {
            item->setText(QStringLiteral("%1 (%2)").arg(item->text()).arg(counts[key]));
        }
    }
    m_groups->setCurrentRow(std::min(m_lastTab, m_groups->count() - 1));

    m_footer->setText(tr("%1 types (%2 scalar, %3 array) \xC2\xB7 %4 without a dedicated editor%5")
                          .arg(total)
                          .arg(total - arrays)
                          .arg(arrays)
                          .arg(fallbacks)
                          .arg(fallbackNames.isEmpty() ? QString() : QStringLiteral(": ") + fallbackNames.join(", ")));
}

void DataTypeGalleryDialog::log(const QString& text) { m_log->setText(text); }

}  // namespace usdcc::usd::properties
