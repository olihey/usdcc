#include "usdcc/usd/shader_connection_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/shade_connections.h"

#include <pxr/usd/usd/stage.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace usdcc::usd {

namespace {
constexpr int kPathRole = Qt::UserRole;

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString typeText(const PXR_NS::UsdAttribute& attribute) { return qs(attribute.GetTypeName().GetAsToken().GetString()); }

// Shown greyed but, unlike a disabled item, without disabling the children: a
// node that can't be picked may hold nodes that can.
void markUnpickable(QTreeWidgetItem* item, const QString& why, const QPalette& palette) {
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    const QBrush grey = palette.brush(QPalette::Disabled, QPalette::Text);
    for (int column = 0; column < item->columnCount(); ++column) {
        item->setForeground(column, grey);
        item->setToolTip(column, why);
    }
}
}  // namespace

ShaderConnectionDialog::ShaderConnectionDialog(const PXR_NS::UsdAttribute& attribute, Mode mode, QWidget* parent)
    : QDialog(parent), m_attribute(attribute), m_mode(mode) {
    const QString name = qs(attribute.GetName().GetString());
    const QString type = typeText(attribute);
    setWindowTitle(mode == Mode::PickSource ? tr("Connect %1").arg(name) : tr("Connect %1 to\xE2\x80\xA6").arg(name));

    auto* layout = new QVBoxLayout(this);
    usdcc::ui::scaled::margins(layout, 8, 8, 8, 0);
    auto* hint = new QLabel(mode == Mode::PickSource
                                ? tr("Pick what %1 (%2) takes its value from.").arg(name, type)
                                : tr("Pick an unconnected %2 input to take its value from %1.").arg(name, type),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    m_nodes = new QTreeWidget(splitter);
    m_nodes->setObjectName(QStringLiteral("connectionNodes"));
    m_nodes->setHeaderLabels({tr("Node"), tr("Type")});
    m_nodes->header()->setStretchLastSection(false);
    m_nodes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_nodes->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_attributes = new QTreeWidget(splitter);
    m_attributes->setObjectName(QStringLiteral("connectionAttributes"));
    m_attributes->setHeaderLabels({tr("Attribute"), tr("Type")});
    m_attributes->setRootIsDecorated(false);
    m_attributes->header()->setStretchLastSection(false);
    m_attributes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_attributes->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto* buttons = new QDialogButtonBox(this);
    usdcc::ui::scaled::margins(buttons->layout(), 0, 6, 0, 8);
    m_connect = buttons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    m_connect->setDefault(true);
    m_connect->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_nodes, &QTreeWidget::itemSelectionChanged, this, &ShaderConnectionDialog::showAttributes);
    connect(m_attributes, &QTreeWidget::itemSelectionChanged, this, &ShaderConnectionDialog::updateConnectButton);
    connect(m_attributes, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->flags() & Qt::ItemIsEnabled) accept();
    });

    const PXR_NS::UsdPrim scope = connectionScope(attribute.GetPrim());
    if (scope) addNode(scope, nullptr);
    m_nodes->expandAll();

    // Start on the node of the current source, else on the first one that can be picked.
    QTreeWidgetItem* start = nullptr;
    if (mode == Mode::PickSource) {
        if (const PXR_NS::SdfPath source = connectedSource(attribute); !source.IsEmpty()) start = nodeItem(source.GetPrimPath());
    }
    for (QTreeWidgetItemIterator it(m_nodes); *it && !start; ++it) {
        if ((*it)->flags() & Qt::ItemIsSelectable) start = *it;
    }
    if (start) m_nodes->setCurrentItem(start);
    if (mode == Mode::PickSource && start) {
        if (const PXR_NS::SdfPath source = connectedSource(attribute); !source.IsEmpty()) {
            if (QTreeWidgetItem* item = attributeItem(PXR_NS::TfToken(source.GetName()))) m_attributes->setCurrentItem(item);
        }
    }
    resize(usdcc::ui::scaled::size(680, 440));
}

