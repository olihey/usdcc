#include "usdcc/usd/attributes_view_panel.h"

#include "usdcc/usd/stage_manager.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <sstream>

namespace usdcc::usd {

namespace {

std::vector<double> parseNumberList(const QString& text) {
    QString trimmed = text.trimmed();
    if (trimmed.startsWith('(') && trimmed.endsWith(')')) {
        trimmed = trimmed.mid(1, trimmed.length() - 2);
    }
    std::vector<double> values;
    for (const QString& part : trimmed.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const double value = part.trimmed().toDouble(&ok);
        if (!ok) {
            return {};
        }
        values.push_back(value);
    }
    return values;
}

// Parses `text` according to `attr`'s actual value type and sets it.
// Dispatches on the attribute's underlying C++ type (pxr::TfType) rather
// than its exact SdfValueTypeName, since role variants like Color3f/
// Vector3f/Point3f/Normal3f all share the same GfVec3f C++ representation
// and Set() only cares about that. Returns false (leaving the attribute
// unchanged) for types not handled here.
bool setAttributeFromText(const PXR_NS::UsdAttribute& attr, const QString& text) {
    using namespace PXR_NS;
    const TfType type = attr.GetTypeName().GetType();
    bool ok = false;

    if (type == TfType::Find<bool>()) {
        const QString lower = text.trimmed().toLower();
        return attr.Set(lower == "true" || lower == "1");
    }
    if (type == TfType::Find<int>()) {
        const int value = text.toInt(&ok);
        return ok && attr.Set(value);
    }
    if (type == TfType::Find<unsigned int>()) {
        const uint value = text.toUInt(&ok);
        return ok && attr.Set(value);
    }
    if (type == TfType::Find<int64_t>()) {
        const qlonglong value = text.toLongLong(&ok);
        return ok && attr.Set(static_cast<int64_t>(value));
    }
    if (type == TfType::Find<float>()) {
        const float value = text.toFloat(&ok);
        return ok && attr.Set(value);
    }
    if (type == TfType::Find<double>()) {
        const double value = text.toDouble(&ok);
        return ok && attr.Set(value);
    }
    if (type == TfType::Find<std::string>()) {
        return attr.Set(text.toStdString());
    }
    if (type == TfType::Find<TfToken>()) {
        return attr.Set(TfToken(text.toStdString()));
    }

    const std::vector<double> values = parseNumberList(text);
    if (type == TfType::Find<GfVec2f>() && values.size() == 2) {
        return attr.Set(GfVec2f(values[0], values[1]));
    }
    if (type == TfType::Find<GfVec3f>() && values.size() == 3) {
        return attr.Set(GfVec3f(values[0], values[1], values[2]));
    }
    if (type == TfType::Find<GfVec4f>() && values.size() == 4) {
        return attr.Set(GfVec4f(values[0], values[1], values[2], values[3]));
    }
    if (type == TfType::Find<GfVec2d>() && values.size() == 2) {
        return attr.Set(GfVec2d(values[0], values[1]));
    }
    if (type == TfType::Find<GfVec3d>() && values.size() == 3) {
        return attr.Set(GfVec3d(values[0], values[1], values[2]));
    }
    if (type == TfType::Find<GfVec4d>() && values.size() == 4) {
        return attr.Set(GfVec4d(values[0], values[1], values[2], values[3]));
    }
    if (type == TfType::Find<GfVec2i>() && values.size() == 2) {
        return attr.Set(GfVec2i(static_cast<int>(values[0]), static_cast<int>(values[1])));
    }
    if (type == TfType::Find<GfVec3i>() && values.size() == 3) {
        return attr.Set(
            GfVec3i(static_cast<int>(values[0]), static_cast<int>(values[1]), static_cast<int>(values[2])));
    }
    if (type == TfType::Find<GfVec4i>() && values.size() == 4) {
        return attr.Set(GfVec4i(static_cast<int>(values[0]), static_cast<int>(values[1]),
                                 static_cast<int>(values[2]), static_cast<int>(values[3])));
    }

    return false;
}

}  // namespace

AttributesViewPanel::AttributesViewPanel(StageManager* stageManager, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(title, parent), m_stageManager(stageManager) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 4, 4, 4);

    m_stageCombo = new QComboBox(toolbar);
    toolbarLayout->addWidget(new QLabel(tr("Stage:"), toolbar));
    toolbarLayout->addWidget(m_stageCombo, 1);

