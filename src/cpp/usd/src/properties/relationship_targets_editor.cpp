#include "usdcc/usd/properties/relationship_targets_editor.h"

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

RelationshipTargetsEditor::RelationshipTargetsEditor(QWidget* parent)
    : PropertyEditor(ValueTypeInfo::classify(PXR_NS::SdfValueTypeNames->Token), parent) {
    auto* flow = new usdcc::ui::FlowLayout(this);
    usdcc::ui::scaled::spacing(flow, 4);
    m_add = iconButton(QStringLiteral(":/usdcc/theme/plus.png"), tr("Add a target"), this);
    connect(m_add, &QToolButton::clicked, this, [this]() { Q_EMIT addRequested(); });
}

QWidget* RelationshipTargetsEditor::rowButton() const { return m_add; }

void RelationshipTargetsEditor::setTargets(const std::vector<Target>& targets, bool replaces) {
    const bool same = m_built && replaces == m_replaces && targets.size() == m_targets.size() &&
                      std::equal(targets.begin(), targets.end(), m_targets.begin(), [](const Target& a, const Target& b) {
                          return a.path == b.path && a.problem == b.problem;
                      });
    if (same) return;  // refreshes come with every change on the stage
    m_targets = targets;
    m_replaces = replaces;
    rebuild();
}

void RelationshipTargetsEditor::setReadOnly(bool readOnly) {
    if (readOnly == isReadOnly()) return;
    PropertyEditor::setReadOnly(readOnly);
    m_add->setVisible(!readOnly);
    rebuild();
}

void RelationshipTargetsEditor::rebuild() {
    m_built = true;
    QLayout* flow = layout();
    while (QLayoutItem* item = flow->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const bool change = m_replaces && !m_targets.empty();
    m_add->setIcon(QIcon(change ? QStringLiteral(":/usdcc/theme/pencil.png") : QStringLiteral(":/usdcc/theme/plus.png")));
    m_add->setToolTip(change ? tr("Change the target") : tr("Add a target"));
    if (m_targets.empty()) {
        auto* none = new QLabel(tr("None"), this);
        none->setObjectName(QStringLiteral("propertyNote"));
        usdcc::ui::scaled::fixedHeight(none, style::kFieldHeight);
        flow->addWidget(none);
    }
    for (const Target& target : m_targets) {
        const QString text = QString::fromStdString(target.path.GetString());
        auto* pill = new QFrame(this);
        pill->setObjectName(QStringLiteral("schemaPill"));  // the same look as the schema pills
        pill->setProperty("state", target.problem.isEmpty() ? "authored" : "unregistered");
        usdcc::ui::scaled::fixedHeight(pill, style::kFieldHeight);
        auto* row = new QHBoxLayout(pill);
        usdcc::ui::scaled::margins(row, 8, 0, 4, 0);
        usdcc::ui::scaled::spacing(row, 4);
        auto* link = new QToolButton(pill);
        link->setObjectName(QStringLiteral("pillLink"));
        link->setText(text);
        link->setCursor(Qt::PointingHandCursor);
        link->setMinimumWidth(1);  // cut short in a narrow panel, the × still showing
        link->setToolTip(target.problem.isEmpty() ? tr("Select %1").arg(text)
                                                  : QStringLiteral("%1\n%2").arg(text, target.problem));
        const PXR_NS::SdfPath path = target.path;
        connect(link, &QToolButton::clicked, this, [this, path]() { Q_EMIT selectRequested(path); });
        row->addWidget(link, 1);
        if (!isReadOnly()) {
            auto* remove = new QToolButton(pill);
            remove->setObjectName(QStringLiteral("schemaPillRemove"));
            remove->setIcon(QIcon(QStringLiteral(":/usdcc/theme/close.png")));
            usdcc::ui::scaled::iconSize(remove, 10, 10);
            remove->setToolTip(tr("Remove this target"));
            connect(remove, &QToolButton::clicked, this, [this, path]() { Q_EMIT removeRequested(path); });
            row->addWidget(remove);
        }
        flow->addWidget(pill);
    }
    updateHeight();
}

void RelationshipTargetsEditor::updateHeight() {
    // Set rather than left to height-for-width, which the rows, sections and
    // scroll area around this wouldn't all pass on.
    const int wanted = layout()->heightForWidth(width());
    if (height() != wanted || minimumHeight() != wanted) setFixedHeight(wanted);
}

void RelationshipTargetsEditor::resizeEvent(QResizeEvent* event) {
    PropertyEditor::resizeEvent(event);
    updateHeight();
}

bool RelationshipTargetsEditor::event(QEvent* event) {
    // The pills' sizes change with the theme's font and the UI scale.
    if (event->type() == QEvent::LayoutRequest) updateHeight();
    return PropertyEditor::event(event);
}

}  // namespace usdcc::usd::properties
