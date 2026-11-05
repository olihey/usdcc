#include "usdcc/usd/properties/connected_inputs_editor.h"

#include "usdcc/ui/flow_layout.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/properties/property_style.h"

#include <pxr/usd/sdf/schema.h>

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

#include <algorithm>

namespace usdcc::usd::properties {

ConnectedInputsEditor::ConnectedInputsEditor(QWidget* parent)
    : PropertyEditor(ValueTypeInfo::classify(PXR_NS::SdfValueTypeNames->Token), parent) {
    auto* flow = new usdcc::ui::FlowLayout(this);
    usdcc::ui::scaled::spacing(flow, 4);
    m_connect = iconButton(QStringLiteral(":/usdcc/theme/link.png"), tr("Connect an input to this output"), this);
    m_connect->setProperty("connectionButton", true);  // keeps the #propertyIconButton look
    connect(m_connect, &QToolButton::clicked, this, [this]() { Q_EMIT connectRequested(); });
}

QWidget* ConnectedInputsEditor::rowButton() const { return m_connect; }

void ConnectedInputsEditor::setReceivers(const std::vector<Receiver>& receivers) {
    const bool same = m_built && receivers.size() == m_receivers.size() &&
                      std::equal(receivers.begin(), receivers.end(), m_receivers.begin(), [](const Receiver& a, const Receiver& b) {
                          return a.path == b.path && a.label == b.label;
                      });
    if (same) return;  // refreshes come with every change on the stage
    m_receivers = receivers;
    rebuild();
}

void ConnectedInputsEditor::setReadOnly(bool readOnly) {
    if (m_built && readOnly == isReadOnly()) return;
    PropertyEditor::setReadOnly(readOnly);
    m_connect->setVisible(!readOnly);
    rebuild();
}

void ConnectedInputsEditor::rebuild() {
    m_built = true;
    QLayout* flow = layout();
    while (QLayoutItem* item = flow->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_receivers.empty()) {
        auto* none = new QLabel(tr("Not connected"), this);
        none->setObjectName(QStringLiteral("propertyNote"));
        usdcc::ui::scaled::fixedHeight(none, style::kFieldHeight);
        flow->addWidget(none);
    }
    for (const Receiver& receiver : m_receivers) {
        auto* pill = new QFrame(this);
        pill->setObjectName(QStringLiteral("schemaPill"));  // the look of the relationship pills
        pill->setProperty("state", "authored");
        usdcc::ui::scaled::fixedHeight(pill, style::kFieldHeight);
        auto* row = new QHBoxLayout(pill);
        usdcc::ui::scaled::margins(row, 8, 0, 4, 0);
        usdcc::ui::scaled::spacing(row, 4);
        auto* link = new QToolButton(pill);
        link->setObjectName(QStringLiteral("pillLink"));
        link->setText(receiver.label);
        link->setCursor(Qt::PointingHandCursor);
        link->setMinimumWidth(1);  // cut short in a narrow panel, the × still showing
        const PXR_NS::SdfPath path = receiver.path;
        link->setToolTip(tr("Select %1").arg(QString::fromStdString(path.GetPrimPath().GetString())));
        connect(link, &QToolButton::clicked, this, [this, path]() { Q_EMIT selectRequested(path); });
        row->addWidget(link, 1);
        if (!isReadOnly()) {
            auto* remove = new QToolButton(pill);
            remove->setObjectName(QStringLiteral("schemaPillRemove"));
            remove->setIcon(QIcon(QStringLiteral(":/usdcc/theme/close.png")));
            usdcc::ui::scaled::iconSize(remove, 10, 10);
            remove->setToolTip(tr("Disconnect %1").arg(QString::fromStdString(path.GetString())));
            connect(remove, &QToolButton::clicked, this, [this, path]() { Q_EMIT disconnectRequested(path); });
            row->addWidget(remove);
        }
        flow->addWidget(pill);
    }
    updateHeight();
}

void ConnectedInputsEditor::updateHeight() {
    // Set rather than left to height-for-width, which the rows, sections and
    // scroll area around this wouldn't all pass on.
    const int wanted = layout()->heightForWidth(width());
    if (height() != wanted || minimumHeight() != wanted) setFixedHeight(wanted);
}

void ConnectedInputsEditor::resizeEvent(QResizeEvent* event) {
    PropertyEditor::resizeEvent(event);
    updateHeight();
}

bool ConnectedInputsEditor::event(QEvent* event) {
    // The pills' sizes change with the theme's font and the UI scale.
    if (event->type() == QEvent::LayoutRequest) updateHeight();
    return PropertyEditor::event(event);
}

}  // namespace usdcc::usd::properties