void ShaderConnectionDialog::addNode(const PXR_NS::UsdPrim& prim, QTreeWidgetItem* parent) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_nodes);
    item->setText(0, qs(prim.GetName().GetString()));
    item->setText(1, qs(prim.GetTypeName().GetString()));
    item->setIcon(0, prim_icons::icon(prim));
    item->setData(0, kPathRole, qs(prim.GetPath().GetString()));
    if (prim.GetPath() == m_attribute.GetPrim().GetPath()) {
        markUnpickable(item, tr("The prim being edited"), palette());
    } else if (candidates(prim).empty()) {
        markUnpickable(item,
                       m_mode == Mode::PickReceiver ? tr("No unconnected %1 input here").arg(typeText(m_attribute))
                                                    : tr("Nothing here %1 can take its value from").arg(qs(m_attribute.GetName())),
                       palette());
    }
    for (const PXR_NS::UsdPrim& child : prim.GetChildren()) addNode(child, item);
}

std::vector<std::pair<PXR_NS::UsdAttribute, std::string>> ShaderConnectionDialog::candidates(const PXR_NS::UsdPrim& node) const {
    std::vector<std::pair<PXR_NS::UsdAttribute, std::string>> result;
    for (const PXR_NS::UsdAttribute& candidate : connectableAttributes(node)) {
        if (m_mode == Mode::PickSource) {
            // Only what could be a source at all: other nodes' outputs, the inputs of the
            // materials and node graphs it is inside. Of those, the wrong type is greyed.
            if (whyNotPossibleSource(m_attribute, candidate)) continue;
            result.emplace_back(candidate, whyNotSource(m_attribute, candidate).value_or(std::string()));
        } else if (isReceiver(candidate) && connectedSource(candidate).IsEmpty() && !whyNotSource(candidate, m_attribute)) {
            result.emplace_back(candidate, std::string());
        }
    }
    return result;
}

void ShaderConnectionDialog::showAttributes() {
    m_attributes->clear();
    const QList<QTreeWidgetItem*> picked = m_nodes->selectedItems();
    if (!picked.isEmpty()) {
        const PXR_NS::SdfPath path(picked.front()->data(0, kPathRole).toString().toStdString());
        const PXR_NS::UsdPrim node = m_attribute.GetStage()->GetPrimAtPath(path);
        for (const auto& [candidate, why] : candidates(node)) {
            auto* item = new QTreeWidgetItem(m_attributes);
            item->setText(0, qs(candidate.GetName().GetString()));
            item->setText(1, typeText(candidate));
            item->setData(0, kPathRole, qs(candidate.GetPath().GetString()));
            if (!why.empty()) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                // Greyed explicitly: the theme's item colour applies to disabled items too.
                const QBrush grey = palette().brush(QPalette::Disabled, QPalette::Text);
                for (int column = 0; column < 2; ++column) {
                    item->setForeground(column, grey);
                    item->setToolTip(column, qs(why));
                }
            }
        }
    }
    updateConnectButton();
}

void ShaderConnectionDialog::updateConnectButton() { m_connect->setEnabled(!selected().IsEmpty()); }

PXR_NS::SdfPath ShaderConnectionDialog::selected() const {
    const QList<QTreeWidgetItem*> picked = m_attributes->selectedItems();
    if (picked.isEmpty() || !(picked.front()->flags() & Qt::ItemIsEnabled)) return {};
    return PXR_NS::SdfPath(picked.front()->data(0, kPathRole).toString().toStdString());
}

QTreeWidgetItem* ShaderConnectionDialog::nodeItem(const PXR_NS::SdfPath& prim) const {
    const QString wanted = qs(prim.GetString());
    for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) {
        if ((*it)->data(0, kPathRole).toString() == wanted) return *it;
    }
    return nullptr;
}

QTreeWidgetItem* ShaderConnectionDialog::attributeItem(const PXR_NS::TfToken& attribute) const {
    const QString wanted = qs(attribute.GetString());
    for (int i = 0; i < m_attributes->topLevelItemCount(); ++i) {
        if (m_attributes->topLevelItem(i)->text(0) == wanted) return m_attributes->topLevelItem(i);
    }
    return nullptr;
}

}  // namespace usdcc::usd