    m_table = new QTableWidget(container);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Value")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);

    layout->addWidget(toolbar);
    layout->addWidget(m_table, 1);

    setContentWidget(container);

    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AttributesViewPanel::onStageComboChanged);
    connect(m_table, &QTableWidget::cellChanged, this, &AttributesViewPanel::onCellChanged);

    if (m_stageManager) {
        m_stageOpenedConnection = m_stageManager->stageOpened.connect([this](StageRefPtr) { refreshStageCombo(); });
        m_stageClosedConnection = m_stageManager->stageClosed.connect([this](StageRefPtr) { refreshStageCombo(); });
        m_selectionChangedConnection = m_stageManager->selectionChanged.connect(
            [this](StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
                onStageSelectionChanged(stage, std::move(paths));
            });
    }

    refreshStageCombo();
}

usdcc::ui::ViewPanel* AttributesViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new AttributesViewPanel(m_stageManager, windowTitle(), parent);
    const int stageIndex = m_stageCombo->currentIndex();
    if (stageIndex >= 0) {
        copy->m_stageCombo->setCurrentIndex(stageIndex);
    }
    return copy;
}

void AttributesViewPanel::refreshStageCombo() {
    m_stageCombo->blockSignals(true);
    m_stageCombo->clear();

    if (m_stageManager) {
        for (const auto& stage : m_stageManager->stages()) {
            const QString identifier = QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier());
            m_stageCombo->addItem(identifier, static_cast<qlonglong>(stage->cacheId()));
        }
    }

    m_stageCombo->blockSignals(false);
    onStageComboChanged(m_stageCombo->currentIndex());
}

void AttributesViewPanel::onStageComboChanged(int index) {
    if (!m_stageManager || index < 0) {
        m_stage = nullptr;
        m_primPath = PXR_NS::SdfPath();
        refreshAttributes();
        return;
    }
    const auto cacheId = static_cast<long>(m_stageCombo->itemData(index).toLongLong());
    m_stage = m_stageManager->findByCacheId(cacheId);
    onStageSelectionChanged(m_stage, m_stageManager->selectedPaths(m_stage));
}

void AttributesViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage != m_stage) {
        return;
    }
    m_primPath = paths.empty() ? PXR_NS::SdfPath() : paths.front();
    refreshAttributes();
}

void AttributesViewPanel::refreshAttributes() {
    m_updatingTable = true;
    m_table->setRowCount(0);

    const PXR_NS::UsdPrim prim = (m_stage && !m_primPath.IsEmpty()) ? m_stage->usdStage()->GetPrimAtPath(m_primPath)
                                                                     : PXR_NS::UsdPrim();
    if (prim) {
        const std::vector<PXR_NS::UsdAttribute> attrs = prim.GetAttributes();
        m_table->setRowCount(static_cast<int>(attrs.size()));

        int row = 0;
        for (const auto& attr : attrs) {
            auto* nameItem = new QTableWidgetItem(QString::fromStdString(attr.GetName().GetString()));
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(row, 0, nameItem);

            auto* typeItem = new QTableWidgetItem(QString::fromStdString(attr.GetTypeName().GetAsToken().GetString()));
            typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(row, 1, typeItem);

            QString valueText;
            PXR_NS::VtValue value;
            if (attr.Get(&value)) {
                std::ostringstream oss;
                oss << value;
                valueText = QString::fromStdString(oss.str());
            }
            m_table->setItem(row, 2, new QTableWidgetItem(valueText));

            ++row;
        }
    }

    m_updatingTable = false;
}

void AttributesViewPanel::onCellChanged(int row, int column) {
    if (m_updatingTable || column != 2 || !m_stage) {
        return;
    }
    QTableWidgetItem* nameItem = m_table->item(row, 0);
    QTableWidgetItem* valueItem = m_table->item(row, column);
    if (!nameItem || !valueItem) {
        return;
    }

    const PXR_NS::UsdPrim prim = m_stage->usdStage()->GetPrimAtPath(m_primPath);
    const PXR_NS::UsdAttribute attr = prim ? prim.GetAttribute(PXR_NS::TfToken(nameItem->text().toStdString()))
                                            : PXR_NS::UsdAttribute();

    if (!attr || !setAttributeFromText(attr, valueItem->text())) {
        // Unsupported type or unparsable text: revert the displayed value
        // rather than leaving an edit that didn't actually apply.
        refreshAttributes();
    }
}

}  // namespace usdcc::usd
