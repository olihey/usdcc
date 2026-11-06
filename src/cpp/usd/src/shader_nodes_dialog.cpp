#include "usdcc/usd/shader_nodes_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/shader_node_catalog.h"

#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>

#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }

constexpr int kIdentifierRole = Qt::UserRole;
constexpr int kSystemRole = Qt::UserRole + 1;

void addNode(QTreeWidgetItem* parent, const ShaderNodeInfo& node) {
    const QString name = qs(node.variant.empty() ? node.label : node.variant);
    auto* item = new QTreeWidgetItem(parent, {name});
    item->setData(0, kIdentifierRole, qs(node.identifier));
    item->setData(0, kSystemRole, qs(node.shadingSystem));
    item->setToolTip(0, qs(node.identifier));
}

void addGroup(QTreeWidgetItem* parent, const ShaderNodeGroup& group) {
    auto* item = new QTreeWidgetItem(parent, {qs(group.name)});
    for (const ShaderNodeGroup& child : group.groups) addGroup(item, child);
    for (const ShaderNodeInfo& node : group.nodes) addNode(item, node);
}

std::string typeName(const SdrShaderProperty& property) {
    const SdfValueTypeName type = property.GetTypeAsSdfType().GetSdfType();
    return type ? type.GetAsToken().GetString() : property.GetType().GetString();
}

// Whether anything under it matches, hiding what doesn't.
bool filterItem(QTreeWidgetItem* item, const QString& text) {
    const bool self = item->text(0).contains(text, Qt::CaseInsensitive) ||
                      item->data(0, kIdentifierRole).toString().contains(text, Qt::CaseInsensitive);
    bool child = false;
    for (int i = 0; i < item->childCount(); ++i) {
        // A group that matches shows all it holds.
        if (self) {
            std::function<void(QTreeWidgetItem*)> show = [&show](QTreeWidgetItem* each) {
                each->setHidden(false);
                for (int j = 0; j < each->childCount(); ++j) show(each->child(j));
            };
            show(item->child(i));
        } else {
            child = filterItem(item->child(i), text) || child;
        }
    }
    item->setHidden(!self && !child);
    if (!text.isEmpty() && child) item->setExpanded(true);
    return self || child;
}
}  // namespace

ShaderNodesDialog::ShaderNodesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("shaderNodesDialog"));
    setWindowTitle(tr("Shader Nodes"));
    resize(usdcc::ui::scaled::size(860, 620));

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("propertyNote"));
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search names and identifiers"));
    m_search->setClearButtonEnabled(true);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_details->setPlaceholderText(tr("Pick a node to see its inputs and outputs."));

    auto* left = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_search);
    leftLayout->addWidget(m_tree, 1);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(left);
    split->addWidget(m_details);
    split->setStretchFactor(1, 1);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(split, 1);
    layout->addWidget(m_summary);

    connect(m_search, &QLineEdit::textChanged, this, &ShaderNodesDialog::applySearch);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() { showDetails(); });
    rebuild();
}

void ShaderNodesDialog::rebuild() {
    m_tree->clear();
    const ShaderNodeCatalog& catalog = ShaderNodeCatalog::instance();
    for (const ShaderNodeGroup& root : catalog.tree()) {
        auto* item = new QTreeWidgetItem(m_tree, {qs(root.name)});
        for (const ShaderNodeGroup& child : root.groups) addGroup(item, child);
        for (const ShaderNodeInfo& node : root.nodes) addNode(item, node);
        item->setExpanded(true);
    }
    const std::size_t all = catalog.allNodes().size();
    const std::size_t shown = catalog.nodes().size();
    m_summary->setText(tr("%1 nodes, and %2 hidden as lights or marked hidden.").arg(shown).arg(all - shown));
}

void ShaderNodesDialog::applySearch(const QString& text) {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) filterItem(m_tree->topLevelItem(i), text);
}

void ShaderNodesDialog::showDetails() {
    const QTreeWidgetItem* item = m_tree->currentItem();
    const QString identifier = item ? item->data(0, kIdentifierRole).toString() : QString();
    if (identifier.isEmpty()) {
        m_details->clear();
        return;
    }
    const SdrShaderNodeConstPtr node = SdrRegistry::GetInstance().GetShaderNodeByIdentifierAndType(
        TfToken(identifier.toStdString()), TfToken(item->data(0, kSystemRole).toString().toStdString()));
    if (!node) {
        m_details->setPlainText(tr("%1 isn't in the registry any more.").arg(identifier));
        return;
    }
    std::ostringstream text;
    text << node->GetIdentifier().GetString() << "\n";
    text << "Shading system: " << node->GetShadingSystem().GetString() << "\n";
    text << "Context in Sdr: " << node->GetContext().GetString() << "\n";
    if (const std::string help = node->GetHelp(); !help.empty()) text << "\n" << help << "\n";
    const auto list = [&text, &node](const char* title, const SdrTokenVec& names, bool inputs) {
        text << "\n" << title << " (" << names.size() << ")\n";
        for (const TfToken& name : names) {
            const SdrShaderPropertyConstPtr property = inputs ? node->GetShaderInput(name) : node->GetShaderOutput(name);
            if (!property) continue;
            text << "  " << typeName(*property) << " " << name.GetString();
            if (const std::string page = property->GetPage().GetString(); !page.empty()) text << "  [" << page << "]";
            text << "\n";
        }
    };
    list("Inputs", node->GetShaderInputNames(), true);
    list("Outputs", node->GetShaderOutputNames(), false);
    m_details->setPlainText(qs(text.str()));
}

}  // namespace usdcc::usd
